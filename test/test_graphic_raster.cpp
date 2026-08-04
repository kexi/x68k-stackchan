// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: G-VRAM のパックドピクセルが色数モードごとに正しく解け、
// グラフィックパレットを引いた RGB565 が出てくること。および
// テキスト画面との重ね合わせが $E82500 / $E82600 に従うこと。
//
// ここが間違っていると「絵は出ているが色やページがずれる」という状態になり、
// 描画したプログラム側のバグと区別がつかない。VRAM の中身を自分で作って
// 検証しておく。

#include <vector>

#include "dev/video.h"
#include "doctest.h"
#include "video/graphic_raster.h"

namespace
{

using x68k::GraphicRaster;
using x68k::VideoController;

// ビデオコントローラのレジスタのオフセット ($E82000 からの相対)。
constexpr x68k::u32 kGraphicPaletteOffset = 0x000;
constexpr x68k::u32 kTextPaletteOffset = 0x200;
constexpr x68k::u32 kScreenModeOffset = 0x400;
constexpr x68k::u32 kPriorityOffset = 0x500;
constexpr x68k::u32 kDisplayCtrlOffset = 0x600;

// G-VRAM の 1 ワードを書く。68000 はビッグエンディアン。
void writeWord(std::vector<x68k::u8>& vram, x68k::u32 x, x68k::u32 y, x68k::u16 value)
{
    const std::size_t byteOffset = (static_cast<std::size_t>(y) * x68k::kGvramPageWidth + x) * 2u;
    vram[byteOffset] = static_cast<x68k::u8>(value >> 8);
    vram[byteOffset + 1] = static_cast<x68k::u8>(value & 0xFF);
}

// テキスト VRAM の 1 ドットを指定のパレット番号で立てる。
void setTextPixel(std::vector<x68k::u8>& vram, x68k::u32 x, x68k::u32 y, x68k::u8 colorIndex)
{
    const x68k::u32 byteOffset = y * x68k::kTvramBytesPerLine + (x >> 3);
    const x68k::u8 mask = static_cast<x68k::u8>(1u << (7u - (x & 7u)));

    for (x68k::u32 plane = 0; plane < x68k::kTvramPlaneCount; ++plane)
    {
        const std::size_t index = plane * x68k::kTvramPlaneSize + byteOffset;
        if ((colorIndex & (1u << plane)) != 0)
        {
            vram[index] = static_cast<x68k::u8>(vram[index] | mask);
        }
        else
        {
            vram[index] = static_cast<x68k::u8>(vram[index] & ~mask);
        }
    }
}

// 512KB の G-VRAM を 0 で用意する。
std::vector<x68k::u8> makeGvram()
{
    return std::vector<x68k::u8>(x68k::kTvramSize, 0);
}

}  // namespace

// --- 色数モードごとのドット解釈 ---------------------------------------------

TEST_CASE("16 色モードでは 1 ワードに 4 ページぶんの 4bit が詰まる")
{
    auto vram = makeGvram();
    // ページ 0 = $1、ページ 1 = $2、ページ 2 = $3、ページ 3 = $4。
    writeWord(vram, 0, 0, 0x4321);

    const auto mode = VideoController::GraphicColorMode::k16Color;
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 0, 0, 0) == 0x1);
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 1, 0, 0) == 0x2);
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 2, 0, 0) == 0x3);
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 3, 0, 0) == 0x4);
}

TEST_CASE("256 色モードでは 1 ワードに 2 ページぶんの 8bit が詰まる")
{
    auto vram = makeGvram();
    // ページ 0 = $CD (下位バイト)、ページ 1 = $AB (上位バイト)。
    writeWord(vram, 0, 0, 0xABCD);

    const auto mode = VideoController::GraphicColorMode::k256Color;
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 0, 0, 0) == 0xCD);
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 1, 0, 0) == 0xAB);
}

TEST_CASE("1 ラインは 512 ワードで次の行に進む")
{
    auto vram = makeGvram();
    writeWord(vram, 0, 1, 0x000F);

    const auto mode = VideoController::GraphicColorMode::k16Color;
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 0, 0, 0) == 0);
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 0, 0, 1) == 0xF);
    // 隣のドットは別ワードなので影響を受けない。
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 0, 1, 1) == 0);
}

