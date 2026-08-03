// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "cgrom_fallback.h"

namespace x68k
{
namespace
{

// 6x12 の字形を 8x16 のコマのどこに置くか。
//
// 6x12 フォントは 12 ラインのうち上 2 ラインと下 1 ラインをほぼ空けており、
// 実質の字形は 9 ライン程度。8x16 のコマへ 2 ライン下げて置くと、CGROM の
// 8x16 ANK とベースラインがだいたい揃う。
constexpr u32 kVerticalOffset = 2u;

}  // namespace

void buildCgromFromIplRom(const u8* iplRom, u8* out)
{
    if (iplRom == nullptr || out == nullptr)
    {
        return;
    }

    // 字形を置かない領域 (漢字など) は 0 のままにする。
    //
    // Why not 0xFF で埋めないか: 0 のままなら「字形が無い文字」は空白として
    // 出る。ベタ塗りの矩形が並ぶより、何が出ていないのかが分かりやすい。
    for (std::size_t i = 0; i < kCgromFallbackSize; ++i)
    {
        out[i] = 0u;
    }

    const u32 fontOffset = kIplromAnk6x12Base - kIplromBase;

    for (u32 code = 0; code < kCgromAnk8x16Glyphs; ++code)
    {
        if (code >= kIplromAnk6x12Glyphs)
        {
            break;
        }

        const u32 srcBase = fontOffset + code * kAnk6x12BytesPerGlyph;
        const u32 dstBase = kCgromAnk8x16Offset + code * kCgromAnk8x16Height;

        for (u32 line = 0; line < kAnk6x12Height; ++line)
        {
            const u32 dstLine = line + kVerticalOffset;
            if (dstLine >= kCgromAnk8x16Height)
            {
                break;
            }
            // 6x12 も 8x16 も「最上位ビットが左端」で揃っているので、
            // 1 バイトをそのまま写せば左寄せで置ける。下位 2bit は
            // 6 ドット幅のフォント側で常に 0 なので、右 2 ドットは自然に空く。
            out[dstBase + dstLine] = iplRom[srcBase + line];
        }
    }
}

}  // namespace x68k
