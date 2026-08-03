// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// テキスト VRAM に描かれている内容を ASCII として読み戻す。
//
// X68000 のテキスト画面はキャラクタ VRAM ではなくビットマップなので、
// 「どの文字が書かれたか」は VRAM を見ても直接は分からない。
// そこで CGROM の 8x16 ANK 字形と 1 セルずつ照合して逆に引く。
//
// 用途は動作確認。実機の LCD は 320x240 しかなく Human68k のコンソール
// (768x512) の一部しか映らないため、画面に何が出ているかを
// シリアルログへ出して確かめる手段が要る。
//
// Why not 画面を PPM で吸い出して外部で OCR するか: 実機では画像を
// 転送する経路がなく、シリアルへ 768x512 のビットマップを流すのは遅すぎる。
// 逆引きなら 1 行 96 バイトで済む。

#ifndef X68K_CORE_VIDEO_TEXT_SCRAPE_H
#define X68K_CORE_VIDEO_TEXT_SCRAPE_H

#include <cstddef>

#include "../cpu/m68k_types.h"

namespace x68k
{

class TextScrape
{
public:
    // ANK は 8x16。Human68k の標準コンソールは 96 桁 x 32 行。
    static constexpr u32 kCellWidth = 8;
    static constexpr u32 kCellHeight = 16;
    static constexpr u32 kColumns = 96;
    static constexpr u32 kRows = 32;

    // 1 行ぶんを ASCII として読み出す。
    //
    //   vram  : テキスト VRAM の先頭
    //   cgrom : CGROM (768KB)。8x16 ANK は $3A800 から
    //   row   : 行番号 (0 起点)
    //   out   : 書き込み先。kColumns + 1 バイト必要 (終端 '\0' を書く)
    //
    // 該当する字形が無いセルは、空白なら ' '、それ以外は '?' になる。
    // 行末の空白は詰める。
    static void readRow(const u8* vram, const u8* cgrom, u32 row, char* out);

    // 1 セルが空 (全プレーンが 0) かどうか。
    [[nodiscard]] static bool isCellBlank(const u8* vram, u32 column, u32 row);

private:
    // セルの 16 バイト (8x16 の 1bit ビットマップ) を取り出す。
    //
    // テキスト画面は 4 プレーンあるが、Human68k のコンソールは
    // プレーン 0 だけで文字を描く。色を変えている箇所では他プレーンにも
    // 同じ形が乗るので、プレーン 0 で足りる。
    static void readCell(const u8* vram, u32 column, u32 row, u8* cell);
};

}  // namespace x68k

#endif  // X68K_CORE_VIDEO_TEXT_SCRAPE_H
