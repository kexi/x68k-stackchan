// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MSM6258V の ADPCM デコーダ。
//
// 【典拠】
// OKI (沖電気) の MSM5205/MSM6258 系が使う 4bit ADPCM は、Dialogic (旧 OKI)
// が公開している "OKI ADPCM" / VOX の仕様として広く知られる。以下の 2 つの
// テーブルが仕様の全体で、両方とも公開資料の値をそのまま使う。
//   1. ステップサイズテーブル (49 段, 16 から 1552 まで約 1.1 倍の等比)
//   2. ニブル下位 3bit → ステップインデックスの増減 (-1,-1,-1,-1,2,4,6,8)
//
// デコードの手順も仕様どおり:
//   diff = step/8 + step/4*b2 + step/2*b1 + step*b0   (b2,b1,b0 はニブルの下位 3bit)
//   ニブルの bit3 が符号。signal += (符号付き diff)
//   signal を -2048..2047 に飽和させ、index を上表で更新して 0..48 に飽和
//
// 【近似】
//   - DMA (DMAC ch3) 経由の連続転送は張っていない。
//   - パンとステレオは持たない。モノラルで出す。
//   - ADPCM レートと出力レートが異なるときは最近傍で伸ばす。線形補間の方が
//     音は良いが、ESP32 でのコストを避けた。Why not は resample の箇所に書いた。

#include "adpcm.h"

namespace x68k
{

namespace
{

// ステップサイズテーブル (49 段)。
// 16 から始まり、およそ 1.1 倍ずつ増えて 1552 まで。整数へ丸めた値が仕様。
constexpr std::int32_t kStepSizeTable[49] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,   45,   50,   55,   60,   66,  73,
    80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209,  230,  253,  279,  307,  337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
};

// ニブルの下位 3bit に応じたステップインデックスの増減。
// 小さい差分が続けばステップを縮め、大きい差分が来れば広げる = 適応量子化。
constexpr std::int32_t kStepAdjustTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// MSM6258 の DAC は 12bit。信号レベルはこの範囲に飽和する。
constexpr std::int32_t kSignalMin = -2048;
constexpr std::int32_t kSignalMax = 2047;

constexpr std::int32_t kStepIndexMax = 48;

}  // namespace

Adpcm::Adpcm()
{
    reset();
}

void Adpcm::reset()
{
    resetDecoder();
    fifo_.fill(0);
    fifoHead_ = 0;
    fifoCount_ = 0;
    highNibblePending_ = false;
    currentByte_ = 0;
    playing_ = false;
    resamplePhase_ = 0;
    lastSample_ = 0;
}

void Adpcm::resetDecoder()
{
    signal_ = 0;
    stepIndex_ = 0;
}

void Adpcm::setSampleRate(u32 outputRate, u32 adpcmRate)
{
    // 0 はゼロ除算になる。既定へ落とす。
    outputRate_ = outputRate == 0 ? kDefaultSampleRate : outputRate;
    adpcmRate_ = adpcmRate == 0 ? kDefaultSampleRate : adpcmRate;
    resamplePhase_ = 0;
}

std::int16_t Adpcm::decodeNibble(u8 nibble)
{
    const std::int32_t step = kStepSizeTable[stepIndex_];

    // 差分を組み立てる。仕様どおり step の 1/8 を基準に、
    // 下位 3bit の各ビットが 1/4, 1/2, 1 倍を足す。
    //
    // Why not step * (2*magnitude + 1) / 8 と一発で書くか: 等価だが、
    // 仕様書がビットごとの加算で書いているので、そのままの形にした方が
    // 資料と突き合わせやすい。実測でコスト差は無い (どちらもシフトと加算)。
    std::int32_t diff = step >> 3;
    const bool hasBit2 = (nibble & 0x04u) != 0;
    const bool hasBit1 = (nibble & 0x02u) != 0;
    const bool hasBit0 = (nibble & 0x01u) != 0;
    if (hasBit2)
    {
        diff += step;
    }
    if (hasBit1)
    {
        diff += step >> 1;
    }
    if (hasBit0)
    {
        diff += step >> 2;
    }

    // bit3 が符号。1 なら負。
    const bool isNegative = (nibble & 0x08u) != 0;
    signal_ += isNegative ? -diff : diff;

    // 12bit へ飽和させる。ここを折り返し (ラップ) にすると、大きな音で
    // 正負が反転して耳障りなノイズになる。飽和が仕様。
    if (signal_ < kSignalMin)
    {
        signal_ = kSignalMin;
    }
    if (signal_ > kSignalMax)
    {
        signal_ = kSignalMax;
    }

    // ステップインデックスを適応させる。
    std::int32_t nextIndex =
        static_cast<std::int32_t>(stepIndex_) + kStepAdjustTable[nibble & 0x07u];
    if (nextIndex < 0)
    {
        nextIndex = 0;
    }
    if (nextIndex > kStepIndexMax)
    {
        nextIndex = kStepIndexMax;
    }
    stepIndex_ = static_cast<u32>(nextIndex);

    return static_cast<std::int16_t>(signal_);
}

