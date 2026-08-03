// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "text_scrape.h"

#include <cstring>

#include "../memmap.h"

namespace x68k
{

namespace
{

// CGROM の 8x16 ANK は $3A800 から、1 文字 16 バイトで並ぶ。
constexpr u32 kAnk8x16Offset = 0x3A800;
constexpr u32 kAnk8x16Bytes = 16;
constexpr u32 kAnkCount = 256;

}  // namespace

void TextScrape::readCell(const u8* vram, u32 column, u32 row, u8* cell)
{
    const u32 x = column * kCellWidth;
    const u32 y = row * kCellHeight;

    for (u32 line = 0; line < kCellHeight; ++line)
    {
        // 1 ライン 128 バイト、1 バイト 8 ドット。セル幅がちょうど 8 なので
        // バイト境界に揃い、シフトなしで 1 バイトを取れる。
        const u32 offset = (y + line) * kTvramBytesPerLine + (x >> 3);
        cell[line] = vram[offset];
    }
}

bool TextScrape::isCellBlank(const u8* vram, u32 column, u32 row)
{
    u8 cell[kCellHeight];
    readCell(vram, column, row, cell);
    for (u32 line = 0; line < kCellHeight; ++line)
    {
        if (cell[line] != 0)
        {
            return false;
        }
    }
    return true;
}

void TextScrape::readRow(const u8* vram, const u8* cgrom, u32 row, char* out)
{
    if (vram == nullptr || cgrom == nullptr || out == nullptr || row >= kRows)
    {
        if (out != nullptr)
        {
            out[0] = '\0';
        }
        return;
    }

    const u8* ank = cgrom + kAnk8x16Offset;

    u32 lastNonBlank = 0;
    bool hasContent = false;

    for (u32 column = 0; column < kColumns; ++column)
    {
        u8 cell[kCellHeight];
        readCell(vram, column, row, cell);

        bool isBlank = true;
        for (u32 line = 0; line < kCellHeight; ++line)
        {
            if (cell[line] != 0)
            {
                isBlank = false;
                break;
            }
        }

        if (isBlank)
        {
            out[column] = ' ';
            continue;
        }

        // 字形を総当たりで照合する。ANK は 256 通りしかないので、
        // 表示のたびに引いても十分に速い。
        //
        // Why not コードから字形へのハッシュを作るか: 逆引きは動作確認
        // でしか使わない。索引を持つと CGROM を差し替えたときに
        // 作り直す手間が増える。
        char found = 0;
        for (u32 code = 0x20; code < kAnkCount; ++code)
        {
            if (std::memcmp(cell, ank + code * kAnk8x16Bytes, kCellHeight) == 0)
            {
                found = static_cast<char>(code);
                break;
            }
        }

        // ANK に無ければ 16x16 の漢字の左半分か右半分。
        //
        // 漢字は 2 セルにまたがるので、ANK と同じ照合では当たらない。
        // どの字かまでは分からなくてよく、「漢字がある」ことが分かれば
        // 表示できているかの判断はつく。日本語が出ていない場合はここが
        // 空白のままになる。
        const bool isAnk = found != 0;
        out[column] = isAnk ? found : '#';
        lastNonBlank = column;
        hasContent = true;
    }

    // 行末の空白は詰める。96 桁ぶんの空白がログを埋めると読めない。
    const u32 length = hasContent ? lastNonBlank + 1 : 0;
    out[length] = '\0';
}

}  // namespace x68k
