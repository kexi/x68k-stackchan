// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: IPL-ROM 内蔵の 6x12 ANK フォントから組み立てた代替 CGROM が、
// IOCS が読みに来る位置 ($F3A800 + コード * 16) に字形を置いていること。
//
// ここが 1 文字ぶんでもずれると、画面には「文字らしきもの」が出るのに
// 読めないという状態になり、CPU やテキスト VRAM のバグと区別がつかない。
// 実際に IPL-ROM を読ませずに済むよう、フォントを自前で合成して検証する。

#include <vector>

#include "doctest.h"
#include "video/cgrom_fallback.h"

namespace
{

// テスト用の IPL-ROM を作る。フォント領域だけ既知の値で埋める。
//
// 文字コード c のライン l に (c + l) を 2bit 左シフトした値を置く。
// 下位 2bit を 0 にしてあるのは、実物の 6x12 フォントが 6 ドット幅で
// 下位 2bit を使わないため。
std::vector<x68k::u8> makeIplRomWithFont()
{
    std::vector<x68k::u8> rom(x68k::kIplromSize, 0x00);
    const x68k::u32 fontOffset = x68k::kIplromAnk6x12Base - x68k::kIplromBase;

    for (x68k::u32 code = 0; code < x68k::kIplromAnk6x12Glyphs; ++code)
    {
        for (x68k::u32 line = 0; line < x68k::kAnk6x12Height; ++line)
        {
            const auto value = static_cast<x68k::u8>(((code + line) & 0x3Fu) << 2u);
            rom[fontOffset + code * x68k::kAnk6x12BytesPerGlyph + line] = value;
        }
    }
    return rom;
}

// 代替 CGROM から文字コード c の 8x16 字形を取り出す。
std::vector<x68k::u8> glyphOf(const std::vector<x68k::u8>& cgrom, x68k::u32 code)
{
    const x68k::u32 base = x68k::kCgromAnk8x16Offset + code * x68k::kCgromAnk8x16Height;
    return {cgrom.begin() + base, cgrom.begin() + base + x68k::kCgromAnk8x16Height};
}

}  // namespace

TEST_CASE("IPL-ROM 内蔵フォントの位置と大きさが実測値と一致する")
{
    // rom/iplrom.dat (EXPERT 用 v1.0) を実際に読んで確かめた値。
    // 資料にある $FFCFF6 は命令列の位置で、フォントはその $22 バイト後ろ。
    CHECK(x68k::kIplromAnk6x12Base == 0x00FFD018u);
    CHECK(x68k::kIplromAnk6x12Glyphs == 254u);
    CHECK(x68k::kAnk6x12BytesPerGlyph == 12u);

    // フォント末尾が IPL-ROM の中に収まっていること。
    const x68k::u32 endAddr =
        x68k::kIplromAnk6x12Base + x68k::kIplromAnk6x12Glyphs * x68k::kAnk6x12BytesPerGlyph;
    CHECK(endAddr <= x68k::kIplromEnd);
}

TEST_CASE("代替 CGROM の ANK 位置は IOCS が読みに来るアドレスと一致する")
{
    // 実測: 起動中の Human68k は $F3A800 + コード * 16 を読んでいた。
    CHECK(x68k::kCgromBase + x68k::kCgromAnk8x16Offset == 0x00F3A800u);
    CHECK(x68k::kCgromAnk8x16Height == 16u);

    // 256 文字ぶんの ANK テーブルが CGROM の中に収まっていること。
    const x68k::u32 end =
        x68k::kCgromAnk8x16Offset + x68k::kCgromAnk8x16Glyphs * x68k::kCgromAnk8x16Height;
    CHECK(end <= x68k::kCgromSize);
}

TEST_CASE("6x12 の字形が 8x16 のコマへ左寄せ・2 ライン下げで写る")
{
    const std::vector<x68k::u8> rom = makeIplRomWithFont();
    std::vector<x68k::u8> cgrom(x68k::kCgromFallbackSize, 0xAAu);
    x68k::buildCgromFromIplRom(rom.data(), cgrom.data());

    const x68k::u32 code = 'A';
    const std::vector<x68k::u8> glyph = glyphOf(cgrom, code);

    // 上 2 ラインは空く。
    CHECK(glyph[0] == 0u);
    CHECK(glyph[1] == 0u);

    // 6x12 の 12 ラインが 2..13 に入る。
    for (x68k::u32 line = 0; line < x68k::kAnk6x12Height; ++line)
    {
        const auto expected = static_cast<x68k::u8>(((code + line) & 0x3Fu) << 2u);
        CHECK(glyph[line + 2] == expected);
    }

    // 下 2 ラインも空く (12 + 2 = 14 なので 14, 15 が残る)。
    CHECK(glyph[14] == 0u);
    CHECK(glyph[15] == 0u);
}

TEST_CASE("字形の無い領域は 0 で埋まる")
{
    const std::vector<x68k::u8> rom = makeIplRomWithFont();
    std::vector<x68k::u8> cgrom(x68k::kCgromFallbackSize, 0xAAu);
    x68k::buildCgromFromIplRom(rom.data(), cgrom.data());

    // 漢字フォントの領域 ($F00000 台の先頭) には何も置かない。
    // 0 のままなら空白として出るので、ベタ塗りの矩形より状況が読める。
    for (std::size_t i = 0; i < 256; ++i)
    {
        CHECK(cgrom[i] == 0u);
    }

    // 254 文字を超えるコードには字形が無い。
    const std::vector<x68k::u8> beyond = glyphOf(cgrom, x68k::kIplromAnk6x12Glyphs);
    for (const auto b : beyond)
    {
        CHECK(b == 0u);
    }
}

TEST_CASE("ヌルポインタを渡しても落ちない")
{
    std::vector<x68k::u8> cgrom(x68k::kCgromFallbackSize, 0xAAu);
    x68k::buildCgromFromIplRom(nullptr, cgrom.data());
    // 何も書かれない。
    CHECK(cgrom[0] == 0xAAu);

    const std::vector<x68k::u8> rom = makeIplRomWithFont();
    x68k::buildCgromFromIplRom(rom.data(), nullptr);
}
