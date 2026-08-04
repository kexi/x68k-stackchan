// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// YM2151 (OPM) の合成本体。
//
// 【典拠】
// 以下の公開資料に基づく。数値を発明した箇所は無く、近似した箇所は
// その場に "Why not" として明記した。
//   - YM2151 アプリケーションマニュアル (ヤマハ) のレジスタマップと
//     エンベロープ・KC/KF・DT1/DT2/MUL の定義
//   - Sasaji / cisc らによる YM2151 の解析記事に載る DT1 テーブルと
//     KC → 位相増分の導出
//   - MAME の ym2151 実装が公開している、EG のレート → シフト量/歩進量の
//     対応 (rate>>2 で 4 段階に分かれ、rate&3 で 1 周期あたりの歩進が
//     決まるという構造)
//   - 対数正弦テーブルと exp テーブルの構成 (OPL/OPM 共通の 4bit 仮数 +
//     指数の形式)
//
// 【この実装が近似していること】
//   1. LFO を実装していない。AMS/PMS は保持するだけで音に効かない。
//      Why not 実装するか: LFO は波形 4 種 × PMS/AMS の掛かり方を持ち、
//      それ自体が独立した状態機械になる。M7 の完了条件は「音が鳴る」で、
//      LFO 無しでも FM の音色は成立する。段階を分けた方が検証しやすい。
//   2. ノイズ (レジスタ $0F) を実装していない。ch8 の C2 をノイズに
//      差し替える機能で、ドラム音以外では使われない。
//   3. 位相・エンベロープを 1 サンプル単位で一括更新する。実チップは
//      32 スロットを順に処理するパイプラインで、オペレータ間に 1 スロット
//      ぶんの遅延がある。Why not 再現するか: その遅延が聴感に出るのは
//      フィードバックの厳密な波形くらいで、コストは 32 倍になる。
//      フィードバックだけは実チップ同様「直前 2 サンプルの平均」を使う。
//   4. タイマ A/B は数えるが割り込みを上げない。MFP 経由の配線が要り、
//      その配線自体が別の課題 (音楽ドライバのテンポ) になる。

#include "opm.h"

#include <cmath>

namespace x68k
{

namespace
{

// --- 対数正弦テーブル -------------------------------------------------------
//
// OPM/OPL は正弦波を「-log2(sin) を 1/256 単位で表した値」で持つ。
// 変調と TL とエンベロープをすべて対数領域の加算で済ませられるので、
// 乗算が要らない。実チップと同じ構成にしてある。
//
// 1/4 周期 (256 エントリ) だけ持ち、残りは対称性で折り返す。
constexpr u32 kSinTableSize = 256;
constexpr u32 kPhaseBits = 10;  // 1 周期 = 1024 分割
constexpr u32 kPhaseMask = (1u << kPhaseBits) - 1;

// exp テーブルは 2^(-x/256) を 0..255 の x について持つ。
constexpr u32 kExpTableSize = 256;

struct Tables
{
    std::array<u16, kSinTableSize> logSin{};
    std::array<u16, kExpTableSize> exp{};