TEST_CASE("1024x1024 モードは 4 ページを 2x2 に並べる")
{
    auto vram = makeGvram();
    // 同じワードに 4 ページぶんの別々の値を置き、座標で選び分かれることを見る。
    writeWord(vram, 3, 5, 0x4321);

    // 左上 = ページ 0、右上 = ページ 1、左下 = ページ 2、右下 = ページ 3。
    CHECK(GraphicRaster::pixelIndexLarge(vram.data(), 3, 5) == 0x1);
    CHECK(GraphicRaster::pixelIndexLarge(vram.data(), 3 + 512, 5) == 0x2);
    CHECK(GraphicRaster::pixelIndexLarge(vram.data(), 3, 5 + 512) == 0x3);
    CHECK(GraphicRaster::pixelIndexLarge(vram.data(), 3 + 512, 5 + 512) == 0x4);
}

TEST_CASE("実画面の外を読んでも 0 が返る")
{
    auto vram = makeGvram();
    const auto mode = VideoController::GraphicColorMode::k16Color;

    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 0, 512, 0) == 0);
    CHECK(GraphicRaster::pixelIndex(vram.data(), mode, 0, 0, 512) == 0);
    CHECK(GraphicRaster::pixelIndexLarge(vram.data(), 1024, 0) == 0);
    CHECK(GraphicRaster::pixelIndexLarge(vram.data(), 0, 1024) == 0);
}

// --- パレット ---------------------------------------------------------------

TEST_CASE("グラフィックパレットを引いた色が返る")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();

    // 16 色モード、512x512。
    video.write(kScreenModeOffset, 0x0000);
    // パレット 5 を純赤 (R=31) にする。
    const x68k::u16 red = static_cast<x68k::u16>(31u << 6);
    video.write(kGraphicPaletteOffset + 5 * 2, red);

    // ページ 0 に色番号 5 を置く。
    writeWord(vram, 10, 20, 0x0005);

    CHECK(GraphicRaster::pixelColor(vram.data(), video, 0, 10, 20) == red);
    // 何も置いていない位置はパレット 0 (reset 直後は 0)。
    CHECK(GraphicRaster::pixelColor(vram.data(), video, 0, 11, 20) == 0);
}

TEST_CASE("256 色モードでは 8bit ぜんぶがパレット番号になる")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0001);  // 256 色

    // 上位のパレット番号 ($FF) が 16 色モードのように切り詰められないこと。
    const x68k::u16 blue = static_cast<x68k::u16>(31u << 1);
    video.write(kGraphicPaletteOffset + 255 * 2, blue);
    writeWord(vram, 0, 0, 0x00FF);

    CHECK(GraphicRaster::pixelIndex(vram.data(), VideoController::GraphicColorMode::k256Color, 0, 0,
                                    0) == 0xFF);
    CHECK(GraphicRaster::pixelColor(vram.data(), video, 0, 0, 0) == blue);
}

TEST_CASE("65536 色モードはパレットを介さずワードがそのまま色になる")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0003);  // 65536 色

    // パレット 0 に何を入れても結果に影響しないことも見る。
    video.write(kGraphicPaletteOffset + 0 * 2, 0x1234);

    const x68k::u16 color = 0xBEEF;
    writeWord(vram, 4, 4, color);

    CHECK(GraphicRaster::pixelColor(vram.data(), video, 0, 4, 4) == color);
}

// --- 矩形の変換 -------------------------------------------------------------

TEST_CASE("矩形を切り出して RGB565 に変換できる")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);   // 16 色
    video.write(kDisplayCtrlOffset, 0x0001);  // ページ 0 だけ表示

    const x68k::u16 white = 0xFFFF;
    video.write(kGraphicPaletteOffset + 1 * 2, white);
    writeWord(vram, 3, 2, 0x0001);  // ページ 0 に色 1

    constexpr x68k::u32 kW = 8;
    constexpr x68k::u32 kH = 4;
    std::vector<x68k::u16> out(kW * kH, 0);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, kH, out.data(), kW);

    CHECK(out[2 * kW + 3] == VideoController::toRgb565(white));
    // 周囲は透明なので触られていない。
    CHECK(out[2 * kW + 4] == 0);
    CHECK(out[1 * kW + 3] == 0);
}

