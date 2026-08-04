// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: YM2151 (OPM) が X68000 のソフトから見て期待どおりに振る舞うこと。
//   - レジスタの書き込みと読み返し (アドレス latch → データの 2 段書き)
//   - ステータスの bit7 (BUSY) が常に落ちている
//     (IPL-ROM $FF9C9C の待ちループにタイムアウトが無いため、ここが命)
//   - キーオンでエンベロープがアタックへ入り、最大音量 (0) へ到達する
//   - キーオフでリリースへ入り、無音 (1023) まで落ちる
//   - 8 種のコネクション (アルゴリズム) がそれぞれ違う結線で音を出す
//   - キーオンが無ければ完全に無音
//   - レート変換してもエンベロープの実時間が保たれる

#include "dev/opm.h"
#include "doctest.h"

#include <cmath>

namespace
{

// レジスタへ 1 つ書く。実機と同じ 2 段 (アドレス → データ)。
void writeReg(x68k::Opm& opm, x68k::u8 reg, x68k::u8 value)
{
    opm.writeAddress(reg);
    opm.writeData(value);
}

// ch を「すぐ最大音量になり、鳴り続ける」設定にする。
// AR を最大、D1R/D2R を 0 (減衰なし)、TL を 0 (最大音量)。
void setupLoudChannel(x68k::Opm& opm, x68k::u8 ch, x68k::u8 connection = 7)
{
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        const x68k::u8 offset = static_cast<x68k::u8>(ch + slot * 8);
        writeReg(opm, static_cast<x68k::u8>(0x40 + offset), 0x01);  // DT1=0 MUL=1
        writeReg(opm, static_cast<x68k::u8>(0x60 + offset), 0x00);  // TL=0 (最大)
        writeReg(opm, static_cast<x68k::u8>(0x80 + offset), 0x1F);  // KS=0 AR=31
        writeReg(opm, static_cast<x68k::u8>(0xA0 + offset), 0x00);  // D1R=0
        writeReg(opm, static_cast<x68k::u8>(0xC0 + offset), 0x00);  // DT2=0 D2R=0
        writeReg(opm, static_cast<x68k::u8>(0xE0 + offset), 0x00);  // D1L=0 RR=0
    }
    // RL=両方on FL=0 CONN
    writeReg(opm, static_cast<x68k::u8>(0x20 + ch), static_cast<x68k::u8>(0xC0 | connection));
    writeReg(opm, static_cast<x68k::u8>(0x28 + ch), 0x4A);  // KC (オクターブ 4 の A)
    writeReg(opm, static_cast<x68k::u8>(0x30 + ch), 0x00);  // KF=0
}

// 全 4 スロットをキーオンする。
void keyOn(x68k::Opm& opm, x68k::u8 ch)
{
    writeReg(opm, 0x08, static_cast<x68k::u8>(0x78 | ch));
}

void keyOff(x68k::Opm& opm, x68k::u8 ch)
{
    writeReg(opm, 0x08, ch);
}

// 何サンプルか回して、出力の絶対値の最大を返す。
std::int32_t peakOver(x68k::Opm& opm, std::size_t frames)
{
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < frames; ++i)
    {
        const std::int32_t sample = opm.renderOneSample();
        const std::int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    return peak;
}

}  // namespace

TEST_CASE("レジスタはアドレス latch のあとデータで書かれる")
{
    x68k::Opm opm;
    opm.reset();

    // 実機は $E90001 にレジスタ番号、$E90003 に値を書く 2 段構え。
    opm.writeAddress(0x60);
    CHECK(opm.latchedAddress() == 0x60);
    opm.writeData(0x5A);
    CHECK(opm.peekRegister(0x60) == 0x5A);

    // アドレスは latch されたままなので、続けてデータだけ書くと同じ所へ入る。
    opm.writeData(0x33);
    CHECK(opm.peekRegister(0x60) == 0x33);
}

TEST_CASE("ステータスの BUSY は常に落ちている")
{
    x68k::Opm opm;
    opm.reset();

    // IPL-ROM $FF9C9C の TST.B $E90003 / BMI.S はタイムアウトを持たない。
    // bit7 が立ったままだと起動がここで永久に止まる。
    CHECK((opm.readStatus() & 0x80u) == 0);

    // 書き込み直後も同じ。実チップは一時的に BUSY を立てるが、
    // 落とし損ねる危険を避けて常に 0 にしてある。
    writeReg(opm, 0x1B, 0x00);
    CHECK((opm.readStatus() & 0x80u) == 0);
}

TEST_CASE("リセット直後は完全に無音")
{
    x68k::Opm opm;
    opm.reset();

    CHECK(opm.isSilent());

    std::int16_t buffer[256] = {};
    opm.renderSamples(buffer, 256);
    for (const std::int16_t sample : buffer)
    {
        CHECK(sample == 0);
    }
}

