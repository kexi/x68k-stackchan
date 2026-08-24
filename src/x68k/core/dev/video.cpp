// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include <cstdint>

#include "video.h"

namespace x68k
{
namespace
{

// 1 ラスタあたりの CPU サイクル。高解像度モードは 568 ラスタ。
constexpr u32 kRasterCount = 568;
constexpr u32 kCyclesPerRaster = Crtc::kCyclesPerFrame / kRasterCount;

}  // namespace

// --- CRTC -------------------------------------------------------------------

void Crtc::reset()
{
    reg_.fill(0);
    frameCycles_ = 0;
    inVBlank_ = false;
}

u16 Crtc::read(u32 regIndex) const
{
    return regIndex < kRegCount ? reg_[regIndex] : 0u;
}

void Crtc::write(u32 regIndex, u16 value)
{
    if (regIndex < kRegCount)
    {
        reg_[regIndex] = value;
    }
}

u32 Crtc::rasterNumber() const
{
    // フレーム内の位置から比例で求める。
    //
    // Why not frameCycles_ / kCyclesPerRaster とするか: kCyclesPerRaster は
    // kCyclesPerFrame / 568 の整数除算なので端数が出る。180342 / 568 = 317
    // だと 568 ラスタで 180056 サイクルにしかならず、余りの 286 サイクルが
    // 最終ラスタへ集中する。ラスタ 0-566 は 317 サイクル、567 だけ 603
    // サイクルという不均等な刻みになり、ラスタ割り込みが後半ほど早く出る。
    //
    // 先に掛けてから割れば端数がフレーム全体へ散り、商が 568 に達することも
    // ない (frameCycles_ < kCyclesPerFrame が保たれる限り)。
    return static_cast<u32>(static_cast<std::uint64_t>(frameCycles_) * kRasterCount /
                            kCyclesPerFrame);
}

bool Crtc::tickSlow(u32 cycles)
{
    // 1 フレーム以上をまとめて渡された場合と、計測のために速い側を
    // 切っている場合 (perf_switch.h) にここへ来る。
    //
    // どちらでも正しいよう、cycles の大小を仮定していない。1 フレーム
    // 未満なら剰余は素通りで、あとは速い側と同じ計算になる。
    //
    // Why not 剰余を速い側にも置かないか: ここは毎命令通る経路から呼ばれる。
    // ESP32-S3 では 64bit 除算が重く、素直に剰余へ変えたら実機の実効クロックが
    // 3.19MHz から 2.77MHz へ落ちた (実測)。命令 1 つのサイクル数は 1 フレームに
    // 遠く及ばないので、ヘッダ側は加算と比較だけで済む。
    //
    // 足す前に減らすのは u32 の桁溢れを避けるため。剰余のあとは
    // remaining < kCyclesPerFrame、frameCycles_ も同様なので、和は
    // 高々 2 倍で収まる。
    const u32 remaining = cycles % kCyclesPerFrame;
    frameCycles_ += remaining;
    if (frameCycles_ >= kCyclesPerFrame)
    {
        frameCycles_ -= kCyclesPerFrame;
    }

    // 表示期間の後ろに垂直帰線が来る。
    const bool nowVBlank = frameCycles_ >= (kCyclesPerFrame - kVBlankCycles);
    if (nowVBlank == inVBlank_)
    {
        return false;
    }
    inVBlank_ = nowVBlank;
    return true;
}

// --- ビデオコントローラ -----------------------------------------------------

void VideoController::reset()
{
    textPalette_.fill(0);
    graphicPalette_.fill(0);
    screenMode_ = 0;
    priority_ = 0;
    displayControl_ = 0;

    // テキストパレットの既定値。色 0 は透明 (黒)、色 1 以降が文字色。
    // IPL-ROM が SRAM の値で上書きするが、それ以前に何か描かれても
    // 真っ黒にならないよう白を入れておく。
    textPalette_[1] = 0xFFFF;
}

u16 VideoController::read(u32 addr) const
{
    const u32 offset = addr & 0x3FFFu;

    // $E82000-$E821FF: グラフィックパレット
    if (offset < 0x200)
    {
        return graphicPalette_[(offset / 2) & 0xFFu];
    }
    // $E82200-$E823FF: テキスト/スプライトパレット
    if (offset < 0x400)
    {
        return textPalette_[((offset - 0x200) / 2) & 0x0Fu];
    }

    switch (offset)
    {
        case 0x400:
            return screenMode_;
        case 0x500:
            return priority_;
        case 0x600:
            return displayControl_;
        default:
            return 0u;
    }
}

void VideoController::write(u32 addr, u16 value)
{
    const u32 offset = addr & 0x3FFFu;

    if (offset < 0x200)
    {
        graphicPalette_[(offset / 2) & 0xFFu] = value;
        return;
    }
    if (offset < 0x400)
    {
        textPalette_[((offset - 0x200) / 2) & 0x0Fu] = value;
        return;
    }

    switch (offset)
    {
        case 0x400:
            screenMode_ = value;
            return;
        case 0x500:
            priority_ = value;
            return;
        case 0x600:
            displayControl_ = value;
            return;
        default:
            return;
    }
}

u16 VideoController::toRgb565(u16 x68kColor)
{
    // X68000 の色は GGGGG RRRRR BBBBB I の並び。
    // 輝度ビット I は全色共通の最下位ビットとして働く。
    const u32 g5 = (x68kColor >> 11) & 0x1Fu;
    const u32 r5 = (x68kColor >> 6) & 0x1Fu;
    const u32 b5 = (x68kColor >> 1) & 0x1Fu;
    const u32 intensity = x68kColor & 1u;

    // 緑は RGB565 で 6bit あるので、輝度ビットを最下位に足して 6bit にする。
    const u32 g6 = (g5 << 1) | intensity;

    return static_cast<u16>((r5 << 11) | (g6 << 5) | b5);
}

}  // namespace x68k