TEST_CASE("オフセットを指定して切り出せる")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);
    video.write(kDisplayCtrlOffset, 0x0001);
    video.write(kGraphicPaletteOffset + 1 * 2, 0xFFFF);

    writeWord(vram, 100, 50, 0x0001);

    constexpr x68k::u32 kW = 4;
    constexpr x68k::u32 kH = 4;
    std::vector<x68k::u16> out(kW * kH, 0);
    GraphicRaster::render(vram.data(), video, 100, 50, kW, kH, out.data(), kW);

    // 切り出した原点に来る。
    CHECK(out[0] == VideoController::toRgb565(0xFFFF));
}

TEST_CASE("透明ドット (パレット番号 0) は出力を上書きしない")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);
    video.write(kDisplayCtrlOffset, 0x0001);
    // パレット 0 に色を入れても、透明の扱いは変わらない。
    video.write(kGraphicPaletteOffset + 0 * 2, 0xFFFF);

    constexpr x68k::u32 kW = 4;
    constexpr x68k::u32 kH = 2;
    // 事前に埋めておいた背景が残ることを確かめる。
    std::vector<x68k::u16> out(kW * kH, 0x1234);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, kH, out.data(), kW);

    for (const auto pixel : out)
    {
        CHECK(pixel == 0x1234);
    }
}

TEST_CASE("16 色モードでは表示が許可された最も手前のページが描かれる")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);

    const x68k::u16 red = static_cast<x68k::u16>(31u << 6);
    const x68k::u16 blue = static_cast<x68k::u16>(31u << 1);
    video.write(kGraphicPaletteOffset + 1 * 2, red);
    video.write(kGraphicPaletteOffset + 2 * 2, blue);

    // ページ 0 に色 1、ページ 1 に色 2 を同じ座標へ置く。
    writeWord(vram, 0, 0, 0x0021);

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);

    // ページ 0 を許可 → 色 1 が出る。
    video.write(kDisplayCtrlOffset, 0x0001);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(red));

    // ページ 1 だけを許可 → 色 2 が出る。
    out[0] = 0;
    video.write(kDisplayCtrlOffset, 0x0002);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(blue));
}

TEST_CASE("複数ページが許可されているときは GP3-GP0 が手前を決める")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);

    const x68k::u16 red = static_cast<x68k::u16>(31u << 6);
    const x68k::u16 blue = static_cast<x68k::u16>(31u << 1);
    video.write(kGraphicPaletteOffset + 1 * 2, red);
    video.write(kGraphicPaletteOffset + 2 * 2, blue);

    // ページ 0 に色 1 (赤)、ページ 1 に色 2 (青)。どちらも表示を許可する。
    writeWord(vram, 0, 0, 0x0021);
    video.write(kDisplayCtrlOffset, 0x0003);

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);

    // 既定の並び (GP0=0, GP1=1) ではページ 0 が手前 → 赤。
    video.write(kPriorityOffset, 0x06E4);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(red));

    // GP を入れ替えてページ 1 を手前にすると、VRAM を触らずに青へ変わる。
    // ページ番号順の決め打ちだとここが赤のままになる。
    out[0] = 0;
    video.write(kPriorityOffset, 0x0601);  // GP0=1, GP1=0
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(blue));
}

// --- ページどうしの重ね合わせ -----------------------------------------------
//
// 保証すること: 手前のページの透明ドット (パレット番号 0) では、その後ろの
// ページの色が出る。実機のグラフィック 4 ページは合成されるプレーンではなく
// 独立した画面で、透明ドットのぶんだけ後ろが透ける。

