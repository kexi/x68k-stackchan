// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: テキスト VRAM に描かれた 8x16 の字形を、CGROM と照合して
// 元の文字コードへ引き戻せること。
//
// この逆引きは実機のデバッグの土台になる。実機の LCD は 320x240 しかなく
// Human68k のコンソール (768x512) の一部しか映らないため、「画面に何が
// 出ているか」はこれを通してしか確かめられない。ここが狂うと、実機の
// 不具合を見ているのか逆引きの不具合を見ているのか区別できなくなる。

#include <cstring>
#include <vector>

#include "doctest.h"
#include "io/ascii_keymap.h"
#include "memmap.h"
#include "video/text_scrape.h"

namespace
{

constexpr x68k::u32 kAnk8x16Offset = 0x3A800;
constexpr x68k::u32 kAnk8x16Bytes = 16;

// 文字コードごとに異なる字形を持つ CGROM を作る。
//
// ライン l に (code + l) を置く。隣り合うコードでも全 16 ラインが
// 一致することはないので、照合の取り違えが起きれば test が落ちる。
std::vector<x68k::u8> makeCgrom()
{
    std::vector<x68k::u8> cgrom(x68k::kCgromSize, 0x00);
    for (x68k::u32 code = 0x20; code < 0x100; ++code)
    {
        for (x68k::u32 line = 0; line < kAnk8x16Bytes; ++line)
        {
            cgrom[kAnk8x16Offset + code * kAnk8x16Bytes + line] =
                static_cast<x68k::u8>((code + line) & 0xFFu);
        }
    }
    return cgrom;
}

// テキスト VRAM の (column, row) のセルへ、その文字コードの字形を書く。
//
// プレーン 0 にだけ書く。Human68k のコンソールが実際にそうしている。
void putChar(std::vector<x68k::u8>& vram, const std::vector<x68k::u8>& cgrom, x68k::u32 column,
             x68k::u32 row, char c)
{
    const auto code = static_cast<x68k::u32>(static_cast<unsigned char>(c));
    for (x68k::u32 line = 0; line < x68k::TextScrape::kCellHeight; ++line)
    {
        const x68k::u32 y = row * x68k::TextScrape::kCellHeight + line;
        const x68k::u32 offset = y * x68k::kTvramBytesPerLine + column;
        vram[offset] = cgrom[kAnk8x16Offset + code * kAnk8x16Bytes + line];
    }
}

// 16x16 の漢字が置かれた状態を作る。ANK のどの字形とも一致しない値を書く。
void putKanjiCell(std::vector<x68k::u8>& vram, x68k::u32 column, x68k::u32 row)
{
    for (x68k::u32 line = 0; line < x68k::TextScrape::kCellHeight; ++line)
    {
        const x68k::u32 y = row * x68k::TextScrape::kCellHeight + line;
        const x68k::u32 offset = y * x68k::kTvramBytesPerLine + column;
        // 全ライン同じ値。makeCgrom はラインごとに値を変えるので当たらない。
        vram[offset] = 0xA5;
    }
}

}  // namespace

TEST_CASE("テキスト VRAM の ANK を文字として読み戻せる")
{
    const auto cgrom = makeCgrom();
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    const char* kText = "A>dir";
    for (x68k::u32 i = 0; kText[i] != '\0'; ++i)
    {
        putChar(vram, cgrom, i, 5, kText[i]);
    }

    char line[x68k::TextScrape::kColumns + 1];
    x68k::TextScrape::readRow(vram.data(), cgrom.data(), 5, line);

    CHECK(std::strcmp(line, "A>dir") == 0);
}

TEST_CASE("何も書かれていない行は空文字列になる")
{
    const auto cgrom = makeCgrom();
    const std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    char line[x68k::TextScrape::kColumns + 1];
    x68k::TextScrape::readRow(vram.data(), cgrom.data(), 0, line);

    CHECK(std::strcmp(line, "") == 0);
}

TEST_CASE("行末の空白は詰める")
{
    const auto cgrom = makeCgrom();
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    // 行の先頭と、離れた位置に 1 文字ずつ置く。
    putChar(vram, cgrom, 0, 3, 'X');
    putChar(vram, cgrom, 4, 3, 'Y');

    char line[x68k::TextScrape::kColumns + 1];
    x68k::TextScrape::readRow(vram.data(), cgrom.data(), 3, line);

    // 間の空白は残り、末尾は詰まる。
    CHECK(std::strcmp(line, "X   Y") == 0);
}

