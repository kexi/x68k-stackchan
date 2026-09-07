// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "compositor.h"

#include "graphic_raster.h"
#include "sprite_raster.h"

namespace x68k
{

void Compositor::render(const u8* graphicVram, const u8* textVram, const Sprite* sprite,
                        const VideoController& video, u32 srcX, u32 srcY, u32 width, u32 height,
                        u16* out, u32 outStride, const Crtc* crtc)
{
    if (out == nullptr)
    {
        return;
    }

    // 先にグラフィックとテキストを重ねる。ここで背景が黒に塗られるので、
    // スプライト面は「透明でないドットだけ上書きする」形で乗せられる。
    //
    // グラフィック面もテキスト面も出ていないなら、composite がやるのは
    // 「全画面を黒で埋める」だけになる。BG が全面を覆うソフト
    // (BG とスプライトだけで絵を作るゲーム) では、その黒がそのまま
    // 上書きされて消えるので、埋める意味が無い。
    //
    // PSRAM 上のフレームバッファへの書き込みは高い。CoreS3 の実測で
    // 320x240 の 1 パスが無視できない重さだったので、要らないパスは飛ばす。
    const bool showGraphic = graphicVram != nullptr && video.graphicEnabled();
    const bool showText = textVram != nullptr && video.textEnabled();
    if (showGraphic || showText)
    {
        GraphicRaster::composite(graphicVram, textVram, video, srcX, srcY, width, height, out,
                                 outStride, crtc);
    }
    else
    {
        // どちらも出ないなら、背景を黒にするだけ。
        for (u32 y = 0; y < height; ++y)
        {
            u16* row = out + static_cast<std::size_t>(y) * outStride;
            for (u32 x = 0; x < width; ++x)
            {
                row[x] = 0;
            }
        }
    }

    // 出すものが無いなら触らない。
    //
    // Why not renderPlane() に任せて素通しするか: あちらも表示許可を見て
    // 早期に返るが、Human68k のコンソールのようにスプライトを一切使わない
    // 経路が毎フレームここを通る。呼び出しごとの分岐を 1 つ減らす意図より、
    // 「スプライトを使わないソフトの表示経路は以前と何も変わらない」ことを
    // 呼ぶ側から読み取れるようにしておく方を採る。
    const bool hasSprites = sprite != nullptr && SpriteRaster::hasVisibleContent(*sprite, video);
    if (!hasSprites)
    {
        return;
    }

    SpriteRaster::renderPlane(*sprite, video, srcX, srcY, width, height, out, outStride);
}

}  // namespace x68k