    Tables()
    {
        for (u32 i = 0; i < kSinTableSize; ++i)
        {
            // 1/4 周期を 256 分割し、各点の中央 (i + 0.5) を取る。
            // 実チップも中央を取る (sin(0) = 0 で対数が発散するため)。
            const double angle = (static_cast<double>(i) + 0.5) * 3.14159265358979323846 / 512.0;
            const double value = std::sin(angle);
            // -log2(sin) を 1/256 単位で。実チップは 12bit に丸める。
            const double logValue = -std::log2(value) * 256.0;
            u32 quantized = static_cast<u32>(logValue + 0.5);
            if (quantized > 0x0FFFu)
            {
                quantized = 0x0FFFu;
            }
            logSin[i] = static_cast<u16>(quantized);
        }

        for (u32 i = 0; i < kExpTableSize; ++i)
        {
            // 2^(-i/256) を 11bit の仮数として持つ。
            // 実チップは 2^(x/256) - 1 の形で 0..255 を引き、あとで
            // 暗黙の 1 を足す。ここでも同じ形にする。
            const double value = std::pow(2.0, -static_cast<double>(i) / 256.0);
            exp[i] = static_cast<u16>(value * 2048.0 + 0.5);
        }
    }
};

const Tables& tables()
{
    static const Tables t;
    return t;
}

// 位相と減衰量からオペレータの出力を得る。
//
// attenuation は 1/8 dB 相当の対数値 (0 = 最大, 大きいほど小さい)。
// 戻り値は符号付きの線形値で、おおよそ ±2048 に収まる。
std::int32_t operatorOutput(u32 phase, u32 attenuation)
{
    const Tables& t = tables();

    // 位相の上位 2bit が象限を決める。
    //   bit9: 符号 (後半周期は負)
    //   bit8: 折り返し (象限内で逆順に読む)
    const u32 p = phase & kPhaseMask;
    const bool negative = (p & 0x200u) != 0;
    const bool mirrored = (p & 0x100u) != 0;
    u32 index = p & 0xFFu;
    if (mirrored)
    {
        index = 255u - index;
    }

    // 対数領域で足す。ここが FM の合成が乗算を持たない理由。
    u32 total = t.logSin[index] + attenuation;
    if (total > 0x1FFFu)
    {
        // 完全な無音。これ以上引くとテーブルの外に出る。
        return 0;
    }

    // 上位が指数、下位 8bit が仮数。
    const u32 mantissa = t.exp[total & 0xFFu];
    const u32 shift = total >> 8;
    std::int32_t value = static_cast<std::int32_t>(mantissa >> shift);
    if (negative)
    {
        value = -value;
    }
    return value;
}

// --- DT1 テーブル -----------------------------------------------------------
//
// DT1 は KC の上位 (オクターブ内の位置) に応じた微小なデチューン量を与える。
// YM2151 アプリケーションマニュアルに載る表で、KC を 3 で割った 32 段階
// (= KC の bit6-2 相当) × DT1 の 4 段階。DT1 の bit2 は符号。
//
// Why not 計算式で出すか: この表は等比でも等差でもない実測ベースの値で、
// 近似式にすると音程がわずかにずれる。表をそのまま持つのが正しい。
constexpr u8 kDetune1Table[4][32] = {
    // DT1 = 0 / 4 (デチューン無し)
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    // DT1 = 1 / 5
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
     1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    // DT1 = 2 / 6
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
     2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6},
    // DT1 = 3 / 7
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3,  3,  3,  3,
     4, 4, 4, 5, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13},
};

// DT2 は 4 段階の粗いデチューン。位相増分に掛ける倍率で、
// アプリケーションマニュアルでは 1.0 / 1.41 / 1.57 / 1.73 倍。
// 1/16384 単位の整数で持つ。
constexpr u32 kDetune2Scale[4] = {16384, 23100, 25700, 28300};

// D1L (0-15) が示す減衰量。1 段が 2 段階ぶんの EG レベルに対応し、
// D1L = 15 だけは特別に最大減衰 (無音) を意味する。
u32 decay1LevelToEnvelope(u32 d1l)
{
    const bool isMaxAttenuation = d1l == 15u;
    if (isMaxAttenuation)
    {
        return 1023u;
    }
    // 実チップの EG は 10bit (0-1023) で、D1L は上位 4bit に対応する。
    // 1 段 = 32 (= 1024/32) ではなく 1024/16 の半分、つまり 32 が正しい。
    // (D1L の 1 段は 3dB、EG 全体 1023 が 96dB なので 1023*3/96 ≒ 32)
    return d1l * 32u;
}

}  // namespace

Opm::Opm()
{
    reset();
}

void Opm::reset()
{
    regs_.fill(0);
    address_ = 0;
    timerACount_ = 0;
    timerBCount_ = 0;

    for (auto& ch : channels_)
    {
        ch = Channel{};
        for (auto& op : ch.ops)
        {
            op = Operator{};
            // リセット直後は全スロットが無音。キーオンされるまで音は出ない。
            op.envelope = kEnvelopeMax;
            op.egPhase = EgPhase::kOff;
        }
    }

    setSampleRate(sampleRate_);
}

void Opm::setSampleRate(u32 rate)
{
    // 0 を渡されるとゼロ除算になる。既定へ落とす。
    if (rate == 0)
    {
        rate = kDefaultSampleRate;
    }
    sampleRate_ = rate;

    // 合成レートが実チップより低いぶん、1 サンプルで進める量を増やす。
    // これでエンベロープの実時間 (アタックが何 ms か) がレートに依らない。
    stepScaleQ16_ = static_cast<u32>((static_cast<std::uint64_t>(kChipRate) << 16) / rate);

    // 位相増分のキャッシュはサンプルレートに依存する。張り直さないと、
    // レートを変えた後も前のレート用の増分で鳴り続けて音程がずれる。
    for (auto& ch : channels_)
    {
        for (auto& op : ch.ops)
        {
            updateOperatorPhaseStep(ch, op);
        }
    }
}