TEST_CASE("ANK に無い字形は漢字として区別する")
{
    const auto cgrom = makeCgrom();
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    putChar(vram, cgrom, 0, 7, 'a');
    putKanjiCell(vram, 1, 7);
    putKanjiCell(vram, 2, 7);

    char line[x68k::TextScrape::kColumns + 1];
    x68k::TextScrape::readRow(vram.data(), cgrom.data(), 7, line);

    // 漢字は 2 セルにまたがるので '#' が 2 つ並ぶ。
    CHECK(std::strcmp(line, "a##") == 0);
}

TEST_CASE("空のセルを見分けられる")
{
    const auto cgrom = makeCgrom();
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    putChar(vram, cgrom, 2, 1, 'Z');

    CHECK(x68k::TextScrape::isCellBlank(vram.data(), 0, 1));
    CHECK_FALSE(x68k::TextScrape::isCellBlank(vram.data(), 2, 1));
}

TEST_CASE("範囲外の行や nullptr で落ちない")
{
    const auto cgrom = makeCgrom();
    const std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    char line[x68k::TextScrape::kColumns + 1];

    // 行番号が範囲外
    line[0] = 'x';
    x68k::TextScrape::readRow(vram.data(), cgrom.data(), x68k::TextScrape::kRows, line);
    CHECK(line[0] == '\0');

    // CGROM が無い (実機で確保に失敗した場合)
    line[0] = 'x';
    x68k::TextScrape::readRow(vram.data(), nullptr, 0, line);
    CHECK(line[0] == '\0');

    // VRAM が無い
    line[0] = 'x';
    x68k::TextScrape::readRow(nullptr, cgrom.data(), 0, line);
    CHECK(line[0] == '\0');
}

TEST_CASE("本文の末尾位置を求められる")
{
    const auto cgrom = makeCgrom();
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    putChar(vram, cgrom, 0, 5, 'A');
    putChar(vram, cgrom, 1, 5, '>');
    putChar(vram, cgrom, 2, 5, 'x');

    CHECK(x68k::TextScrape::lastUsedRow(vram.data()) == 5);
    CHECK(x68k::TextScrape::lastUsedColumn(vram.data(), 5) == 2);
}

TEST_CASE("ファンクションキー行は本文の末尾に数えない")
{
    const auto cgrom = makeCgrom();
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    // Human68k はファンクションキーの一覧を常に最下行へ出す。
    // これを本文と数えると、表示位置が常に画面最下部へ飛んでしまう。
    putChar(vram, cgrom, 0, 5, 'A');
    for (x68k::u32 c = 0; c < 40; ++c)
    {
        putChar(vram, cgrom, c, x68k::TextScrape::kFunctionKeyRow, 'F');
    }

    CHECK(x68k::TextScrape::lastUsedRow(vram.data()) == 5);
}

TEST_CASE("何も書かれていなければ本文の末尾は 0 行目")
{
    const std::vector<x68k::u8> vram(x68k::kTvramSize, 0x00);

    CHECK(x68k::TextScrape::lastUsedRow(vram.data()) == 0);
    CHECK(x68k::TextScrape::lastUsedColumn(vram.data(), 0) == 0);
}

TEST_CASE("ASCII からスキャンコードへの対応")
{
    // IPL-ROM 内の変換表から読み取った値。ここがずれると実機で
    // 打った文字と違う文字が入る。
    CHECK(x68k::asciiToScanCode('1') == 0x02);
    CHECK(x68k::asciiToScanCode('q') == 0x11);
    CHECK(x68k::asciiToScanCode('a') == 0x1E);
    CHECK(x68k::asciiToScanCode('z') == 0x2A);
    CHECK(x68k::asciiToScanCode(' ') == 0x35);
    CHECK(x68k::asciiToScanCode('\r') == 0x1D);
    CHECK(x68k::asciiToScanCode('\n') == 0x1D);

    // 大文字は小文字と同じスキャンコード (シフト面は別で扱う)。
    CHECK(x68k::asciiToScanCode('A') == x68k::asciiToScanCode('a'));

    // 打てない文字は 0。'\0' を strchr が終端に当てて誤って
    // スキャンコードを返さないことも確かめる。
    CHECK(x68k::asciiToScanCode('\0') == 0);
    CHECK(x68k::asciiToScanCode('\t') == 0);
}