TEST_CASE("キーオンしていない ch は音を出さない")
{
    x68k::Opm opm;
    opm.reset();

    // 音色だけ設定してキーオンしない。音色設定は音を出さない。
    setupLoudChannel(opm, 0);
    CHECK(opm.isSilent());
    CHECK(peakOver(opm, 512) == 0);
}

TEST_CASE("キーオン直後の renderSamples が音を出す")
{
    // renderSamples は無音のとき合成を省く早期リターンを持つ。
    // キーオン直後はエンベロープがまだ 1023 (無音) なので、そこを
    // 「無音」と判定すると音が一度も鳴り始めない。
    // renderOneSample を先に呼んで暖めると隠れてしまうため、
    // キーオンしてすぐ renderSamples だけを叩く。
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    keyOn(opm, 0);

    // まだ 1 サンプルも合成していない状態。
    CHECK(opm.isSilent() == false);

    std::int16_t buffer[512] = {};
    opm.renderSamples(buffer, 512);

    int nonZero = 0;
    for (const std::int16_t sample : buffer)
    {
        if (sample != 0)
        {
            ++nonZero;
        }
    }
    CHECK(nonZero > 0);
}

TEST_CASE("キーオンでアタックへ入り最大音量へ到達する")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0);

    // キーオン前は Off。
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kOff);
    CHECK(opm.envelopeLevel(0, 0) == 1023);

    keyOn(opm, 0);
    CHECK(opm.isKeyOn(0));
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kAttack);

    // AR=31 は最速。数サンプルで最大音量 (エンベロープ 0) へ届く。
    opm.renderSamples(nullptr, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(opm.envelopeLevel(0, 0) == 0);
    // アタックが終わればディケイへ移る。
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kDecay);
    CHECK(opm.isSilent() == false);
}

TEST_CASE("キーオフでリリースへ入り無音まで落ちる")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0);
    keyOn(opm, 0);

    // まず鳴らしきる。
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(opm.envelopeLevel(0, 0) == 0);

    // RR を最大にしてからキーオフする。速く落ちること。
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        writeReg(opm, static_cast<x68k::u8>(0xE0 + slot * 8), 0x0F);  // D1L=0 RR=15
    }
    keyOff(opm, 0);
    CHECK(opm.isKeyOn(0) == false);
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kRelease);

    // リリースが進めば必ず無音へ届く。
    for (int i = 0; i < 4096; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(opm.envelopeLevel(0, 0) == 1023);
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kOff);
    CHECK(opm.isSilent());
}

TEST_CASE("D1L に達したらサステインへ移る")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0);

    // D1R を動かし、D1L を途中に置く。
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        writeReg(opm, static_cast<x68k::u8>(0xA0 + slot * 8), 0x1F);  // D1R=31
        writeReg(opm, static_cast<x68k::u8>(0xE0 + slot * 8), 0x40);  // D1L=4 RR=0
    }
    keyOn(opm, 0);

    for (int i = 0; i < 512; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    // D1L=4 は EG レベル 128 に相当する。そこを越えたらサステインへ。
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kSustain);
    CHECK(opm.envelopeLevel(0, 0) >= 128);
}

TEST_CASE("AR=0 のスロットは鳴らない")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0);

    // AR=0 は「変化しない」。キーオンしても最小音量のまま。
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        writeReg(opm, static_cast<x68k::u8>(0x80 + slot * 8), 0x00);
    }
    keyOn(opm, 0);

    for (int i = 0; i < 1024; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(opm.envelopeLevel(0, 0) == 1023);
    CHECK(peakOver(opm, 256) == 0);
}

TEST_CASE("レート 0 のフェーズではエンベロープが動かない")
{
    // OPM のエンベロープはレート 0 を「変化しない」と定める。
    // D2R=0 のサステインは、その音量のまま鳴り続ける (音を伸ばす基本の手)。
    // ここでレートの 0 判定を落とすと、伸ばしたい音が勝手に減衰する。
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    // D1R=31 で速やかに D1L まで落とし、そこから D2R=0 で止める。
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        writeReg(opm, static_cast<x68k::u8>(0xA0 + slot * 8), 0x1F);  // D1R=31
        writeReg(opm, static_cast<x68k::u8>(0xC0 + slot * 8), 0x00);  // D2R=0
        writeReg(opm, static_cast<x68k::u8>(0xE0 + slot * 8), 0x40);  // D1L=4
    }
    keyOn(opm, 0);

    for (int i = 0; i < 1024; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kSustain);
    const x68k::u32 sustained = opm.envelopeLevel(0, 0);

    // 止まる位置は D1L=4 の折り返し点の少し先。無音 (1023) でも
    // 最大音量 (0) でもない、途中の値で止まっていることを確かめる。
    // ここが 1023 なら「レート 0 なのに減衰し続けた」ということ。
    CHECK(sustained > 0);
    CHECK(sustained < 1023);

    // D2R=0 なので、ここから先はいくら回しても 1 も動かない。
    for (int i = 0; i < 20000; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(opm.envelopeLevel(0, 0) == sustained);
    CHECK(opm.isSilent() == false);
}

