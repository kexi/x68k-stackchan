// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "text_raster.h"

namespace x68k
{

u8 TextRaster::pixelIndex(const u8* vram, u32 x, u32 y)
{
    // テキスト VRAM は 1 ライン 128 バイト、1 バイトに 8 ドット。
    // 最上位ビットが左端。
    const u32 byteOffset = y * kTvramBytesPerLine + (x >> 3);
    const u32 bit = 7u - (x & 7u);
    const u8 mask = static_cast<u8>(1u << bit);

    // 4 プレーンの同じ位置を集める。プレーン 0 が最下位ビット。
    u8 index = 0;
    for (u32 plane = 0; plane < kTvramPlaneCount; ++plane)
    {
        const u8 byte = vram[plane * kTvramPlaneSize + byteOffset];
        if ((byte & mask) != 0)
        {
            index = static_cast<u8>(index | (1u << plane));
        }
    }
    return index;
}

void TextRaster::render(const u8* vram, const VideoController& video, u32 srcX, u32 srcY, u32 width,
                        u32 height, u16* out, u32 outStride)
{
    if (vram == nullptr || out == nullptr)
    {
        return;
    }

    // パレットを先に RGB565 へ変換しておく。ピクセルごとに変換すると
    // 同じ計算を何万回も繰り返すことになる。
    u16 palette[VideoController::kTextPaletteCount];
    for (u32 i = 0; i < VideoController::kTextPaletteCount; ++i)
    {
        palette[i] = VideoController::toRgb565(video.textPalette(i));
    }

    for (u32 y = 0; y < height; ++y)
    {
        const u32 vy = srcY + y;
        if (vy >= 1024)
        {
            break;  // テキスト画面は 1024 ライン
        }

        u16* row = out + static_cast<std::size_t>(y) * outStride;
        const u32 lineBase = vy * kTvramBytesPerLine;

        for (u32 x = 0; x < width; ++x)
        {
            const u32 vx = srcX + x;
            if (vx >= 1024)
            {
                break;
            }

            // pixelIndex を呼ぶ代わりに、ここでインライン展開する。
            // 1 ピクセルごとの関数呼び出しは変換全体の支配的なコストになる。
            const u32 byteOffset = lineBase + (vx >> 3);
            const u32 bit = 7u - (vx & 7u);
            const u8 mask = static_cast<u8>(1u << bit);

            u32 index = 0;
            if ((vram[byteOffset] & mask) != 0)
            {
                index |= 1u;
            }
            if ((vram[kTvramPlaneSize + byteOffset] & mask) != 0)
            {
                index |= 2u;
            }
            if ((vram[2 * kTvramPlaneSize + byteOffset] & mask) != 0)
            {
                index |= 4u;
            }
            if ((vram[3 * kTvramPlaneSize + byteOffset] & mask) != 0)
            {
                index |= 8u;
            }

            row[x] = palette[index];
        }
    }
}

}  // namespace x68k
