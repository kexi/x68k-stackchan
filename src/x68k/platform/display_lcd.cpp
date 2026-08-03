// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "display_lcd.h"

#include <M5Unified.h>
#include <esp_log.h>

#include "video/text_raster.h"

namespace x68k_platform
{

void DisplayLcd::begin()
{
    forceFullRedraw_ = true;

    M5.Display.setColorDepth(16);

    // バイト順の変換を通す。
    //
    // これが無いと M5GFX は「変換不要」と判断し、渡したポインタを
    // そのまま SPI の DMA descriptor に設定する (Panel_LCD.cpp の
    // no_convert 経路)。フレームバッファは PSRAM 上にあるため、
    // CPU がキャッシュへ書いた内容と GDMA が PSRAM から読む内容が
    // 食い違い、転送しても画面が更新されない。M5GFX 0.2.26 には
    // DMA 可能メモリかの判定も ESP32-S3 向けの esp_cache_msync も無い。
    //
    // 変換を挟むと M5GFX は内部の DMA 可能バッファへ画素を写してから
    // 送るので、この問題が起きない。
    //
    // Why not esp_cache_msync を自分で呼ぶか: M5GFX の DMA 経路は
    // 外部 RAM 判定もエラー処理も持たないため、同期しても descriptor の
    // 扱いは変わらない。内部バッファに寄せる方が確実。
    M5.Display.setSwapBytes(true);

    // 回転を明示する。CoreS3 の既定値と同じだが、依存を明示しておく。
    M5.Display.setRotation(1);

    ESP_LOGI("x68k.lcd", "LCD %dx%d rotation=%d colorDepth=%d", M5.Display.width(),
             M5.Display.height(), M5.Display.getRotation(), M5.Display.getColorDepth());

    M5.Display.fillScreen(TFT_BLACK);
}

void DisplayLcd::setZoom(x68k::u32 zoom)
{
    const x68k::u32 clamped = zoom < kMinZoom ? kMinZoom : (zoom > kMaxZoom ? kMaxZoom : zoom);
    if (zoom_ == clamped)
    {
        return;
    }
    zoom_ = clamped;
    // 拡大率が変わると同じ画素に別の内容が来る。全体を描き直さないと
    // 前の拡大率で描いた文字が下に残って層になる。
    forceFullRedraw_ = true;
}

void DisplayLcd::setViewport(x68k::u32 x, x68k::u32 y)
{
    if (viewX_ == x && viewY_ == y)
    {
        return;
    }
    viewX_ = x;
    viewY_ = y;
    // 切り出し位置が動くと同じ VRAM 行が LCD の別の位置に来る。
    // 部分更新では古い描画が残るので全体を描き直す。
    forceFullRedraw_ = true;
}

void DisplayLcd::renderZoomed(x68k::Machine& machine, const x68k::u8* textVram, x68k::u16* out)
{
    const x68k::u32 srcWidth = kScreenWidth / zoom_;
    const x68k::u32 srcHeight = kScreenHeight / zoom_;

    // いったん左上の srcWidth x srcHeight に等倍で描いてから、
    // 右下から引き伸ばす。バッファを 1 枚で済ませるため、
    // 上書きされる前の画素を先に読む向き (右下→左上) で回す。
    x68k::TextRaster::render(textVram, machine.video(), viewX_, viewY_, srcWidth, srcHeight, out,
                             kScreenWidth);

    for (x68k::u32 y = srcHeight; y-- > 0;)
    {
        const x68k::u16* srcRow = out + static_cast<std::size_t>(y) * kScreenWidth;
        for (x68k::u32 sy = zoom_; sy-- > 0;)
        {
            x68k::u16* destRow = out + static_cast<std::size_t>(y * zoom_ + sy) * kScreenWidth;
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
}

bool DisplayLcd::renderTo(x68k::Machine& machine, const x68k::u8* textVram, x68k::u16* out)
{
    if (textVram == nullptr || out == nullptr)
    {
        return false;
    }

    auto& bus = machine.bus();

    // 変化が無ければ作り直さない。プロンプトが点滅しているだけなら
    // ここで抜けるので、Core1 の時間をエミュレーションに回せる。
    const bool needsRedraw = forceFullRedraw_ || bus.anyTextDirty();
    if (!needsRedraw)
    {
        return false;
    }

    const bool isZoomed = zoom_ > 1;
    if (isZoomed)
    {
        renderZoomed(machine, textVram, out);
    }
    else
    {
        // 全画面を作り直す。
        //
        // Why not ダーティ行だけ変換するか: ダーティ行は「テキスト VRAM の
        // どこが変わったか」しか表さない。切り出し位置 (viewX_/viewY_) が
        // 動くと同じ VRAM 行が LCD の別の位置に来るので、変わっていない行の
        // 古い描画が残り、新しい描画と二重に見える。実機で「A> が 2 個
        // 出る」形で露見した。320x240 の変換なら実測で間に合う。
        x68k::TextRaster::render(textVram, machine.video(), viewX_, viewY_, kScreenWidth,
                                 kScreenHeight, out, kScreenWidth);
    }

    bus.clearTextDirty();
    forceFullRedraw_ = false;
    return true;
}

void DisplayLcd::pushFrame(const x68k::u16* frame)
{
    if (frame == nullptr)
    {
        return;
    }

    // 前の転送が終わるのを待ってから送る。
    //
    // pushImageDMA は転送の完了を待たずに戻る。待たずに次を送ると
    // 転送中のバッファを別の内容で上書きすることになり、LCD には
    // 新旧が混ざった画面が出る。
    M5.Display.waitDMA();
    M5.Display.pushImageDMA(0, 0, static_cast<int>(kScreenWidth), static_cast<int>(kScreenHeight),
                            const_cast<x68k::u16*>(frame));
    M5.Display.waitDMA();
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