TEST_CASE("AR=0 はキースケーリングが効く設定でも鳴らない")
{
    // レート 0 は「変化しない」であって、キースケーリングを足して
    // 0 でなくしてよい値ではない。KS=3 かつ高い KC だと、キースケール分
    // (KC>>0) が大きいので、レート 0 の判定を落とすと 0 のはずのレートが
    // 非ゼロになり、AR=0 なのにアタックが進んでしまう。
    //
    // KS=0 では足す量が小さく差が出ないので、この組み合わせでしか捕まらない。
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        // KS=3 (最大)、AR=0。
        writeReg(opm, static_cast<x68k::u8>(0x80 + slot * 8), 0xC0);
    }
    // 高いキーコード。キースケール量を最大にする。
    writeReg(opm, 0x28, 0x7A);
    keyOn(opm, 0);

    for (int i = 0; i < 4096; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }

    // アタックのまま、最小音量から一歩も動かないこと。
    CHECK(opm.envelopeLevel(0, 0) == 1023);
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kAttack);
    CHECK(peakOver(opm, 256) == 0);
}

TEST_CASE("TL が音量を下げる")
{
    x68k::Opm opm;
    opm.reset();

    setupLoudChannel(opm, 0, 7);  // アルゴリズム 7 = 4 つ全部が出力
    keyOn(opm, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    const std::int32_t loudPeak = peakOver(opm, 1024);
    CHECK(loudPeak > 0);

    // TL を上げる (= 音を小さくする)。
    x68k::Opm quiet;
    quiet.reset();
    setupLoudChannel(quiet, 0, 7);
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        writeReg(quiet, static_cast<x68k::u8>(0x60 + slot * 8), 0x40);  // TL=64
    }
    keyOn(quiet, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(quiet.renderOneSample());
    }
    const std::int32_t quietPeak = peakOver(quiet, 1024);

    // TL=64 は約 48dB の減衰。はっきり小さくなること。
    CHECK(quietPeak < loudPeak);
}

TEST_CASE("TL を最大にすると無音になる")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        writeReg(opm, static_cast<x68k::u8>(0x60 + slot * 8), 0x7F);  // TL=127
    }
    keyOn(opm, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    // TL=127 は最大減衰。対数領域でテーブルの外まで行くので完全な 0。
    CHECK(peakOver(opm, 1024) == 0);
}

TEST_CASE("8 種のコネクションすべてが音を出す")
{
    // アルゴリズムごとに結線が違うが、どれも「キャリアが 1 つ以上ある」ので
    // 音は出なければならない。出ないアルゴリズムがあれば結線ミス。
    for (x68k::u8 connection = 0; connection < 8; ++connection)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, connection);
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        const std::int32_t peak = peakOver(opm, 2048);
        INFO("connection = ", static_cast<int>(connection));
        CHECK(peak > 0);
    }
}

TEST_CASE("加算アルゴリズムは直列アルゴリズムより大きい")
{
    // アルゴリズム 7 は 4 オペレータをそのまま加算する。
    // アルゴリズム 0 は 4 段直列で、出力するのは最終段の C2 だけ。
    // 同じ音色設定なら 7 の方が振幅が大きくなる。ここが逆なら
    // 結線 (どれをキャリアとして足すか) が間違っている。
    x68k::Opm additive;
    additive.reset();
    setupLoudChannel(additive, 0, 7);
    keyOn(additive, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(additive.renderOneSample());
    }
    const std::int32_t additivePeak = peakOver(additive, 2048);

    x68k::Opm serial;
    serial.reset();
    setupLoudChannel(serial, 0, 0);
    keyOn(serial, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(serial.renderOneSample());
    }
    const std::int32_t serialPeak = peakOver(serial, 2048);

    CHECK(additivePeak > serialPeak);
}

TEST_CASE("直列アルゴリズムでは変調器を黙らせても音が残る")
{
    // アルゴリズム 0 (M1→C1→M2→C2) で、最終段の C2 以外の TL を最大にする。
    // C2 だけが出力なので、音は残るが変調が消えて純音に近くなる。
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 0);
    // レジスタ上のスロット並びは M1(0) / C1(1) / M2(2) / C2(3)。
    // アルゴリズム 0 の出力は C2 = スロット 3。
    writeReg(opm, 0x60, 0x7F);  // M1 を無音に
    writeReg(opm, 0x68, 0x7F);  // C1 を無音に
    writeReg(opm, 0x70, 0x7F);  // M2 を無音に
    keyOn(opm, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    // 出力段が生きているので音は出る。
    CHECK(peakOver(opm, 2048) > 0);
}