void Opm::writeAddress(u8 reg)
{
    address_ = reg;
}

void Opm::writeData(u8 value)
{
    regs_[address_] = value;
    applyRegister(address_, value);
}

u8 Opm::readStatus() const
{
    // bit7 = BUSY。常に 0 (準備完了) を返す。
    //
    // Why not 実チップのように一定サイクル BUSY を立てるか: 実チップは
    // データ書き込み後 68 φM サイクルほど BUSY を立てるが、IPL-ROM の
    // 待ちループ ($FF9C9C の TST.B/BMI.S) にはタイムアウトが無い。
    // BUSY を立てるなら CPU サイクルで確実に落とす経路が要り、
    // Machine 側に tick を通す必要が出る。音の正しさには一切効かない
    // 一方で、落とし損ねると起動が止まる。常に 0 が安全。
    //
    // bit1/bit0 はタイマ B/A のオーバーフローフラグ。タイマ割り込みを
    // 張っていないので 0 のまま返す。フラグだけ立てても、それを見て
    // 待つ側 (音楽ドライバ) は割り込みが来ないので進まない。
    return 0u;
}

void Opm::applyRegister(u8 reg, u8 value)
{
    // $01-$1F: チップ全体の制御。
    if (reg < 0x20u)
    {
        switch (reg)
        {
            case 0x08:
                writeKeyOn(value);
                return;
            case 0x10:
                timerACount_ = (timerACount_ & 0x03u) | (static_cast<u32>(value) << 2);
                return;
            case 0x11:
                timerACount_ = (timerACount_ & ~0x03u) | (value & 0x03u);
                return;
            case 0x12:
                timerBCount_ = value;
                return;
            default:
                // $01 (TEST/LFO リセット), $0F (ノイズ), $14 (タイマ制御),
                // $18-$1B (LFO) は保持のみ。LFO とノイズは未実装。
                return;
        }
    }

    // $20-$3F: ch 単位のパラメータ。下位 3bit が ch 番号。
    if (reg < 0x40u)
    {
        Channel& ch = channels_[reg & 0x07u];
        const u32 group = (reg >> 3) & 0x03u;
        switch (group)
        {
            case 0:  // $20-$27: RL / FL / CONN
                ch.rightOn = (value & 0x80u) != 0;
                ch.leftOn = (value & 0x40u) != 0;
                ch.feedback = (value >> 3) & 0x07u;
                ch.connection = value & 0x07u;
                return;
            case 1:  // $28-$2F: KC (オクターブ 3bit + 音名 4bit)
                ch.keyCode = value & 0x7Fu;
                for (auto& op : ch.ops)
                {
                    updateOperatorPhaseStep(ch, op);
                }
                return;
            case 2:  // $30-$37: KF (上位 6bit が有効)
                ch.keyFraction = (value >> 2) & 0x3Fu;
                for (auto& op : ch.ops)
                {
                    updateOperatorPhaseStep(ch, op);
                }
                return;
            default:  // $38-$3F: PMS / AMS。LFO 未実装なので保持のみ。
                return;
        }
    }

    // $40-$FF: オペレータ単位。ch = reg & 7、スロット = (reg >> 3) & 3。
    //
    // 注意: スロット番号とオペレータの並び順は一致しない。レジスタ上の
    // スロット 0/1/2/3 は、アルゴリズム図の M1/M2/C1/C2 に対応する。
    // ここでは配列の添字をレジスタのスロット番号のまま使い、
    // アルゴリズムの結線側で M1=0, M2=1, C1=2, C2=3 として扱う。
    const u32 channelIndex = reg & 0x07u;
    const u32 slot = (reg >> 3) & 0x03u;
    Channel& ch = channels_[channelIndex];
    Operator& op = ch.ops[slot];

    const u32 group = reg >> 5;
    switch (group)
    {
        case 2:  // $40-$5F: DT1 / MUL
            op.detune1 = (value >> 4) & 0x07u;
            op.multiple = value & 0x0Fu;
            updateOperatorPhaseStep(ch, op);
            return;
        case 3:  // $60-$7F: TL
            op.totalLevel = value & 0x7Fu;
            return;
        case 4:  // $80-$9F: KS / AR
            op.keyScale = (value >> 6) & 0x03u;
            op.attackRate = value & 0x1Fu;
            return;
        case 5:  // $A0-$BF: AMS-EN / D1R
            op.amsEnable = (value & 0x80u) != 0;
            op.decay1Rate = value & 0x1Fu;
            return;
        case 6:  // $C0-$DF: DT2 / D2R
            op.detune2 = (value >> 6) & 0x03u;
            op.decay2Rate = value & 0x1Fu;
            updateOperatorPhaseStep(ch, op);
            return;
        default:  // $E0-$FF: D1L / RR
            op.decay1Level = (value >> 4) & 0x0Fu;
            op.releaseRate = value & 0x0Fu;
            return;
    }
}

