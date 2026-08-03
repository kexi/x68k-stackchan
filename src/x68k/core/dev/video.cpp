// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

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
    // 範囲に収める。
    //
    // kCyclesPerRaster は kCyclesPerFrame / 568 の整数除算なので切り捨てが
    // 起きる。frameCycles_ がフレーム終端に近いと商が 568 以上になり、
    // 実機には存在しないラスタ番号を返す。ラスタ割り込みの比較や
    // 「今どの行を描いているか」の判定で 1 フレームぶんずれる。
    const u32 raster = frameCycles_ / kCyclesPerRaster;
    return raster < kRasterCount ? raster : kRasterCount - 1;
}

bool Crtc::tick(u32 cycles)
{
    frameCycles_ += cycles;
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