TEST_CASE("16 色モードで手前のページが透明なら後ろのページが透ける")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);

    const x68k::u16 red = static_cast<x68k::u16>(31u << 6);
    const x68k::u16 blue = static_cast<x68k::u16>(31u << 1);
    video.write(kGraphicPaletteOffset + 1 * 2, red);
    video.write(kGraphicPaletteOffset + 2 * 2, blue);

    // x=0: ページ 0 = 色 1 (赤)、ページ 1 = 色 2 (青)  → 手前 (ページ 0) が勝つ
    // x=1: ページ 0 = 透明、    ページ 1 = 色 2 (青)  → 後ろのページ 1 が出る
    writeWord(vram, 0, 0, 0x0021);
    writeWord(vram, 1, 0, 0x0020);

    video.write(kPriorityOffset, 0x06E4);     // GP0=0, GP1=1 でページ 0 が手前
    video.write(kDisplayCtrlOffset, 0x0003);  // ページ 0 と 1 を表示

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);

    CHECK(out[0] == VideoController::toRgb565(red));
    CHECK(out[1] == VideoController::toRgb565(blue));
}

TEST_CASE("16 色モードの 4 ページは GP3-GP0 の順に透けていく")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);

    // 色 1-4 をそれぞれ別の値にして、どのページの色が出たか区別できるようにする。
    for (x68k::u16 i = 1; i <= 4; ++i)
    {
        video.write(kGraphicPaletteOffset + i * 2, static_cast<x68k::u16>(i << 6));
    }
    const auto colorOf = [](x68k::u16 index)
    { return VideoController::toRgb565(static_cast<x68k::u16>(index << 6)); };

    // 4 ページすべてを表示し、GP を逆順にしてページ 3 を最も手前にする。
    video.write(kDisplayCtrlOffset, 0x000F);
    video.write(kPriorityOffset, 0x001B);  // GP3=0, GP2=1, GP1=2, GP0=3

    // x=0: 全ページ不透明 (page0=1, page1=2, page2=3, page3=4)
    //      → 手前のページ 3 の色 4 が出る。ページ番号順なら色 1 になってしまう。
    // x=1: ページ 3 だけ透明 → 次に手前のページ 2 の色 3。
    // x=2: ページ 3 と 2 が透明 → ページ 1 の色 2。
    // x=3: ページ 1 だけ不透明 → 色 2。
    writeWord(vram, 0, 0, 0x4321);
    writeWord(vram, 1, 0, 0x0321);
    writeWord(vram, 2, 0, 0x0021);
    writeWord(vram, 3, 0, 0x0020);

    constexpr x68k::u32 kW = 4;
    std::vector<x68k::u16> out(kW, 0);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);

    CHECK(out[0] == colorOf(4));
    CHECK(out[1] == colorOf(3));
    CHECK(out[2] == colorOf(2));
    CHECK(out[3] == colorOf(2));
}

TEST_CASE("256 色モードでも手前のページが透明なら後ろのページが透ける")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0001);  // 256 色 (2 ページ)

    // 8bit ぜんぶが番号になることが分かる値にする。
    const x68k::u16 red = static_cast<x68k::u16>(31u << 6);
    const x68k::u16 blue = static_cast<x68k::u16>(31u << 1);
    video.write(kGraphicPaletteOffset + 0x12 * 2, red);
    video.write(kGraphicPaletteOffset + 0x34 * 2, blue);

    // x=0: ページ 0 = $12、ページ 1 = $34 → 手前のページ 0 が勝つ
    // x=1: ページ 0 = 透明、ページ 1 = $34 → ページ 1 が透ける
    writeWord(vram, 0, 0, 0x3412);
    writeWord(vram, 1, 0, 0x3400);

    // 256 色のページ許可は 2bit ずつ。$0F で両ページを表示。
    video.write(kDisplayCtrlOffset, 0x000F);
    video.write(kPriorityOffset, 0x06E4);  // GP0=0, GP1=1

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);

    CHECK(out[0] == VideoController::toRgb565(red));
    CHECK(out[1] == VideoController::toRgb565(blue));
}