void Adpcm::writeCommand(u8 value)
{
    // 停止は再生より優先。実チップも同時指定なら停止する。
    const bool wantStop = (value & kCommandStop) != 0;
    if (wantStop)
    {
        playing_ = false;
        // 停止でデコーダの状態も戻す。次の再生が前の音の続きから
        // 始まると、先頭に大きな段差が出る。
        resetDecoder();
        fifoHead_ = 0;
        fifoCount_ = 0;
        highNibblePending_ = false;
        resamplePhase_ = 0;
        lastSample_ = 0;
        return;
    }

    const bool wantPlay = (value & kCommandPlay) != 0;
    if (wantPlay)
    {
        playing_ = true;
        return;
    }

    // 録音 (bit0) は実装しない。X68000 でも使う場面が限られる。
    // Why not 何かするか: 入力源が無いので、受け付けても嘘の値を返すだけ。
}

u8 Adpcm::readStatus() const
{
    // bit7 = 再生中。IOCS はこれを見て転送の完了を待つ。
    //
    // 再生中でも FIFO が空なら「もう送るものが無い」= 完了として
    // 0 を返す。Why not 再生フラグだけで返すか: 停止コマンドを出す前に
    // 完了を待つコードがあり、FIFO が空でも BUSY を立て続けると
    // そこで止まる。
    const bool isBusy = playing_ && fifoCount_ > 0;
    return isBusy ? kStatusPlaying : 0u;
}

void Adpcm::writeData(u8 value)
{
    // FIFO が満杯なら捨てる。実機は DMA の速度で律速するので溢れない。
    if (fifoCount_ >= kFifoBytes)
    {
        return;
    }
    const std::size_t tail = (fifoHead_ + fifoCount_) % kFifoBytes;
    fifo_[tail] = value;
    ++fifoCount_;
}

bool Adpcm::nextNibble(u8* nibble)
{
    // 1 バイトの後半 (下位ニブル) が残っていればそれを返す。
    if (highNibblePending_)
    {
        highNibblePending_ = false;
        *nibble = currentByte_ & 0x0Fu;
        return true;
    }

    if (fifoCount_ == 0)
    {
        return false;
    }

    currentByte_ = fifo_[fifoHead_];
    fifoHead_ = (fifoHead_ + 1) % kFifoBytes;
    --fifoCount_;

    // OKI ADPCM は上位ニブルが先。
    // Why not 下位から読むか: 順序を逆にすると、波形が 1 サンプルずれた
    // 別物になる。仕様で上位が先と決まっている。
    highNibblePending_ = true;
    *nibble = (currentByte_ >> 4) & 0x0Fu;
    return true;
}

std::int16_t Adpcm::renderOneSample()
{
    if (!playing_)
    {
        return 0;
    }

    // 出力レートが ADPCM レートより高いとき、同じサンプルを複数回出す
    // (最近傍補間)。位相アキュムレータで刻む。
    //
    // Why not 線形補間するか: ESP32 で 1 サンプルごとに乗算 2 回と
    // 除算相当が増える。ADPCM は元々 4bit の粗い音源で、最近傍で出る
    // 高域の折り返しは元の量子化ノイズに埋もれる。コストに見合わない。
    resamplePhase_ += adpcmRate_;
    const bool needsNewSample = resamplePhase_ >= outputRate_;
    if (!needsNewSample)
    {
        return lastSample_;
    }
    resamplePhase_ -= outputRate_;

    u8 nibble = 0;
    if (!nextNibble(&nibble))
    {
        // 供給が途切れた。直前の値を保つと直流が残るので 0 へ落とす。
        lastSample_ = 0;
        return 0;
    }

    const std::int16_t decoded = decodeNibble(nibble);

    // 12bit を 16bit へ広げる。ADPCM の最大振幅が FM と釣り合うようにする。
    lastSample_ = static_cast<std::int16_t>(decoded * 8);
    return lastSample_;
}

void Adpcm::renderSamples(std::int16_t* out, std::size_t frames)
{
    if (out == nullptr)
    {
        return;
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        out[i] = renderOneSample();
    }
}

}  // namespace x68k