TEST_CASE("直列アルゴリズムで出力段を黙らせると無音になる")
{
    // 逆に、アルゴリズム 0 で C2 (スロット 3) だけ黙らせると全体が無音。
    // ここで音が残るなら、変調器が誤って出力へ足されている。
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 0);
    writeReg(opm, 0x78, 0x7F);  // C2 (スロット 3) を無音に
    keyOn(opm, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(peakOver(opm, 2048) == 0);
}

TEST_CASE("各アルゴリズムのキャリアが漏れなく出力へ足される")
{
    // 「音が出るか」だけを見ると、キャリアを 1 つ足し忘れていても
    // 残りが鳴るので気付けない。そこでキャリアを 1 つずつ単独で残し
    // (他の 3 つを TL=127 で黙らせ)、そのそれぞれが単独で音を出すことを見る。
    // 足し忘れているキャリアがあれば、それを単独で残したときだけ無音になる。
    //
    // レジスタ上のスロット並びは M1(0) / C1(1) / M2(2) / C2(3)。
    // アルゴリズムごとの「出力へ足されるスロット」は YM2151 の
    // アルゴリズム図のとおり。
    struct AlgorithmCarriers
    {
        x68k::u8 connection;
        int carriers[4];  // 出力へ足されるスロット。-1 で終端
    };
    const AlgorithmCarriers table[] = {
        {0, {3, -1, -1, -1}},  // M1→C1→M2→C2、出力は C2 のみ
        {1, {3, -1, -1, -1}},  // (M1+C1)→M2→C2
        {2, {3, -1, -1, -1}},  // (M1+M2)→C2
        {3, {3, -1, -1, -1}},  // (C1+M2)→C2
        {4, {1, 3, -1, -1}},   // M1→C1 と M2→C2 の 2 系列
        {5, {1, 2, 3, -1}},    // M1 が C1/M2/C2 を変調し 3 つを加算
        {6, {1, 2, 3, -1}},    // M1→C1 と、M2・C2 をそのまま加算
        {7, {0, 1, 2, 3}},     // 4 つすべてを加算
    };

    for (const AlgorithmCarriers& entry : table)
    {
        for (int index = 0; index < 4; ++index)
        {
            const int carrier = entry.carriers[index];
            if (carrier < 0)
            {
                break;
            }

            x68k::Opm opm;
            opm.reset();
            setupLoudChannel(opm, 0, entry.connection);
            // 対象のキャリア以外をすべて黙らせる。
            for (x68k::u8 slot = 0; slot < 4; ++slot)
            {
                if (static_cast<int>(slot) == carrier)
                {
                    continue;
                }
                writeReg(opm, static_cast<x68k::u8>(0x60 + slot * 8), 0x7F);
            }
            keyOn(opm, 0);
            for (int i = 0; i < 64; ++i)
            {
                static_cast<void>(opm.renderOneSample());
            }

            INFO("connection = ", static_cast<int>(entry.connection), " carrier slot = ", carrier);
            // このキャリアが出力へ足されていれば単独でも音が出る。
            CHECK(peakOver(opm, 2048) > 0);
        }
    }
}

TEST_CASE("複数の変調器を持つアルゴリズムは全部を足して変調する")
{
    // アルゴリズム 1/2/3 は 2 つの変調器の和がキャリアを変調する。
    //   1: (M1 + C1) → M2 → C2
    //   2: (M1 + M2) → C2      (M2 は C1 に変調されている)
    //   3: (C1 + M2) → C2      (C1 は M1 に変調されている)
    // 片方しか足していないと、その片方を黙らせたときに音色が変わらなくなる。
    // キャリアは常に鳴るので「音が出るか」では捕まらない。変調が効くと
    // 倍音が増えてゼロ交差が増えるので、そこで見る。
    //
    // レジスタ上のスロット並びは M1(0) / C1(1) / M2(2) / C2(3)。
    struct ModulatorPair
    {
        x68k::u8 connection;
        x68k::u8 slots[2];  // 和になる 2 つの変調器のスロット
    };
    const ModulatorPair table[] = {
        {1, {0, 1}},  // M1 と C1
        {2, {0, 2}},  // M1 と M2
        {3, {1, 2}},  // C1 と M2
    };

    auto crossings = [](x68k::u8 connection, int silencedSlot)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, connection);
        if (silencedSlot >= 0)
        {
            writeReg(opm, static_cast<x68k::u8>(0x60 + silencedSlot * 8), 0x7F);
        }
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        int count = 0;
        std::int16_t previous = opm.renderOneSample();
        for (int i = 0; i < 4096; ++i)
        {
            const std::int16_t current = opm.renderOneSample();
            if ((previous < 0) != (current < 0))
            {
                ++count;
            }
            previous = current;
        }
        return count;
    };

    for (const ModulatorPair& entry : table)
    {
        const int both = crossings(entry.connection, -1);
        for (const x68k::u8 slot : entry.slots)
        {
            INFO("connection = ", static_cast<int>(entry.connection),
                 " silenced slot = ", static_cast<int>(slot));
            // その変調器を黙らせたら波形が変わること
            // (= その変調器が和に寄与している)。
            CHECK(both != crossings(entry.connection, static_cast<int>(slot)));
        }
    }
}