TEST_CASE("ページの重なり順はページ番号ではなく GP3-GP0 で決まる")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);

    const x68k::u16 blue = static_cast<x68k::u16>(31u << 1);
    const x68k::u16 green = static_cast<x68k::u16>(31u << 11);
    video.write(kGraphicPaletteOffset + 2 * 2, blue);
    video.write(kGraphicPaletteOffset + 3 * 2, green);

    // ページ 0 は透明、ページ 1 = 色 2 (青)、ページ 2 = 色 3 (緑)。
    writeWord(vram, 0, 0, 0x0320);
    video.write(kDisplayCtrlOffset, 0x0007);  // ページ 0-2 を表示

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);

    // 既定の並びではページ 1 がページ 2 より手前 → 透明なページ 0 を抜けて青。
    video.write(kPriorityOffset, 0x06E4);  // GP0=0, GP1=1, GP2=2
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(blue));

    // GP2 をページ 1 より手前にすると、VRAM を触らずに緑へ変わる。
    // ページ番号順に重ねる実装だとここが青のままになる。
    out[0] = 0;
    // GP3=3, GP2=1, GP1=2, GP0=0 → $E4 の GP1/GP2 を入れ替えた $D8。
    video.write(kPriorityOffset, 0x06D8);
    REQUIRE(video.graphicPagePriority(0) == 0);
    REQUIRE(video.graphicPagePriority(1) == 2);
    REQUIRE(video.graphicPagePriority(2) == 1);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(green));
}

TEST_CASE("全ページが透明なドットには何も書かれない")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);
    video.write(kDisplayCtrlOffset, 0x000F);  // 4 ページすべて表示
    video.write(kPriorityOffset, 0x06E4);
    // パレット 0 に色を入れても透明の扱いは変わらない。
    video.write(kGraphicPaletteOffset + 0 * 2, 0xFFFF);

    // ワードが 0 なので 4 ページとも色番号 0 = 透明。
    constexpr x68k::u32 kW = 4;
    std::vector<x68k::u16> out(kW, 0x1234);
    GraphicRaster::render(vram.data(), video, 0, 0, kW, 1, out.data(), kW);

    for (const auto pixel : out)
    {
        CHECK(pixel == 0x1234);
    }
}

TEST_CASE("どのページも許可されていなければ何も描かれない")
{
    auto vram = makeGvram();
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);
    video.write(kDisplayCtrlOffset, 0x0000);  // ページ 0-3 すべて不許可
    video.write(kGraphicPaletteOffset + 1 * 2, 0xFFFF);
    writeWord(vram, 0, 0, 0x1111);

    std::vector<x68k::u16> out(4, 0x0777);
    GraphicRaster::render(vram.data(), video, 0, 0, 4, 1, out.data(), 4);

    for (const auto pixel : out)
    {
        CHECK(pixel == 0x0777);
    }
}

// --- レジスタの解釈 ---------------------------------------------------------

TEST_CASE("画面モードレジスタから色数と実画面サイズが読める")
{
    VideoController video;
    video.reset();

    video.write(kScreenModeOffset, 0x0000);
    CHECK(video.graphicColorMode() == VideoController::GraphicColorMode::k16Color);
    CHECK(video.isGraphic1024() == false);

    video.write(kScreenModeOffset, 0x0004);
    CHECK(video.graphicColorMode() == VideoController::GraphicColorMode::k16Color);
    CHECK(video.isGraphic1024() == true);

    video.write(kScreenModeOffset, 0x0001);
    CHECK(video.graphicColorMode() == VideoController::GraphicColorMode::k256Color);

    video.write(kScreenModeOffset, 0x0003);
    CHECK(video.graphicColorMode() == VideoController::GraphicColorMode::k65536Color);
}