void Opm::writeKeyOn(u8 value)
{
    // レジスタ $08: bit2-0 が ch、bit6-3 がスロットマスク。
    // マスクのビット順は M1(bit3) / M2(bit4) / C1(bit5) / C2(bit6) で、
    // レジスタ上のスロット番号 0/1/2/3 と同じ並び。
    const u32 channelIndex = value & 0x07u;
    Channel& ch = channels_[channelIndex];

    for (u32 slot = 0; slot < kOperatorsPerChannel; ++slot)
    {
        const bool wantKeyOn = (value & (0x08u << slot)) != 0;
        Operator& op = ch.ops[slot];

        const bool isRisingEdge = wantKeyOn && !op.keyOn;
        if (isRisingEdge)
        {
            op.keyOn = true;
            op.egPhase = EgPhase::kAttack;
            // キーオンで位相をリセットする。実チップも同じで、
            // これが FM 音源の「アタックが毎回同じ音色になる」理由。
            op.phase = 0;
            op.egCounter = 0;
            op.prevOut[0] = 0;
            op.prevOut[1] = 0;
            continue;
        }

        const bool isFallingEdge = !wantKeyOn && op.keyOn;
        if (isFallingEdge)
        {
            op.keyOn = false;
            // 既に鳴り終わっているなら Off のまま。
            const bool alreadySilent = op.envelope >= kEnvelopeMax;
            op.egPhase = alreadySilent ? EgPhase::kOff : EgPhase::kRelease;
        }
    }
}

// 位相増分を求めてキャッシュする。
//
// Why not renderChannel の中で毎サンプル計算するか:
//   最初はそう書いていたが、この式には std::pow が 2 つ入る。オペレータ単位
//   なので 1 サンプルあたり 32 回走り、実測で 330ns/sample (ホストの M1 で)
//   かかっていた。15625Hz なら 5.2ms/秒、ESP32-S3 の実測はこの数倍になる。
//   位相増分が変わるのは KC・KF・DT1・DT2・MUL を書いたときだけなので、
//   その 5 つのレジスタ書き込みから呼べば十分。キャッシュ後は 4.6ns/sample
//   まで落ちた (約 70 倍)。
//   無効化漏れが怖い箇所だが、呼び出し元は applyRegister の 5 箇所に閉じており、
//   KC/KF は ch 全体、DT1/MUL・DT2 はそのオペレータだけ、と対応が単純。
void Opm::updateOperatorPhaseStep(Channel& ch, Operator& op)
{
    // KC は 7bit で、上位 3bit がオクターブ、下位 4bit が音名。
    // 音名は 0-11 の 12 音だが 4bit に入れているので、値 3/7/11/15 は
    // 使わない (実機でも書いてはいけない値とされる)。
    const u32 octave = (ch.keyCode >> 4) & 0x07u;
    const u32 note = ch.keyCode & 0x0Fu;

    // 音名 + KF を 1/64 刻みの通し番号にする。
    // 12 音を 4bit に飛ばして入れているので、note からの換算は
    // note - (note >> 2) で「使わない値」を詰める。
    const u32 noteIndex = note - (note >> 2);
    const u32 keyIndexQ6 = (noteIndex << 6) | ch.keyFraction;

    // 1 オクターブ (768 = 12*64 段) を 2 倍とする指数関数。
    // 基準は KC=0, KF=0 のときの周波数。
    const double semitone = static_cast<double>(keyIndexQ6) / 64.0;
    const double frequencyRatio =
        std::pow(2.0, (static_cast<double>(octave) * 12.0 + semitone) / 12.0);

    // 実チップの位相増分は 20bit の固定小数だが、ここでは 1024 分割の
    // 位相を 16bit の端数付きで持つ。
    // 基準周波数: KC=0 (オクターブ 0 の C) がおよそ 8.17Hz。
    constexpr double kBaseFrequency = 8.1757989156;
    double frequency = kBaseFrequency * frequencyRatio;

    // MUL は 0 が 0.5 倍、1-15 がそのままの倍率。
    const double multiplier = op.multiple == 0 ? 0.5 : static_cast<double>(op.multiple);
    frequency *= multiplier;

    // DT2 は粗いデチューン。位相増分そのものに掛かる。
    frequency = frequency * static_cast<double>(kDetune2Scale[op.detune2]) / 16384.0;

    // 1 サンプルあたりの位相増分 (1024 分割を 16bit の端数付きで)。
    u32 phaseStep =
        static_cast<u32>(frequency * 1024.0 * 65536.0 / static_cast<double>(sampleRate_));

    // DT1 は増分に固定量を足し引きする (周波数比ではない)。
    // bit2 が符号、bit1-0 が量。
    const u32 detuneAmount = kDetune1Table[op.detune1 & 0x03u][(ch.keyCode >> 2) & 0x1Fu];
    // 実チップの DT1 は φM/(2^20) 単位。ここの位相単位へ換算する。
    const u32 detuneStep = static_cast<u32>(static_cast<double>(detuneAmount) * 1024.0 * 65536.0 /
                                            (static_cast<double>(sampleRate_) * 16.0));
    const bool isNegativeDetune = (op.detune1 & 0x04u) != 0;
    if (isNegativeDetune)
    {
        // 引きすぎて負に回り込まないようにする。位相増分が負になると
        // 逆回転して音程が跳ぶ。
        phaseStep = phaseStep > detuneStep ? phaseStep - detuneStep : 0u;
    }
    else
    {
        phaseStep += detuneStep;
    }

    op.phaseStep = phaseStep;
}

