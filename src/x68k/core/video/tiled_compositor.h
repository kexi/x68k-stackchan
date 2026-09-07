// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_VIDEO_TILED_COMPOSITOR_H
#define X68K_CORE_VIDEO_TILED_COMPOSITOR_H

#include "compositor.h"
#include "tile_generations.h"
#include "visual_damage.h"

namespace x68k
{
// 等倍320x240専用。viewport/出力bufferの対応は呼び出し側で固定する。
// 描画中にゲストを実行しない。公開成否と合成済み世代は独立。
class TiledCompositor
{
public:
    TiledCompositor() = default;
    TiledCompositor(const TiledCompositor&) = delete;
    TiledCompositor& operator=(const TiledCompositor&) = delete;

    VisualDamage observer()
    {
        return {this, [](void* context, std::int32_t x, std::int32_t y, std::int32_t width,
                         std::int32_t height)
                { static_cast<TiledCompositor*>(context)->invalidate(x, y, width, height); }};
    }

    void invalidateAll()
    {
        fullPending_ = true;
    }

    void setViewport(u32 x, u32 y)
    {
        const bool changed = x != viewX_ || y != viewY_;
        if (changed)
        {
            viewX_ = x;
            viewY_ = y;
            invalidateAll();
        }
    }

    u32 render(const u8* graphic, const u8* text, const Sprite* sprite,
               const VideoController& video, u16* out, u32 buffer, const Crtc* crtc = nullptr)
    {
        const bool invalid = out == nullptr || buffer >= TileGenerations<>::kBuffers;
        if (invalid)
        {
            return 0;
        }
        const bool direct = crtc != nullptr && video.graphicColorMode() ==
                                                   VideoController::GraphicColorMode::k65536Color;
        const u32 scrollKey =
            direct ? (0x40000u | crtc->graphicScrollX() | (crtc->graphicScrollY() << 9)) : 0u;
        const bool scrollChanged = scrollKey != scrollKey_;
        if (scrollChanged)
        {
            // MMIO だけの page flip も両バッファへ届く。observer 登録に依存しない。
            scrollKey_ = scrollKey;
            invalidateAll();
        }
        if (fullPending_)
        {
            generations_.invalidateAll();
            fullPending_ = false;
        }
        u32 count = 0;
        for (u32 tile = 0; tile < TileGenerations<>::kTiles; ++tile)
        {
            count += generations_.needsRender(buffer, tile) ? 1u : 0u;
        }
        const bool full = count == TileGenerations<>::kTiles;
        if (full)
        {
            // 全面変更で300回palette/全spriteを走査する費用は払わない。
            Compositor::render(graphic, text, sprite, video, viewX_, viewY_, 320, 240, out, 320,
                               crtc);
            for (u32 tile = 0; tile < TileGenerations<>::kTiles; ++tile)
            {
                generations_.rendered(buffer, tile);
            }
            return count;
        }
        for (u32 row = 0; row < TileGenerations<>::kRows; ++row)
        {
            u32 column = 0;
            while (column < TileGenerations<>::kColumns)
            {
                const u32 tile = row * TileGenerations<>::kColumns + column;
                const bool dirty = generations_.needsRender(buffer, tile);
                if (!dirty)
                {
                    ++column;
                    continue;
                }
                const u32 first = column++;
                while (column < TileGenerations<>::kColumns &&
                       generations_.needsRender(buffer, row * TileGenerations<>::kColumns + column))
                {
                    ++column;
                }
                const u32 x = first * 16u;
                const u32 y = row * 16u;
                Compositor::render(graphic, text, sprite, video, viewX_ + x, viewY_ + y,
                                   (column - first) * 16u, 16, out + y * 320u + x, 320, crtc);
                for (u32 done = first; done < column; ++done)
                {
                    generations_.rendered(buffer, row * TileGenerations<>::kColumns + done);
                }
            }
        }
        return count;
    }

private:
    void invalidate(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
    {
        const bool full = width == 0;
        if (full)
        {
            invalidateAll();
        }
        if (fullPending_)
        {
            return;
        }
        const auto left = std::int64_t{x} - viewX_;
        const auto top = std::int64_t{y} - viewY_;
        const bool outside = left >= 320 || top >= 240 || left + width <= 0 || top + height <= 0;
        if (outside)
        {
            return;
        }
        // 交差がある矩形だけなので、クリップ後の値は画面内に収まる。
        const auto clippedLeft = std::max<std::int64_t>(0, left);
        const auto clippedTop = std::max<std::int64_t>(0, top);
        generations_.invalidateRect(
            static_cast<std::int32_t>(clippedLeft), static_cast<std::int32_t>(clippedTop),
            static_cast<std::int32_t>(std::min<std::int64_t>(320, left + width) - clippedLeft),
            static_cast<std::int32_t>(std::min<std::int64_t>(240, top + height) - clippedTop));
    }

    TileGenerations<> generations_{};
    u32 viewX_ = 0;
    u32 viewY_ = 0;
    u32 scrollKey_ = 0;
    bool fullPending_ = true;
};
}  // namespace x68k
#endif
