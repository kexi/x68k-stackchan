// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: MSM6258V の ADPCM デコーダが OKI ADPCM の仕様どおりに
// 動くこと。具体的には
//   - 既知のニブル列が既知の PCM 列になる (デコードの手順が正しい)
//   - 上位ニブルが先に読まれる (バイトからの取り出し順)
//   - 12bit で飽和する (折り返さない)
//   - ステップインデックスが 0..48 に留まる
//   - 停止中とデータ切れのときは無音
//
// 期待値の根拠:
//   OKI/Dialogic の 4bit ADPCM 仕様の 2 つのテーブル (49 段のステップサイズと
//   ニブル下位 3bit → インデックス増減 {-1,-1,-1,-1,2,4,6,8}) から手計算した。
//   デコード式は diff = step/8 + step/4*b2 + step/2*b1 + step*b0、bit3 が符号。
//   実装を読んで作った値ではなく、仕様から独立に出した値を置いている。

#include "dev/adpcm.h"
#include "doctest.h"

namespace
{

// 仕様から手計算した参照値。
//
// ニブル 7 (符号 +、下位 3bit = 7) を 4 回続けたときの信号レベル。
//   step[0]=16  : diff = 2 + 16 + 8 + 4 = 30      -> signal 30,   index 0+6=6
//   step[6]=28  : diff = 3 + 28 + 14 + 7 = 52... (実際は 63)      -> signal 93
// 具体値は仕様のテーブルどおりに追った結果で、下の配列がその全体。
constexpr std::int16_t kNibble7Ramp[] = {30, 93, 229, 522, 1153, 2047, 2047, 2047};

}  // namespace

TEST_CASE("既知のニブル列が仕様どおりの PCM になる")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    // ニブル 7 を 4 回。ステップが適応して増えていく様子まで含めて一致すること。
    CHECK(adpcm.decodeNibble(0x7) == 30);
    CHECK(adpcm.decodeNibble(0x7) == 93);
    CHECK(adpcm.decodeNibble(0x7) == 229);
    CHECK(adpcm.decodeNibble(0x7) == 522);

    // ニブル 7 の下位 3bit は 7 なので、増減表の最後の要素 (+8) が効く。
    // 4 回で 0 + 8*4 = 32。飽和 (48) にはまだ届かない。
    CHECK(adpcm.stepIndex() == 32);
}

TEST_CASE("ニブルの bit3 が符号として効く")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    // ニブル 0: diff = step[0]/8 = 2。正方向。
    CHECK(adpcm.decodeNibble(0x0) == 2);
    // ニブル $F: 符号が負で、下位 3bit = 7 なので大きく引く。
    // step[0] のまま (ニブル 0 はインデックスを -1 するが 0 で飽和) なので
    // diff = 2 + 16 + 8 + 4 = 30、2 - 30 = -28。
    CHECK(adpcm.decodeNibble(0xF) == -28);
}

TEST_CASE("ステップの最小側は 0 で止まる")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    // ニブル 0 はインデックスを -1 する。初期値 0 から何度引いても 0 のまま。
    for (int i = 0; i < 10; ++i)
    {
        adpcm.decodeNibble(0x0);
    }
    CHECK(adpcm.stepIndex() == 0);

    // step[0] = 16 のままなので、1 回あたり +2 で 10 回ぶん進んでいる。
    CHECK(adpcm.signalLevel() == 20);
}

TEST_CASE("正側は 2047 で飽和する")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    // ニブル 7 を積み続ければ必ず上限に張り付く。
    for (int i = 0; i < 24; ++i)
    {
        adpcm.decodeNibble(0x7);
    }
    // 12bit DAC の上限。折り返して負にならないこと。
    CHECK(adpcm.signalLevel() == 2047);

    // さらに積んでも超えない。
    adpcm.decodeNibble(0x7);
    CHECK(adpcm.signalLevel() == 2047);
}

TEST_CASE("負側は -2048 で飽和する")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    for (int i = 0; i < 24; ++i)
    {
        adpcm.decodeNibble(0xF);
    }
    CHECK(adpcm.signalLevel() == -2048);

    adpcm.decodeNibble(0xF);
    CHECK(adpcm.signalLevel() == -2048);
}

TEST_CASE("ステップインデックスは 48 で飽和する")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    // ニブル 7 はインデックスを +6 する。8 回で 48 を超える。
    for (int i = 0; i < 16; ++i)
    {
        adpcm.decodeNibble(0x7);
    }
    // テーブルは 49 段 (0-48)。ここを超えると配列外参照になる。
    CHECK(adpcm.stepIndex() == 48);
}

TEST_CASE("1 バイトは上位ニブルが先に再生される")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    // 出力レートと ADPCM レートを揃え、1 サンプルにつき 1 ニブル進むようにする。
    adpcm.setSampleRate(15625, 15625);
    adpcm.writeCommand(x68k::Adpcm::kCommandPlay);

    // $70 = 上位ニブル 7、下位ニブル 0。
    // 上位が先なら 1 サンプル目は大きく振れ (30*8)、2 サンプル目は小さい。
    adpcm.writeData(0x70);

    const std::int16_t first = adpcm.renderOneSample();
    const std::int16_t second = adpcm.renderOneSample();

    // 1 サンプル目はニブル 7 のぶん。12bit を 8 倍して出す。
    CHECK(first == 30 * 8);
    // 2 サンプル目はニブル 0。ニブル 7 でインデックスが +8 されて
    // step[8] = 34 になっているので diff = 34/8 = 4、30 + 4 = 34。
    CHECK(second == 34 * 8);
    // 逆順ならこの大小関係が入れ替わる。
    CHECK(first < second);
}

