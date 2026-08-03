// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "display_lcd.h"

#include <M5Unified.h>
#include <esp_log.h>

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

    // 回転を明示する。
    //
    // CoreS3 は既定で rotation=1 になっており、この状態で pushImageDMA に
    // 生の画素を渡すと座標が読み替えられ、実機では上下が入れ替わって
    // 表示された。エミュレータ側は 320x240 の並びで画素を作っているので、
    // パネルの向きをそれに合わせる。
    M5.Display.setRotation(1);

    // パネルの実寸と回転を記録する。想定 (320x240) と食い違うと
    // 画面が右へずれたり上下が切れたりする。
    ESP_LOGI("x68k.lcd", "LCD %dx%d rotation=%d colorDepth=%d", M5.Display.width(),
             M5.Display.height(), M5.Display.getRotation(), M5.Display.getColorDepth());

    // 画面を消す経路と描く経路を pushImageDMA に統一する。
    //
    // Why not fillScreen を使うか: 実機で fillScreen が LCD へ届かない
    // 場面があった。pushImageDMA と混在させると SPI のトランザクションが
    // 噛み合わないらしい。片方に寄せれば食い違いが起きない。
    clearScreen();
}

void DisplayLcd::setZoom(x68k::u32 zoom)
{
    const x68k::u32 clamped = zoom < 1 ? 1 : (zoom > 4 ? 4 : zoom);
    if (zoom_ == clamped)
    {
        return;
    }
    zoom_ = clamped;
    // 拡大率が変わると同じ画素に別の内容が来る。消さずに描き直すと、
    // 前の拡大率で描いた文字が下に残って層になる。
    clearScreen();
}

void DisplayLcd::forceClear()
{
    clearScreen();
}

void DisplayLcd::clearScreen()
{
    forceFullRedraw_ = true;

    if (buffer_ == nullptr)
    {
        return;
    }

    // バッファを黒で埋めて送る。
    //
    // Why not fillScreen を使うか: 実機で fillScreen が反映されなかった。
    // pushImageDMA と混在させると SPI のトランザクションが噛み合わず、
    // 塗り潰しが LCD へ届かないまま次の転送が始まるらしい。
    // 転送経路を pushImageDMA だけに統一すれば、この食い違いが起きない。
    const std::size_t pixels = static_cast<std::size_t>(kScreenWidth) * kScreenHeight;
    M5.Display.waitDMA();
    for (std::size_t i = 0; i < pixels; ++i)
    {
        buffer_[i] = 0;
    }

    M5.Display.pushImageDMA(0, 0, static_cast<int>(kScreenWidth), static_cast<int>(kScreenHeight),
                            buffer_);
    M5.Display.waitDMA();
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

    // 前回の DMA 転送が終わるのを待ってから送る。
    //
    // pushImageDMA は転送の完了を待たずに戻る。待たずに次のフレームを
    // 作ると、転送中のバッファを上書きすることになり、LCD には新旧が
    // 混ざった画面が出る。等倍は行単位に分けて送るので目立たなかったが、
    // 拡大は毎回 150KB を 1 回で送るため確実に競合する。
    M5.Display.waitDMA();
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
    // 表示範囲が変わったら全体を描き直す。
    //
    // Why not 消してから描くか: present は毎回全画面を送るので、
    // 描き直せば古い内容は必ず上書きされる。消す一手間を挟むと
    // 位置が動くたびに黒い画面が 1 フレーム挟まって明滅する。
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
        M5.Display.waitDMA();
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

    // 変化があれば全画面を作り直す。
    //
    // Why not ダーティ行だけ送るか: ダーティ行は「テキスト VRAM のどこが
    // 変わったか」しか表さない。切り出し位置 (viewX_/viewY_) が動くと
    // 同じ VRAM 行が LCD の別の位置に来るので、変わっていない行の古い
    // 描画が LCD に残り、新しい描画と二重に見える。実機で「A> が 2 個
    // 出る」形で露見した。
    //
    // 全画面でも 320x240 の変換で足りており、実測で間に合う。
    // 転送量を削るのは、切り出し位置を固定できるようになってからでよい。
    x68k::TextRaster::render(textVram, machine.video(), viewX_, viewY_, kScreenWidth, kScreenHeight,
                             buffer_, kScreenWidth);
    M5.Display.waitDMA();
    M5.Display.pushImageDMA(0, 0, static_cast<int>(kScreenWidth), static_cast<int>(kScreenHeight),
                            buffer_);
    bus.clearTextDirty();
}

// 以下はダーティ行だけを送る実装。切り出し位置が動くと使えないので
// 現在は呼んでいない。表示位置を固定する運用に戻すときに復活させる。
void DisplayLcd::presentDirtyRowsOnly(x68k::Machine& machine, const x68k::u8* textVram)
{
    auto& bus = machine.bus();

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
        // 描く前に前回の転送を待つ。同じバッファを使い回すので、
        // 転送中に render で上書きすると LCD に古い内容が残る。
        M5.Display.waitDMA();
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