TEST_CASE("非キャリアのスロットは単独では音を出さない")
{
    // 上の裏返し。変調器だけを残してキャリアを全部黙らせたら無音になる。
    // 変調器が誤って出力へ足されていればここで鳴ってしまう。
    // アルゴリズム 0 (出力は C2 = スロット 3) で、M1/C1/M2 を 1 つずつ残す。
    for (x68k::u8 modulator = 0; modulator < 3; ++modulator)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, 0);
        for (x68k::u8 slot = 0; slot < 4; ++slot)
        {
            if (slot == modulator)
            {
                continue;
            }
            writeReg(opm, static_cast<x68k::u8>(0x60 + slot * 8), 0x7F);
        }
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        INFO("modulator slot = ", static_cast<int>(modulator));
        CHECK(peakOver(opm, 2048) == 0);
    }
}

TEST_CASE("アルゴリズム 4 は 2 系列の和になる")
{
    // アルゴリズム 4 は M1→C1 と M2→C2 の 2 系列。出力は C1 と C2。
    // C1 (スロット 1) だけ黙らせても、C2 の系列が残るので音は出る。
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 4);
    writeReg(opm, 0x68, 0x7F);  // C1 を無音に
    keyOn(opm, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(peakOver(opm, 2048) > 0);

    // 両方のキャリアを黙らせたら無音。
    x68k::Opm muted;
    muted.reset();
    setupLoudChannel(muted, 0, 4);
    writeReg(muted, 0x68, 0x7F);  // C1
    writeReg(muted, 0x78, 0x7F);  // C2
    keyOn(muted, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(muted.renderOneSample());
    }
    CHECK(peakOver(muted, 2048) == 0);
}

TEST_CASE("パンが両方 off の ch は出力されない")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    // RL の bit7/bit6 を両方 0 にする。実機でも音は出ない。
    writeReg(opm, 0x20, 0x07);
    keyOn(opm, 0);
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    CHECK(peakOver(opm, 1024) == 0);
    // ただしエンベロープは進む (実機も同じ。パンは出力段の話)。
    CHECK(opm.isSilent() == false);
}

TEST_CASE("8ch すべてが独立に鳴る")
{
    for (x68k::u8 ch = 0; ch < 8; ++ch)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, ch, 7);
        keyOn(opm, ch);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }
        INFO("channel = ", static_cast<int>(ch));
        CHECK(peakOver(opm, 1024) > 0);
        CHECK(opm.isKeyOn(ch));
        // 他の ch は鳴っていない。
        const x68k::u8 other = static_cast<x68k::u8>((ch + 1) & 7);
        CHECK(opm.isKeyOn(other) == false);
    }
}

TEST_CASE("キーオンはスロット単位で効く")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);

    // レジスタ $08 の bit3 だけ立てる = スロット 0 (M1) のみキーオン。
    writeReg(opm, 0x08, 0x08);
    CHECK(opm.envelopePhase(0, 0) == x68k::Opm::EgPhase::kAttack);
    CHECK(opm.envelopePhase(0, 1) == x68k::Opm::EgPhase::kOff);
    CHECK(opm.envelopePhase(0, 2) == x68k::Opm::EgPhase::kOff);
    CHECK(opm.envelopePhase(0, 3) == x68k::Opm::EgPhase::kOff);

    // bit6 (スロット 3) を足す。
    writeReg(opm, 0x08, 0x48);
    CHECK(opm.envelopePhase(0, 3) == x68k::Opm::EgPhase::kAttack);
}

TEST_CASE("キーオンで位相がリセットされる")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    keyOn(opm, 0);

    // 最初の数サンプルを覚える。
    std::int16_t first[8] = {};
    for (int i = 0; i < 8; ++i)
    {
        first[i] = opm.renderOneSample();
    }

    // しばらく回してから、キーオフ → キーオンし直す。
    for (int i = 0; i < 500; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    keyOff(opm, 0);
    keyOn(opm, 0);

    // 位相がリセットされるので、同じ波形が出るはず。
    // (エンベロープも 1023 から始まり直すので厳密には同じにならないが、
    //  位相が残っていれば最初のサンプルの符号が変わる可能性が高い)
    std::int16_t second[8] = {};
    for (int i = 0; i < 8; ++i)
    {
        second[i] = opm.renderOneSample();
    }
    // 立ち上がりの符号が一致すること。位相がリセットされない実装だと
    // 再キーオンのたびに波形の途中から始まり、ここが崩れる。
    CHECK((first[1] >= 0) == (second[1] >= 0));
}