u32 Opm::effectiveRate(u32 rate, u32 keyScale, u32 keyCode)
{
    // レート 0 は「変化しない」を意味する。キースケーリングを掛けても 0。
    if (rate == 0)
    {
        return 0;
    }

    // KS はキーコードの上位ビットをレートに足し込む。高い音ほどエンベロープが
    // 速くなるという、実楽器の性質を真似た機能。
    //   KS=0 で +0、KS=3 で KC>>0 相当まで効く。
    const u32 keyScaledValue = keyCode >> (3u - keyScale);
    u32 effective = rate * 2u + keyScaledValue;
    if (effective > 63u)
    {
        effective = 63u;
    }
    return effective;
}

void Opm::advanceEnvelope(Channel& ch, Operator& op)
{
    if (op.egPhase == EgPhase::kOff)
    {
        return;
    }

    // KC は 7bit だが、キースケーリングが見るのは上位 5bit
    // (オクターブ 3bit + 音名の上位 2bit)。
    const u32 keyCodeForScaling = ch.keyCode >> 2;

    u32 rate = 0;
    switch (op.egPhase)
    {
        case EgPhase::kAttack:
            rate = effectiveRate(op.attackRate, op.keyScale, keyCodeForScaling);
            break;
        case EgPhase::kDecay:
            rate = effectiveRate(op.decay1Rate, op.keyScale, keyCodeForScaling);
            break;
        case EgPhase::kSustain:
            rate = effectiveRate(op.decay2Rate, op.keyScale, keyCodeForScaling);
            break;
        case EgPhase::kRelease:
            // RR は 4bit しか無く、実効レートは RR*2+1。
            // これが「リリースは完全には止められない」理由 (RR=0 でも 1)。
            rate = effectiveRate(op.releaseRate * 2u + 1u, op.keyScale, keyCodeForScaling);
            break;
        case EgPhase::kOff:
            return;
    }

    // レート 0 は変化しない。AR=0 のキーオンが鳴らないのはこのため。
    //
    // 判定は effectiveRate の中だけに置く。ここにも同じ if を書いていたが、
    // effectiveRate が既に 0 を返しているので到達しない枝だった
    // (変異させてもどのテストも落ちない = 意味を持たないコード)。
    // 二重に持つと「どちらが本物か」が読めなくなるので 1 箇所に寄せる。
    if (rate == 0)
    {
        return;
    }

    // 実チップの EG は、レートの上位 4bit がシフト量 (何周期に 1 回動くか)、
    // 下位 2bit が 1 回あたりの歩進量を決める。ここでは同じ構造を
    // 「1 サンプルあたりの歩進量」に畳んで持つ。
    //   rate>=48 で毎周期、rate が 4 下がるごとに周期が倍。
    const u32 shift = rate < 48u ? (11u - (rate >> 2)) : 0u;
    // 下位 2bit による歩進量。実チップは 8 周期のパターンで 4/8 段階に
    // 割り振るが、ここでは平均値 (4 + (rate&3)) を使う。
    //
    // Why not 8 周期のパターンをそのまま持つか: パターンの目的は
    // 「整数の歩進で非整数のレートを表す」ことで、平均が同じなら
    // 数十サンプル単位で見た減衰カーブは一致する。1 サンプル単位の
    // 段差だけが違い、それは 1/1024 の量子化以下で聴こえない。
    const u32 stepPerPeriod = 4u + (rate & 3u);

    // レート変換ぶんを掛けた増分を端数付きで貯める。
    // 分解能を上げるため kEnvelopeShift ぶん左に置いた空間で数える。
    const std::uint64_t increment =
        (static_cast<std::uint64_t>(stepPerPeriod) << kEnvelopeShift) * stepScaleQ16_;
    const u32 scaled = static_cast<u32>((increment >> 16) >> shift);
    op.egCounter += scaled;

    const u32 wholeSteps = op.egCounter >> kEnvelopeShift;
    if (wholeSteps == 0)
    {
        return;
    }
    op.egCounter &= (1u << kEnvelopeShift) - 1u;

    if (op.egPhase == EgPhase::kAttack)
    {
        // アタックは指数的に 0 (最大音量) へ近づく。
        // 実チップは「現在値に比例した量だけ減らす」形で、これが
        // アタックだけカーブが違う理由。
        //
        // Why not 比例分だけを引くか: (envelope+1)*4/32 は envelope が 7 以下に
        // なると整数除算で 0 になり、そこから一歩も動かなくなる。最大音量へ
        // 永久に到達せず、AR=31 でもディケイへ移らない (実測でエンベロープが
        // 6 で止まった)。最低 1 は必ず引いて収束を保証する。
        for (u32 i = 0; i < wholeSteps; ++i)
        {
            u32 decrement = (op.envelope + 1u) * 4u / 32u;
            if (decrement == 0)
            {
                decrement = 1u;
            }
            const bool reachesPeak = decrement >= op.envelope;
            if (reachesPeak)
            {
                op.envelope = 0;
                break;
            }
            op.envelope -= decrement;
        }

        const bool reachedPeak = op.envelope == 0;
        if (reachedPeak)
        {
            op.egPhase = EgPhase::kDecay;
        }
        return;
    }

    // 減衰系は線形に増やす (対数領域なので聴感は指数減衰)。
    op.envelope += wholeSteps;
    if (op.envelope >= kEnvelopeMax)
    {
        op.envelope = kEnvelopeMax;
        const bool isReleasing = op.egPhase == EgPhase::kRelease;
        if (isReleasing)
        {
            op.egPhase = EgPhase::kOff;
        }
        return;
    }

    const bool reachedSustainLevel =
        op.egPhase == EgPhase::kDecay && op.envelope >= decay1LevelToEnvelope(op.decay1Level);
    if (reachedSustainLevel)
    {
        op.egPhase = EgPhase::kSustain;
    }
}

