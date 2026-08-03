// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "display_lcd.h"

#include <M5Unified.h>

#include "video/text_raster.h"

namespace x68k_platform
{
namespace
{

// ダーティ管理の粒度。バスが記録するタイル行の高さと揃える。
constexpr x68k::u32 kTileHeight = x68k::SystemBus::kDirtyTileHeight;

}  // namespace

void DisplayLcd::begin(x68k::u16* buffer)
{
    buffer_ = buffer;
    forceFullRedraw_ = true;

    M5.Display.setColorDepth(16);
    M5.Display.fillScreen(TFT_BLACK);
}

void DisplayLcd::setZoom(x68k::u32 zoom)
{
    const x68k::u32 clamped = zoom < 1 ? 1 : (zoom > 4 ? 4 : zoom);
    if (zoom_ == clamped)
    {
        return;
    }
    zoom_ = clamped;
    forceFullRedraw_ = true;
}

void DisplayLcd::renderZoomed(x68k::Machine& machine, const x68k::u8* textVram)
{
    // 拡大時は画面全体を作り直す。
    //
    // Why not ダーティ行だけ送るか: ダーティ管理はテキスト VRAM の
    // タイル行を単位にしており、拡大すると 1 タイル行が LCD 上の
    // 複数行に伸びる。対応を取り直すより、変換元が 1/zoom^2 に減ることを
    // 使って全画面を作り直す方が単純で、実測でも間に合う。
    const x68k::u32 srcWidth = kScreenWidth / zoom_;
    const x68k::u32 srcHeight = kScreenHeight / zoom_;

    // いったん左上の srcWidth x srcHeight に等倍で描いてから、
    // 右下から引き伸ばす。バッファを 1 枚で済ませるため、
    // 上書きされる前の画素を先に読む向き (右下→左上) で回す。
    x68k::TextRaster::render(textVram, machine.video(), viewX_, viewY_, srcWidth, srcHeight,
                             buffer_, kScreenWidth);

    for (x68k::u32 y = srcHeight; y-- > 0;)
    {
        const x68k::u16* srcRow = buffer_ + static_cast<std::size_t>(y) * kScreenWidth;
        for (x68k::u32 sy = zoom_; sy-- > 0;)
        {
            x68k::u16* destRow = buffer_ + static_cast<std::size_t>(y * zoom_ + sy) * kScreenWidth;
            for (x68k::u32 x = srcWidth; x-- > 0;)
            {
                const x68k::u16 c = srcRow[x];
                for (x68k::u32 sx = 0; sx < zoom_; ++sx)
                {
                    destRow[x * zoom_ + sx] = c;
                }
            }
        }
    }

    M5.Display.pushImageDMA(0, 0, static_cast<int>(kScreenWidth), static_cast<int>(kScreenHeight),
                            buffer_);
}

void DisplayLcd::setViewport(x68k::u32 x, x68k::u32 y)
{
    if (viewX_ == x && viewY_ == y)
    {
        return;
    }
    viewX_ = x;
    viewY_ = y;
    // 表示範囲が変わったらダーティ情報は当てにならない。
    forceFullRedraw_ = true;
}

void DisplayLcd::present(x68k::Machine& machine, const x68k::u8* textVram)
{
    if (buffer_ == nullptr || textVram == nullptr)
    {
        return;
    }

    auto& bus = machine.bus();

    const bool isZoomed = zoom_ > 1;
    if (isZoomed)
    {
        // 拡大時はダーティ管理を使わない。変化が無ければ送らない点は同じ。
        if (!forceFullRedraw_ && !bus.anyTextDirty())
        {
            return;
        }
        renderZoomed(machine, textVram);
        bus.clearTextDirty();
        forceFullRedraw_ = false;
        return;
    }

    if (forceFullRedraw_)
    {
        x68k::TextRaster::render(textVram, machine.video(), viewX_, viewY_, kScreenWidth,
                                 kScreenHeight, buffer_, kScreenWidth);
        M5.Display.pushImageDMA(0, 0, static_cast<int>(kScreenWidth),
                                static_cast<int>(kScreenHeight), buffer_);
        bus.clearTextDirty();
        forceFullRedraw_ = false;
        return;
    }

    if (!bus.anyTextDirty())
    {
        return;
    }

    // 表示している範囲に対応するタイル行だけを見る。
    const x68k::u32 firstRow = viewY_ / kTileHeight;
    const x68k::u32 lastRow = (viewY_ + kScreenHeight - 1) / kTileHeight;

    // 連続するダーティ行はまとめて 1 回で送る。
    // 矩形ごとにアドレスウィンドウの設定コマンドが要るので、
    // 細切れに送るとコマンドのオーバヘッドが効いてくる。
    x68k::u32 runStart = 0;
    bool inRun = false;

    const auto flushRun = [&](x68k::u32 endRowExclusive)
    {
        if (!inRun)
        {
            return;
        }
        // タイル行 → 画面上の Y 範囲へ。
        const x68k::u32 srcYStart = runStart * kTileHeight;
        const x68k::u32 srcYEnd = endRowExclusive * kTileHeight;

        // ビューポートからはみ出す分を切り詰める。
        const x68k::u32 clampedStart = srcYStart < viewY_ ? viewY_ : srcYStart;
        const x68k::u32 viewBottom = viewY_ + kScreenHeight;
        const x68k::u32 clampedEnd = srcYEnd > viewBottom ? viewBottom : srcYEnd;
        if (clampedStart >= clampedEnd)
        {
            inRun = false;
            return;
        }

        const x68k::u32 destY = clampedStart - viewY_;
        const x68k::u32 height = clampedEnd - clampedStart;

        x68k::u16* dest = buffer_ + static_cast<std::size_t>(destY) * kScreenWidth;
        x68k::TextRaster::render(textVram, machine.video(), viewX_, clampedStart, kScreenWidth,
                                 height, dest, kScreenWidth);
        M5.Display.pushImageDMA(0, static_cast<int>(destY), static_cast<int>(kScreenWidth),
                                static_cast<int>(height), dest);
        inRun = false;
    };

    for (x68k::u32 row = firstRow; row <= lastRow; ++row)
    {
        if (bus.isTextRowDirty(row))
        {
            if (!inRun)
            {
                runStart = row;
                inRun = true;
            }
            continue;
        }
        flushRun(row);
    }
    flushRun(lastRow + 1);

    bus.clearTextDirty();
}

void DisplayLcd::showMessage(const char* line1, const char* line2)
{
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 60);
    M5.Display.println(line1);
    if (line2 != nullptr)
    {
        M5.Display.setTextSize(1);
        M5.Display.setCursor(8, 100);
        M5.Display.println(line2);
    }
}

}  // namespace x68k_platform