TEST_CASE("プライオリティレジスタから各面の表示順位が読める")
{
    VideoController video;
    video.reset();

    // IPL-ROM が起動時に書く既定値 ($FF6436 の move.w #$06E4,$E82500)。
    // スプライト = 0 (bit13-12)、テキスト = 1 (bit11-10)、
    // グラフィック = 2 (bit9-8)。
    video.write(kPriorityOffset, 0x06E4);
    CHECK(video.spritePriority() == 0);
    CHECK(video.textPriority() == 1);
    CHECK(video.graphicPriority() == 2);

    // 面の順位は上位バイトだけで決まり、下位バイト (GP3-GP0) には影響されない。
    video.write(kPriorityOffset, 0x0900);
    CHECK(video.spritePriority() == 0);
    CHECK(video.textPriority() == 2);
    CHECK(video.graphicPriority() == 1);

    video.write(kPriorityOffset, 0x1000);
    CHECK(video.spritePriority() == 1);
    CHECK(video.textPriority() == 0);
    CHECK(video.graphicPriority() == 0);
}

TEST_CASE("プライオリティレジスタからグラフィック 4 ページの順位が読める")
{
    VideoController video;
    video.reset();

    // 既定値 $06E4 の下位バイト $E4 = 11_10_01_00。
    // GP3 = 3、GP2 = 2、GP1 = 1、GP0 = 0 でページ番号順。
    video.write(kPriorityOffset, 0x06E4);
    CHECK(video.graphicPagePriority(0) == 0);
    CHECK(video.graphicPagePriority(1) == 1);
    CHECK(video.graphicPagePriority(2) == 2);
    CHECK(video.graphicPagePriority(3) == 3);

    // 並びを逆にすると、ページ 3 が最も手前になる。
    video.write(kPriorityOffset, 0x001B);  // GP3=0, GP2=1, GP1=2, GP0=3
    CHECK(video.graphicPagePriority(0) == 3);
    CHECK(video.graphicPagePriority(1) == 2);
    CHECK(video.graphicPagePriority(2) == 1);
    CHECK(video.graphicPagePriority(3) == 0);

    // 範囲外のページは 0 を返す (落ちない)。
    CHECK(video.graphicPagePriority(4) == 0);
}

TEST_CASE("表示制御レジスタから各面の表示許可が読める")
{
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);  // 16 色 512x512

    video.write(kDisplayCtrlOffset, 0x0000);
    CHECK(video.graphicEnabled() == false);
    CHECK(video.textEnabled() == false);
    CHECK(video.spriteEnabled() == false);

    video.write(kDisplayCtrlOffset, 0x0070);
    CHECK(video.textEnabled() == true);
    CHECK(video.spriteEnabled() == true);

    // ページ単位の許可は bit3-0。
    video.write(kDisplayCtrlOffset, 0x000A);
    CHECK(video.graphicPageEnabled(0) == false);
    CHECK(video.graphicPageEnabled(1) == true);
    CHECK(video.graphicPageEnabled(2) == false);
    CHECK(video.graphicPageEnabled(3) == true);
}

TEST_CASE("512x512 ではページビットだけでグラフィック面が有効になる")
{
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);  // 16 色 512x512

    // GS4 (bit4) が無くても、GS3-GS0 が 1 つでも立っていれば表示される。
    // IOCS を通さず $E82600 を直接書くプログラムがこの形を作る。
    video.write(kDisplayCtrlOffset, 0x0001);
    CHECK(video.graphicEnabled() == true);

    video.write(kDisplayCtrlOffset, 0x0008);
    CHECK(video.graphicEnabled() == true);

    // ページビットが全部落ちていれば、GS4 があっても表示されない。
    video.write(kDisplayCtrlOffset, 0x0010);
    CHECK(video.graphicEnabled() == false);

    // IOCS が組み立てる形 (GS4 + ページ 0)。
    video.write(kDisplayCtrlOffset, 0x0011);
    CHECK(video.graphicEnabled() == true);
}

TEST_CASE("1024x1024 では GS4 がグラフィック面の表示許可になる")
{
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0004);  // 16 色 1024x1024
    REQUIRE(video.isGraphic1024());

    // 4 ページを 1 枚として使うので、ページビットではなく GS4 が効く。
    video.write(kDisplayCtrlOffset, 0x0010);
    CHECK(video.graphicEnabled() == true);

    video.write(kDisplayCtrlOffset, 0x000F);
    CHECK(video.graphicEnabled() == false);
}