TEST_CASE("MUL が音程を変える")
{
    // MUL=1 と MUL=2 では出力の周期が 2 倍違う。
    // ゼロ交差の回数を数えれば周波数の比が見える。
    auto countZeroCrossings = [](x68k::u8 multiple)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, 7);
        for (x68k::u8 slot = 0; slot < 4; ++slot)
        {
            writeReg(opm, static_cast<x68k::u8>(0x40 + slot * 8), multiple);
        }
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        int crossings = 0;
        std::int16_t previous = opm.renderOneSample();
        for (int i = 0; i < 4096; ++i)
        {
            const std::int16_t current = opm.renderOneSample();
            if ((previous < 0) != (current < 0))
            {
                ++crossings;
            }
            previous = current;
        }
        return crossings;
    };

    const int mul1 = countZeroCrossings(1);
    const int mul2 = countZeroCrossings(2);

    CHECK(mul1 > 0);
    // MUL=2 は倍の周波数。ゼロ交差もおおよそ倍になる。
    CHECK(mul2 > mul1);
}

TEST_CASE("DT2 が周波数をずらす")
{
    // DT2 は 4 段階の粗いデチューンで、位相増分そのものに掛かる倍率
    // (1.0 / 1.41 / 1.57 / 1.73 倍)。DT2 を無視すると 4 段とも同じ音になり、
    // DT2 を使った音色 (金属的な響き) が作れなくなる。
    auto crossings = [](x68k::u8 detune2)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, 7);
        for (x68k::u8 slot = 0; slot < 4; ++slot)
        {
            // DT2 は $C0 の bit7-6。D2R は 0 のまま。
            writeReg(opm, static_cast<x68k::u8>(0xC0 + slot * 8),
                     static_cast<x68k::u8>(detune2 << 6));
        }
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        int count = 0;
        std::int16_t previous = opm.renderOneSample();
        for (int i = 0; i < 8192; ++i)
        {
            const std::int16_t current = opm.renderOneSample();
            if ((previous < 0) != (current < 0))
            {
                ++count;
            }
            previous = current;
        }
        return count;
    };

    const int detune0 = crossings(0);
    const int detune3 = crossings(3);
    CHECK(detune0 > 0);
    // DT2=3 は約 1.73 倍。はっきり高くなること。
    CHECK(detune3 > detune0);
}

TEST_CASE("MUL=0 は半分の周波数になる")
{
    // MUL は 1-15 がそのままの倍率だが、0 だけは 0.5 倍という例外。
    // ここを「0 倍」や「1 倍」にすると、MUL=0 の音色が 1 オクターブずれる。
    auto crossings = [](x68k::u8 multiple)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, 7);
        for (x68k::u8 slot = 0; slot < 4; ++slot)
        {
            writeReg(opm, static_cast<x68k::u8>(0x40 + slot * 8), multiple);
        }
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        int count = 0;
        std::int16_t previous = opm.renderOneSample();
        for (int i = 0; i < 8192; ++i)
        {
            const std::int16_t current = opm.renderOneSample();
            if ((previous < 0) != (current < 0))
            {
                ++count;
            }
            previous = current;
        }
        return count;
    };

    const int half = crossings(0);
    const int single = crossings(1);

    // MUL=0 でも音は出る (0 倍にして無音にしてはいけない)。
    CHECK(half > 0);
    // MUL=1 のちょうど半分あたりになる。
    const double ratio = static_cast<double>(single) / static_cast<double>(half);
    CHECK(ratio > 1.7);
    CHECK(ratio < 2.3);
}