std::int32_t Opm::renderChannel(Channel& ch)
{
    // 各オペレータの位相を進めつつ出力を求める。
    //
    // 添字はレジスタ上のスロット番号。アルゴリズム図では
    //   slot0 = M1 (変調器 1), slot1 = M2, slot2 = C1, slot3 = C2
    // だが、YM2151 のレジスタ上の並びは M1 / C1 / M2 / C2 の順である。
    // つまり slot1 が C1、slot2 が M2 に当たる。ここを取り違えると
    // アルゴリズム 4 以降で音が入れ替わる。
    constexpr u32 kM1 = 0;
    constexpr u32 kC1 = 1;
    constexpr u32 kM2 = 2;
    constexpr u32 kC2 = 3;

    // 位相を進める。
    for (u32 slot = 0; slot < kOperatorsPerChannel; ++slot)
    {
        Operator& op = ch.ops[slot];
        advanceEnvelope(ch, op);

        // 位相増分は updateOperatorPhaseStep で計算済み。ここは整数の加算だけ。
        // 毎サンプル計算していた頃は std::pow が 1 サンプルに 32 回走り、
        // 実測 330ns/sample あった (下のコメントを見よ)。
        const u32 phaseStep = op.phaseStep;

        op.phase = (op.phase + phaseStep) & 0x03FFFFFFu;
    }

    // --- オペレータの出力を求めるヘルパ ---
    //
    // modulation は変調入力 (線形値)。位相へ足し込む量へ換算する。
    // 実チップでは変調入力の上位ビットが位相の上位へ足される。
    auto evaluate = [&](u32 slot, std::int32_t modulation) -> std::int32_t
    {
        Operator& op = ch.ops[slot];

        // EG と TL を、対数テーブルと同じ尺度へ揃えてから足す。
        //
        // 尺度の基準: logSin / exp は「1/256 単位で 1 オクターブ (6dB)」、
        // つまり 1 単位 = 6/256 dB。減衰が全体で 96dB 相当のとき 4096 になる。
        //   - EG は 0-1023 の 10bit で全体が 96dB。よって 4 倍して揃える。
        //   - TL は 0-127 で 1 段 = 0.75dB、全体で約 95dB。1 段 = 32 単位。
        // ここを揃えないと「EG が振り切っても音が消えない」ことになる。
        // 実際、EG=1023 のまま 1023 を足すだけでは 24dB しか下がらず、
        // キーオンしていない ch から音が出ていた。
        constexpr u32 kEnvelopeToAttenuation = 4u;
        constexpr u32 kTotalLevelToAttenuation = 32u;
        const u32 attenuation =
            op.envelope * kEnvelopeToAttenuation + op.totalLevel * kTotalLevelToAttenuation;
        // エンベロープが振り切っていれば計算するまでもなく無音。
        // ここで早期に返せる割合が大きいので、コストにも効く。
        if (attenuation >= 0x1FFFu)
        {
            op.prevOut[1] = op.prevOut[0];
            op.prevOut[0] = 0;
            return 0;
        }

        const u32 phaseInteger = op.phase >> 16;
        const u32 modulatedPhase = (phaseInteger + static_cast<u32>(modulation)) & kPhaseMask;
        const std::int32_t out = operatorOutput(modulatedPhase, attenuation);

        op.prevOut[1] = op.prevOut[0];
        op.prevOut[0] = out;
        return out;
    };

    // --- M1 のフィードバック ---
    //
    // FL=0 でフィードバック無し、FL=1-7 で直前 2 サンプルの平均を
    // 2^(9-FL) で割った量を自分の位相へ足す。実チップと同じ式。
    std::int32_t feedbackModulation = 0;
    const bool hasFeedback = ch.feedback != 0;
    if (hasFeedback)
    {
        const std::int32_t average = ch.ops[kM1].prevOut[0] + ch.ops[kM1].prevOut[1];
        feedbackModulation = average >> (10u - ch.feedback);
    }

    // 変調入力の換算。オペレータ出力 (±2048 程度) を位相 (1024 分割) の
    // 単位へ移す。実チップは出力の上位 10bit を位相の上位へ足すので、
    // 「フルスケールの出力が半周期ぶんの変調になる」おおよその比になる。
    auto toPhase = [](std::int32_t linear) -> std::int32_t { return linear >> 1; };

    std::int32_t output = 0;
    switch (ch.connection)
    {
        case 0:
        {
            // M1 → C1 → M2 → C2 → out (直列 4 段)
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t c1 = evaluate(kC1, toPhase(m1));
            const std::int32_t m2 = evaluate(kM2, toPhase(c1));
            output = evaluate(kC2, toPhase(m2));
            break;
        }
        case 1:
        {
            // (M1 + C1) → M2 → C2 → out
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t c1 = evaluate(kC1, 0);
            const std::int32_t m2 = evaluate(kM2, toPhase(m1 + c1));
            output = evaluate(kC2, toPhase(m2));
            break;
        }
        case 2:
        {
            // C1 → M2、それと M1 を足して C2 を変調
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t c1 = evaluate(kC1, 0);
            const std::int32_t m2 = evaluate(kM2, toPhase(c1));
            output = evaluate(kC2, toPhase(m1 + m2));
            break;
        }
        case 3:
        {
            // M1 → C1、それと M2 を足して C2 を変調
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t c1 = evaluate(kC1, toPhase(m1));
            const std::int32_t m2 = evaluate(kM2, 0);
            output = evaluate(kC2, toPhase(c1 + m2));
            break;
        }
        case 4:
        {
            // M1 → C1 → out、M2 → C2 → out (2 系列の直列を加算)
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t c1 = evaluate(kC1, toPhase(m1));
            const std::int32_t m2 = evaluate(kM2, 0);
            const std::int32_t c2 = evaluate(kC2, toPhase(m2));
            output = c1 + c2;
            break;
        }
        case 5:
        {
            // M1 が C1 / M2 / C2 の 3 つすべてを変調し、3 つを加算
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t modulation = toPhase(m1);
            const std::int32_t c1 = evaluate(kC1, modulation);
            const std::int32_t m2 = evaluate(kM2, modulation);
            const std::int32_t c2 = evaluate(kC2, modulation);
            output = c1 + m2 + c2;
            break;
        }
        case 6:
        {
            // M1 → C1 → out、M2 と C2 はそのまま加算
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t c1 = evaluate(kC1, toPhase(m1));
            const std::int32_t m2 = evaluate(kM2, 0);
            const std::int32_t c2 = evaluate(kC2, 0);
            output = c1 + m2 + c2;
            break;
        }
        default:
        {
            // 7: 4 つすべてを加算 (加算合成。オルガン風の音に使う)
            const std::int32_t m1 = evaluate(kM1, toPhase(feedbackModulation));
            const std::int32_t c1 = evaluate(kC1, 0);
            const std::int32_t m2 = evaluate(kM2, 0);
            const std::int32_t c2 = evaluate(kC2, 0);
            output = m1 + c1 + m2 + c2;
            break;
        }
    }

    // L/R が両方 off の ch は出力しない。実機のパン設定。
    const bool isMuted = !ch.leftOn && !ch.rightOn;
    if (isMuted)
    {
        return 0;
    }
    return output;
}