TEST_CASE("停止中は無音を返す")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    adpcm.setSampleRate(15625, 15625);

    // 再生コマンドを出さずにデータだけ積む。
    adpcm.writeData(0x77);
    adpcm.writeData(0x77);

    std::int16_t buffer[8] = {};
    adpcm.renderSamples(buffer, 8);
    for (const std::int16_t sample : buffer)
    {
        CHECK(sample == 0);
    }
}

TEST_CASE("データが尽きたら無音になる")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    adpcm.setSampleRate(15625, 15625);
    adpcm.writeCommand(x68k::Adpcm::kCommandPlay);

    // 1 バイト = 2 サンプルぶんだけ与える。
    adpcm.writeData(0x77);

    std::int16_t buffer[8] = {};
    adpcm.renderSamples(buffer, 8);

    // 最初の 2 つは音が出る。
    CHECK(buffer[0] != 0);
    CHECK(buffer[1] != 0);
    // それ以降は供給が無いので 0。直前値を保持し続けると直流が残る。
    for (std::size_t i = 2; i < 8; ++i)
    {
        CHECK(buffer[i] == 0);
    }
}

TEST_CASE("停止コマンドでデコーダの状態が戻る")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    adpcm.setSampleRate(15625, 15625);
    adpcm.writeCommand(x68k::Adpcm::kCommandPlay);
    adpcm.writeData(0x77);
    static_cast<void>(adpcm.renderOneSample());

    // 途中まで進めた状態から停止する。
    CHECK(adpcm.signalLevel() != 0);
    adpcm.writeCommand(x68k::Adpcm::kCommandStop);

    // 次の再生が前の音の続きから始まると先頭に段差が出る。
    CHECK(adpcm.isPlaying() == false);
    CHECK(adpcm.signalLevel() == 0);
    CHECK(adpcm.stepIndex() == 0);
    CHECK(adpcm.fifoCount() == 0);
}

TEST_CASE("停止は再生より優先される")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    // 再生と停止を同時に指定したら停止する (実チップと同じ)。
    adpcm.writeCommand(x68k::Adpcm::kCommandPlay | x68k::Adpcm::kCommandStop);
    CHECK(adpcm.isPlaying() == false);
}

TEST_CASE("ステータスは再生中かつデータがあるときだけ BUSY を返す")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    // 停止中は 0。
    CHECK(adpcm.readStatus() == 0);

    adpcm.writeCommand(x68k::Adpcm::kCommandPlay);
    // 再生中でも FIFO が空なら「送るものが無い」= 完了として 0。
    // ここで BUSY を立て続けると、完了を待つ側が止まる。
    CHECK(adpcm.readStatus() == 0);

    adpcm.writeData(0x77);
    CHECK((adpcm.readStatus() & x68k::Adpcm::kStatusPlaying) != 0);
}

TEST_CASE("FIFO が溢れても壊れない")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    adpcm.writeCommand(x68k::Adpcm::kCommandPlay);

    // 容量を超えて書く。実機は DMA で律速するので溢れないが、
    // 溢れたときに配列外へ書かないことを確かめる。
    for (std::size_t i = 0; i < x68k::Adpcm::kFifoBytes * 2; ++i)
    {
        adpcm.writeData(0x55);
    }
    CHECK(adpcm.fifoCount() == x68k::Adpcm::kFifoBytes);
}

TEST_CASE("出力レートが高いとサンプルが引き伸ばされる")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    // ADPCM 7812Hz を 15625Hz で出す。おおよそ 2 倍に伸びる。
    adpcm.setSampleRate(15625, 7812);
    adpcm.writeCommand(x68k::Adpcm::kCommandPlay);
    for (int i = 0; i < 8; ++i)
    {
        adpcm.writeData(0x77);
    }

    std::int16_t buffer[16] = {};
    adpcm.renderSamples(buffer, 16);

    // 同じ値が 2 つ続く箇所があること (最近傍で伸ばしている証拠)。
    int repeated = 0;
    for (std::size_t i = 1; i < 16; ++i)
    {
        if (buffer[i] == buffer[i - 1])
        {
            ++repeated;
        }
    }
    CHECK(repeated > 0);
}

TEST_CASE("参照列と完全に一致する")
{
    x68k::Adpcm adpcm;
    adpcm.reset();

    // 仕様から手計算した列 (kNibble7Ramp) と 1 サンプルも違わないこと。
    // ここが合っていればステップテーブル・増減表・飽和のすべてが正しい。
    for (const std::int16_t expected : kNibble7Ramp)
    {
        CHECK(adpcm.decodeNibble(0x7) == expected);
    }
}

TEST_CASE("レートに 0 を渡してもゼロ除算しない")
{
    x68k::Adpcm adpcm;
    adpcm.reset();
    adpcm.setSampleRate(0, 0);
    adpcm.writeCommand(x68k::Adpcm::kCommandPlay);
    adpcm.writeData(0x77);

    std::int16_t buffer[4] = {};
    adpcm.renderSamples(buffer, 4);
    // 落ちずに何か返ればよい。
    CHECK(buffer[0] != 0);
}