TEST_CASE("256 色モードのページ許可は 2bit ずつで読む")
{
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0001);  // 256 色

    // IOCS はページ 0 を $03、ページ 1 を $0C に展開する ($FFB2F2 以降)。
    video.write(kDisplayCtrlOffset, 0x0003);
    CHECK(video.graphicPageEnabled(0) == true);
    CHECK(video.graphicPageEnabled(1) == false);

    video.write(kDisplayCtrlOffset, 0x000C);
    CHECK(video.graphicPageEnabled(0) == false);
    CHECK(video.graphicPageEnabled(1) == true);

    // 256 色モードにページ 2 以降は存在しない。
    video.write(kDisplayCtrlOffset, 0x000F);
    CHECK(video.graphicPageEnabled(2) == false);
    CHECK(video.graphicPageEnabled(3) == false);
}

// --- 重ね合わせ -------------------------------------------------------------

namespace
{

// 重ね合わせのテストで使う定型のセットアップ。
struct CompositeFixture
{
    std::vector<x68k::u8> gvram = makeGvram();
    std::vector<x68k::u8> tvram = std::vector<x68k::u8>(x68k::kTvramSize, 0);
    VideoController video;

    // グラフィックは色 1 = 赤、テキストは色 1 = 青。
    static constexpr x68k::u16 kRed = static_cast<x68k::u16>(31u << 6);
    static constexpr x68k::u16 kBlue = static_cast<x68k::u16>(31u << 1);

    CompositeFixture()
    {
        video.reset();
        video.write(kScreenModeOffset, 0x0000);   // 16 色
        video.write(kDisplayCtrlOffset, 0x0031);  // グラフィック + テキスト + ページ 0
        video.write(kGraphicPaletteOffset + 1 * 2, kRed);
        video.write(kTextPaletteOffset + 1 * 2, kBlue);
    }
};

}  // namespace

TEST_CASE("プライオリティに従ってテキストとグラフィックが重なる")
{
    CompositeFixture f;

    // 同じ座標に両方のドットを置く。
    writeWord(f.gvram, 0, 0, 0x0001);
    setTextPixel(f.tvram, 0, 0, 1);

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);

    // テキストを手前 (値が小さいほど手前)。
    f.video.write(kPriorityOffset, 0x02E4);  // graphic=2, text=0
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kBlue));

    // グラフィックを手前。
    out[0] = 0;
    f.video.write(kPriorityOffset, 0x08E4);  // graphic=0, text=2
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kRed));
}

TEST_CASE("手前の面が透明なら背後の面が見える")
{
    CompositeFixture f;

    // グラフィックだけにドットを置き、テキストは透明のまま。
    writeWord(f.gvram, 0, 0, 0x0001);

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);

    // テキストを手前にしても、そこが透明ならグラフィックが透ける。
    f.video.write(kPriorityOffset, 0x02E4);  // graphic=2, text=0
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kRed));
}

TEST_CASE("表示が禁止された面は重ね合わせに出ない")
{
    CompositeFixture f;

    writeWord(f.gvram, 0, 0, 0x0001);
    setTextPixel(f.tvram, 0, 0, 1);
    // グラフィックを手前に置いたうえで、グラフィックを禁止する。
    f.video.write(kPriorityOffset, 0x08E4);  // graphic=0, text=2

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);

    // グラフィック禁止 → テキストだけが出る。
    // 512x512 では GS4 だけでなく GS3-GS0 も落とさないと消えない。
    f.video.write(kDisplayCtrlOffset, 0x0020);
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kBlue));

    // テキスト禁止 (bit5 を落とす) → グラフィックだけが出る。
    out[0] = 0;
    f.video.write(kDisplayCtrlOffset, 0x0011);
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kRed));

    // 両方禁止 → 黒。
    out[0] = 0x7777;
    f.video.write(kDisplayCtrlOffset, 0x0000);
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == 0);
}