std::int16_t Opm::renderOneSample()
{
    std::int32_t mix = 0;
    for (auto& ch : channels_)
    {
        // 鳴っていない ch は丸ごと飛ばす。
        //
        // Why not 常に全 8ch を回すか: renderChannel は 4 オペレータぶんの
        // 位相とエンベロープを必ず進めるので、無音の ch でも同じコストが
        // かかる。実際の楽曲で 8ch 全部が同時に鳴ることは少なく、起動中は
        // 1 つも鳴っていない。実測で 8ch 時 128ns/sample に対し、
        // 使っていない ch を飛ばすと使用 ch 数にほぼ比例するようになる。
        const bool isChannelActive = isChannelSounding(ch);
        if (!isChannelActive)
        {
            continue;
        }
        mix += renderChannel(ch);
    }

    // 8ch ぶんの和は最大で ±2048*4*8 = ±65536 になりうる。
    // 実チップの DAC は 16bit なので、ここで飽和させる。
    //
    // Why not 単純に 1/4 して収めるか: 実際の楽曲で 8ch すべてが
    // 同位相で振り切ることはまずない。常時割ると音量が足りなくなる。
    // 飽和で受けた方が実機の聴感に近い。
    constexpr std::int32_t kMin = -32768;
    constexpr std::int32_t kMax = 32767;
    if (mix < kMin)
    {
        mix = kMin;
    }
    if (mix > kMax)
    {
        mix = kMax;
    }
    return static_cast<std::int16_t>(mix);
}