TEST_CASE("キースケーリングは高い音ほどエンベロープを速くする")
{
    // KS はキーコードに応じてエンベロープのレートを上げる。
    // KS=3 (最大) なら、高い音ほどリリースが速く終わるはず。
    // KS を無視する実装だと、この差が消える。
    auto releaseSamples = [](x68k::u8 keyCode, x68k::u8 keyScale)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, 7);
        for (x68k::u8 slot = 0; slot < 4; ++slot)
        {
            // KS を指定し、AR は最大のままにする。
            writeReg(opm, static_cast<x68k::u8>(0x80 + slot * 8),
                     static_cast<x68k::u8>((keyScale << 6) | 0x1F));
            writeReg(opm, static_cast<x68k::u8>(0xE0 + slot * 8), 0x02);  // RR=2
        }
        writeReg(opm, 0x28, keyCode);
        keyOn(opm, 0);
        for (int i = 0; i < 128; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }
        keyOff(opm, 0);

        int samples = 0;
        while (samples < 200000 && opm.envelopeLevel(0, 0) < 1023)
        {
            static_cast<void>(opm.renderOneSample());
            ++samples;
        }
        return samples;
    };

    // KS=3 で、低い音 (オクターブ 1) と高い音 (オクターブ 7) を比べる。
    const int lowNote = releaseSamples(0x1A, 3);
    const int highNote = releaseSamples(0x7A, 3);
    CHECK(lowNote > 0);
    // 高い音の方が速く減衰しきる。
    CHECK(highNote < lowNote);

    // KS そのものの効き方も見る。KS はキーコードを (3 - KS) だけ右シフトして
    // 足すので、同じ音でも KS が大きいほど速い。
    //
    // Why not 音程の高低だけで足りるとしないか: シフト量を (3 - KS) ではなく
    // 固定の 3 にする取り違えをしても、高い音ほど速いという関係は保たれる
    // (KC が大きければシフト後も大きい)。KS を変えたときの差を見て初めて
    // 「KS がレートに効いているか」を確かめられる。
    const int sameNoteKs0 = releaseSamples(0x4A, 0);
    const int sameNoteKs3 = releaseSamples(0x4A, 3);
    // KS=3 の方が速く減衰しきる。
    CHECK(sameNoteKs3 < sameNoteKs0);
}

TEST_CASE("KC が高いほど周波数が高い")
{
    auto countZeroCrossings = [](x68k::u8 keyCode)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, 7);
        writeReg(opm, 0x28, keyCode);
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        int crossings = 0;
        std::int16_t previous = opm.renderOneSample();
        for (int i = 0; i < 4096; ++i)
        {
            const std::int16_t current = opm.renderOneSample();
            if ((previous < 0) != (current < 0))
            {
                ++crossings;
            }
            previous = current;
        }
        return crossings;
    };

    // オクターブが 1 つ上がれば周波数は 2 倍。
    const int octave3 = countZeroCrossings(0x3A);
    const int octave4 = countZeroCrossings(0x4A);
    CHECK(octave3 > 0);
    CHECK(octave4 > octave3);
}

TEST_CASE("出力レートを変えてもエンベロープの実時間が保たれる")
{
    // アタックからリリース完了までにかかる「秒数」がレートに依らないこと。
    // ここが崩れると、レートを変えたときに音の長さが変わる。
    auto releaseSeconds = [](x68k::u32 rate)
    {
        x68k::Opm opm;
        opm.reset();
        opm.setSampleRate(rate);
        setupLoudChannel(opm, 0, 7);
        // RR をほどほどに設定して、測れる長さにする。
        for (x68k::u8 slot = 0; slot < 4; ++slot)
        {
            writeReg(opm, static_cast<x68k::u8>(0xE0 + slot * 8), 0x05);  // RR=5
        }
        keyOn(opm, 0);
        for (int i = 0; i < 128; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }
        keyOff(opm, 0);

        int samples = 0;
        const int limit = static_cast<int>(rate);  // 最大 1 秒ぶん
        while (samples < limit && opm.envelopeLevel(0, 0) < 1023)
        {
            static_cast<void>(opm.renderOneSample());
            ++samples;
        }
        return static_cast<double>(samples) / static_cast<double>(rate);
    };

    const double atDefault = releaseSeconds(15625);
    const double atHigh = releaseSeconds(31250);

    CHECK(atDefault > 0.0);
    CHECK(atHigh > 0.0);
    // レートが 2 倍でも実時間はほぼ同じ。20% 以内で一致すること。
    const double ratio = atHigh / atDefault;
    CHECK(ratio > 0.8);
    CHECK(ratio < 1.25);
}

TEST_CASE("サンプルレートを変えても音程は変わらない")
{
    // 位相増分はサンプルレートに依存する形でキャッシュしてある。
    // setSampleRate がキャッシュを張り直さないと、レート変更後も
    // 前のレート用の増分で鳴り続けて音程がずれる。
    // 「1 秒あたりのゼロ交差数」= 周波数がレートに依らないことで見る。
    auto crossingsPerSecond = [](x68k::u32 rate, bool changeRateAfterSetup)
    {
        x68k::Opm opm;
        opm.reset();
        if (!changeRateAfterSetup)
        {
            opm.setSampleRate(rate);
        }
        setupLoudChannel(opm, 0, 7);
        if (changeRateAfterSetup)
        {
            // 音色を決めた後でレートを変える。キャッシュの張り直しが要る経路。
            opm.setSampleRate(rate);
        }
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }

        const int frames = static_cast<int>(rate) / 4;  // 0.25 秒ぶん
        int crossings = 0;
        std::int16_t previous = opm.renderOneSample();
        for (int i = 0; i < frames; ++i)
        {
            const std::int16_t current = opm.renderOneSample();
            if ((previous < 0) != (current < 0))
            {
                ++crossings;
            }
            previous = current;
        }
        return static_cast<double>(crossings) * 4.0;
    };

    const double base = crossingsPerSecond(15625, false);
    const double doubled = crossingsPerSecond(31250, false);
    CHECK(base > 0.0);
    // レートが 2 倍でも同じ音程 (= 同じ周波数) のはず。10% 以内で一致すること。
    CHECK(doubled / base > 0.9);
    CHECK(doubled / base < 1.1);

    // 音色を決めた後にレートを変えても同じ音程であること。
    const double changedLater = crossingsPerSecond(31250, true);
    CHECK(changedLater / base > 0.9);
    CHECK(changedLater / base < 1.1);
}

