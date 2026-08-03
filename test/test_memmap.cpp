// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: X68000 のアドレス空間の定義が、実機のメモリマップと矛盾しないこと。
//
// アドレス定数の写し間違いは「起動しない」という形でしか現れず、しかも原因が
// CPU コアのバグと区別しにくい。領域が重ならないこと・サイズが仕様どおりであること
// を機械的に押さえておくと、M2 以降の切り分けが楽になる。

#include "doctest.h"
#include "memmap.h"

namespace
{

// 領域 [a, a+asize) と [b, b+bsize) が重ならないこと。
bool disjoint(std::uint32_t a, std::uint32_t asize, std::uint32_t b, std::uint32_t bsize)
{
    return (a + asize <= b) || (b + bsize <= a);
}

}  // namespace

TEST_CASE("IPLROM は $FE0000 から 128KB を占め、アドレス空間の末尾で終わる")
{
    CHECK(x68k::kIplromBase == 0x00FE0000u);
    CHECK(x68k::kIplromSize == 128u * 1024u);
    // 24bit アドレス空間の最後まで使い切る。ここがずれるとリセットベクタを読めない。
    CHECK(x68k::kIplromEnd == 0x01000000u);
}

TEST_CASE("リセット後の実行開始点が IPLROM 内にある")
{
    // 68000 はリセット時にベクタから SSP と PC を読む。X68000 では
    // SSP=$002000 / PC=$FF0010 になる。PC が IPLROM の範囲外だと即座に暴走する。
    CHECK(x68k::kResetPc >= x68k::kIplromBase);
    CHECK(x68k::kResetPc < x68k::kIplromEnd);
    CHECK(x68k::kResetSsp == x68k::kBootSectorLoadAddr);
}

TEST_CASE("テキスト VRAM は 4 プレーン × 128KB = 512KB")
{
    CHECK(x68k::kTvramPlaneCount == 4u);
    CHECK(x68k::kTvramPlaneSize == 128u * 1024u);
    CHECK(x68k::kTvramSize == 512u * 1024u);
    CHECK(x68k::kTvramBase == 0x00E00000u);
    // I/O 空間 ($E80000-) を侵さないこと。
    CHECK(x68k::kTvramEnd <= x68k::kCrtcBase);
}

TEST_CASE("テキスト画面の 1 ラインは 1 プレーンあたり 128 バイト")
{
    // 1bit/dot で 1024 dot ぶん。プレーン内オフセットから x,y を逆算する
    // ダーティ管理がこの値に依存する。
    CHECK(x68k::kTvramBytesPerLine * 8u == 1024u);
    // 1 プレーンで 1024 ライン ぶん持てる (1024x1024)。
    CHECK(x68k::kTvramPlaneSize / x68k::kTvramBytesPerLine == 1024u);
}

TEST_CASE("CGROM は $F00000 から 768KB")
{
    CHECK(x68k::kCgromBase == 0x00F00000u);
    CHECK(x68k::kCgromSize == 768u * 1024u);
    // IPLROM と重ならないこと。
    CHECK(x68k::kCgromEnd <= x68k::kIplromBase);
}

TEST_CASE("IPLROM 内の 6x12 ANK フォントが IPLROM の範囲に収まる")
{
    // CGROM が入手できない場合に英数字を表示する唯一の経路なので、
    // アドレスと文字数の写し間違いは致命的。
    CHECK(x68k::kIplromAnk6x12Addr >= x68k::kIplromBase);
    // 6x12 は 1 文字 12 バイト (1 ライン 1 バイト × 12 ライン)。
    constexpr std::uint32_t kBytesPerGlyph = 12u;
    CHECK(x68k::kIplromAnk6x12Addr + x68k::kIplromAnk6x12Count * kBytesPerGlyph <=
          x68k::kIplromEnd);
    // 256 ではなく 254 文字であることが実機の仕様。
    CHECK(x68k::kIplromAnk6x12Count == 254u);
}

TEST_CASE("SRAM は $ED0000 から 16KB で、CGROM と重ならない")
{
    CHECK(x68k::kSramBase == 0x00ED0000u);
    CHECK(x68k::kSramSize == 16u * 1024u);
    CHECK(x68k::kSramEnd <= x68k::kCgromBase);
}

TEST_CASE("主要な領域どうしが重ならない")
{
    CHECK(disjoint(x68k::kMainRamBase, x68k::kMainRamSize, x68k::kGvramBase,
                   x68k::kGvramEnd - x68k::kGvramBase));
    CHECK(disjoint(x68k::kGvramBase, x68k::kGvramEnd - x68k::kGvramBase, x68k::kTvramBase,
                   x68k::kTvramSize));
    CHECK(disjoint(x68k::kTvramBase, x68k::kTvramSize, x68k::kSramBase, x68k::kSramSize));
    CHECK(disjoint(x68k::kSramBase, x68k::kSramSize, x68k::kIplromBase, x68k::kIplromSize));
}

TEST_CASE("エリアセットレジスタは メモリコントローラの領域内にある")
{
    // IPLROM が起動直後に触る最初の I/O。ここを間違えるとブートの一歩目で止まる。
    CHECK(x68k::kAreaSetReg == 0x00E86001u);
    CHECK(x68k::kAreaSetReg >= x68k::kAreaSetBase);
    CHECK(x68k::kAreaSetReg < x68k::kMfpBase);
}

TEST_CASE("ブートの固定アドレスがメインメモリの中にある")
{
    // IPLROM はブートセクタを $002000 に置いて実行し、ブートセクタは
    // HUMAN.SYS を $0067C0 から読み込む。どちらも RAM 上でなければならない。
    CHECK(x68k::kBootSectorLoadAddr < x68k::kMainRamSize);
    CHECK(x68k::kHumanLoadReadAddr < x68k::kMainRamSize);
    CHECK(x68k::kHumanLoadAddr < x68k::kMainRamSize);
    // 実行ファイルヘッダ ($40 バイト) の分だけ読み込み開始が手前にある。
    CHECK(x68k::kHumanLoadAddr - x68k::kHumanLoadReadAddr == 0x40u);
}

TEST_CASE("アドレスマスクが 24bit である")
{
    // 68000 は上位 8bit を出力しない。バスアクセスのたびに折り返す必要がある。
    CHECK(x68k::kAddressMask == 0x00FFFFFFu);
    CHECK(((0xFF000000u | x68k::kIplromBase) & x68k::kAddressMask) == x68k::kIplromBase);
}