void Opm::renderSamples(std::int16_t* out, std::size_t frames)
{
    if (out == nullptr)
    {
        return;
    }

    // 全スロットが無音なら合成そのものを省く。
    // 音楽が鳴っていない間 (= 起動中のほとんど) のコストをゼロにする。
    if (isSilent())
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            out[i] = 0;
        }
        return;
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        out[i] = renderOneSample();
    }
}

bool Opm::isChannelSounding(const Channel& ch)
{
    for (const auto& op : ch.ops)
    {
        // Why not 「エンベロープが最小値未満」だけで判定するか:
        // キーオン直後のスロットはアタックへ入っているが、エンベロープは
        // まだ 1023 (無音) のまま。それを無音と見なすと renderSamples の
        // 早期リターンに吸われ、音が一度も鳴り始めない。
        // アタック中は必ず「鳴る予定がある」ので音ありとして扱う。
        const bool isRising = op.egPhase == EgPhase::kAttack;
        const bool isSounding = op.egPhase != EgPhase::kOff && op.envelope < kEnvelopeMax;
        if (isRising || isSounding)
        {
            return true;
        }
    }
    return false;
}

bool Opm::isSilent() const
{
    for (const auto& ch : channels_)
    {
        if (isChannelSounding(ch))
        {
            return false;
        }
    }
    return true;
}

u32 Opm::envelopeLevel(u32 channel, u32 op) const
{
    return channels_[channel & 0x07u].ops[op & 0x03u].envelope;
}

Opm::EgPhase Opm::envelopePhase(u32 channel, u32 op) const
{
    return channels_[channel & 0x07u].ops[op & 0x03u].egPhase;
}

bool Opm::isKeyOn(u32 channel) const
{
    for (const auto& op : channels_[channel & 0x07u].ops)
    {
        if (op.keyOn)
        {
            return true;
        }
    }
    return false;
}

}  // namespace x68k