TEST_CASE("レートに 0 を渡しても壊れない")
{
    x68k::Opm opm;
    opm.reset();
    opm.setSampleRate(0);
    // 既定へ落ちること。ゼロ除算しない。
    CHECK(opm.sampleRate() == x68k::Opm::kDefaultSampleRate);
}

TEST_CASE("renderSamples が nullptr で落ちない")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    keyOn(opm, 0);
    // platform 層が誤って null を渡しても落ちないこと。
    opm.renderSamples(nullptr, 128);
    CHECK(true);
}

TEST_CASE("無音のときの renderSamples は 0 で埋める")
{
    x68k::Opm opm;
    opm.reset();

    std::int16_t buffer[64];
    for (std::int16_t& sample : buffer)
    {
        sample = 12345;  // 汚しておく
    }
    opm.renderSamples(buffer, 64);
    for (const std::int16_t sample : buffer)
    {
        CHECK(sample == 0);
    }
}

TEST_CASE("出力が 16bit の範囲に収まる")
{
    // 8ch すべてを最大音量でキーオンし、飽和が効いていることを確かめる。
    x68k::Opm opm;
    opm.reset();
    for (x68k::u8 ch = 0; ch < 8; ++ch)
    {
        setupLoudChannel(opm, ch, 7);
        keyOn(opm, ch);
    }
    for (int i = 0; i < 64; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }

    // renderOneSample の戻り値は int16_t なので型で保証されるが、
    // 内部の飽和が効かず折り返していれば波形が壊れる。
    // 連続するサンプルの差が極端に大きくならないことで見る。
    std::int16_t previous = opm.renderOneSample();
    for (int i = 0; i < 2048; ++i)
    {
        const std::int16_t current = opm.renderOneSample();
        const std::int32_t delta = static_cast<std::int32_t>(current) - previous;
        // 折り返しが起きると +32767 -> -32768 のような 65535 の跳びが出る。
        CHECK(std::abs(delta) < 65000);
        previous = current;
    }
}

TEST_CASE("フィードバックが波形を変える")
{
    auto peakWithFeedback = [](x68k::u8 feedback)
    {
        x68k::Opm opm;
        opm.reset();
        setupLoudChannel(opm, 0, 0);  // 直列。M1 のフィードバックが効く
        writeReg(opm, 0x20, static_cast<x68k::u8>(0xC0 | (feedback << 3) | 0));
        keyOn(opm, 0);
        for (int i = 0; i < 64; ++i)
        {
            static_cast<void>(opm.renderOneSample());
        }
        std::int32_t sum = 0;
        for (int i = 0; i < 1024; ++i)
        {
            const std::int32_t sample = opm.renderOneSample();
            sum += sample < 0 ? -sample : sample;
        }
        return sum;
    };

    // FL=0 (フィードバック無し) と FL=7 (最大) で波形が変わること。
    CHECK(peakWithFeedback(0) != peakWithFeedback(7));

    // FL は 2^(10-FL) で割る量なので、段ごとに深さが変わる。
    // シフト量を FL に依らない固定値にすると、FL=1 から 7 まで全部同じ音に
    // なる。中間の段どうしも違うことを確かめて、そこを捕まえる。
    CHECK(peakWithFeedback(1) != peakWithFeedback(7));
    CHECK(peakWithFeedback(3) != peakWithFeedback(6));
}

TEST_CASE("キーオンを重ねてもアタックはやり直されない")
{
    x68k::Opm opm;
    opm.reset();
    setupLoudChannel(opm, 0, 7);
    keyOn(opm, 0);
    for (int i = 0; i < 32; ++i)
    {
        static_cast<void>(opm.renderOneSample());
    }
    const x68k::u32 levelAfterAttack = opm.envelopeLevel(0, 0);

    // 同じキーオンをもう一度書く。実チップは立ち上がりでしか反応しない。
    keyOn(opm, 0);
    CHECK(opm.envelopeLevel(0, 0) == levelAfterAttack);
}