TEST_CASE("グラフィックの全ページが透明ならテキストが見えて残りは黒になる")
{
    CompositeFixture f;

    // グラフィック 4 ページすべてを表示し、グラフィックを手前に置く。
    f.video.write(kDisplayCtrlOffset, 0x002F);  // テキスト + ページ 0-3
    f.video.write(kPriorityOffset, 0x08E4);     // graphic=0, text=2

    // グラフィックはどのページも書かない (全ドット透明)。テキストだけ置く。
    setTextPixel(f.tvram, 0, 0, 1);

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0x5A5A);
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);

    // 手前のグラフィックが全ページ透明なので、後ろのテキストが出る。
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kBlue));
    // どちらの面も出ない位置は黒。
    CHECK(out[1] == 0);
}

TEST_CASE("どちらの面も出ない位置は黒で埋まる")
{
    CompositeFixture f;
    f.video.write(kPriorityOffset, 0x02E4);

    constexpr x68k::u32 kW = 4;
    constexpr x68k::u32 kH = 2;
    // 前フレームの残骸を模した値。合成後に残っていてはいけない。
    std::vector<x68k::u16> out(kW * kH, 0x5A5A);
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, kH, out.data(), kW);

    for (const auto pixel : out)
    {
        CHECK(pixel == 0);
    }
}

TEST_CASE("outStride を指定すると行ごとに飛ばして書ける")
{
    CompositeFixture f;
    f.video.write(kPriorityOffset, 0x02E4);
    writeWord(f.gvram, 0, 1, 0x0001);

    constexpr x68k::u32 kW = 2;
    constexpr x68k::u32 kH = 2;
    constexpr x68k::u32 kStride = 5;
    std::vector<x68k::u16> out(kStride * kH, 0xFFFF);
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, kH, out.data(),
                             kStride);

    // 2 行目の先頭に赤が来る。
    CHECK(out[kStride] == VideoController::toRgb565(CompositeFixture::kRed));
    // 幅の外 (パディング) は触られない。
    CHECK(out[kW] == 0xFFFF);
    CHECK(out[kStride + kW] == 0xFFFF);
}

// --- VRAM が無い場合 --------------------------------------------------------

TEST_CASE("G-VRAM が null でも落ちない")
{
    VideoController video;
    video.reset();
    video.write(kScreenModeOffset, 0x0000);
    video.write(kDisplayCtrlOffset, 0x0031);

    std::vector<x68k::u16> out(16, 0);

    // MemoryMap::graphicVram は任意なので、確保されないまま描画が呼ばれうる。
    GraphicRaster::render(nullptr, video, 0, 0, 4, 4, out.data(), 4);
    GraphicRaster::render(nullptr, video, 0, 0, 4, 4, nullptr, 4);
    CHECK(GraphicRaster::pixelIndex(nullptr, VideoController::GraphicColorMode::k16Color, 0, 0,
                                    0) == 0);
    CHECK(GraphicRaster::pixelIndexLarge(nullptr, 0, 0) == 0);
    CHECK(GraphicRaster::pixelColor(nullptr, video, 0, 0, 0) == 0);
}

TEST_CASE("重ね合わせで片方の VRAM が null でも残る面は描かれる")
{
    CompositeFixture f;
    f.video.write(kPriorityOffset, 0x02E4);  // graphic=2, text=0
    setTextPixel(f.tvram, 0, 0, 1);

    constexpr x68k::u32 kW = 2;
    std::vector<x68k::u16> out(kW, 0);

    // G-VRAM が無くてもテキストは出る。
    GraphicRaster::composite(nullptr, f.tvram.data(), f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kBlue));

    // テキスト VRAM が無くてもグラフィックは出る。
    writeWord(f.gvram, 0, 0, 0x0001);
    out[0] = 0;
    GraphicRaster::composite(f.gvram.data(), nullptr, f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == VideoController::toRgb565(CompositeFixture::kRed));

    // 両方無ければ黒のまま。
    out[0] = 0x1111;
    GraphicRaster::composite(nullptr, nullptr, f.video, 0, 0, kW, 1, out.data(), kW);
    CHECK(out[0] == 0);

    // out が null でも落ちない。
    GraphicRaster::composite(f.gvram.data(), f.tvram.data(), f.video, 0, 0, kW, 1, nullptr, kW);
}
