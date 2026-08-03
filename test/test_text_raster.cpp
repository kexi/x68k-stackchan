// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: テキスト VRAM の 4 プレーンが正しく合成され、
// パレットを引いた RGB565 が出てくること。
//
// ここが間違っていると「画面に何か出ているが読めない」という状態になり、
// CPU のバグと区別がつかない。VRAM の中身を自分で作って検証しておく。

#include <vector>

#include "dev/video.h"
#include "doctest.h"
#include "video/text_raster.h"

namespace
{

// 指定座標のピクセルを、指定したプレーン構成で立てる。
void setPixel(std::vector<x68k::u8>& vram, x68k::u32 x, x68k::u32 y, x68k::u8 colorIndex)
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

}  // namespace

TEST_CASE("4 プレーンが合成されて 4bit のパレット番号になる")
{
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0);

    // 色番号 0-15 をそれぞれ別の座標に置く。
    for (x68k::u32 color = 0; color < 16; ++color)
    {
        setPixel(vram, color, 0, static_cast<x68k::u8>(color));
    }

    for (x68k::u32 color = 0; color < 16; ++color)
    {
        CHECK(x68k::TextRaster::pixelIndex(vram.data(), color, 0) == color);
    }
}

TEST_CASE("1 バイトに 8 ドットが入り、最上位ビットが左端")
{
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0);
    // プレーン 0 の先頭バイトに $80 を書くと、左端 1 ドットだけが立つ。
    vram[0] = 0x80;

    CHECK(x68k::TextRaster::pixelIndex(vram.data(), 0, 0) == 1);
    CHECK(x68k::TextRaster::pixelIndex(vram.data(), 1, 0) == 0);

    vram[0] = 0x01;
    CHECK(x68k::TextRaster::pixelIndex(vram.data(), 0, 0) == 0);
    CHECK(x68k::TextRaster::pixelIndex(vram.data(), 7, 0) == 1);
}

TEST_CASE("1 ライン 128 バイトで次の行に進む")
{
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0);
    // ライン 1 の左端。
    vram[x68k::kTvramBytesPerLine] = 0x80;

    CHECK(x68k::TextRaster::pixelIndex(vram.data(), 0, 0) == 0);
    CHECK(x68k::TextRaster::pixelIndex(vram.data(), 0, 1) == 1);
}

TEST_CASE("X68000 の色形式が RGB565 に変換される")
{
    // X68000 は GGGGG RRRRR BBBBB I。
    // 純赤: R=31, G=0, B=0, I=0 → $07C0
    const x68k::u16 red = static_cast<x68k::u16>(31u << 6);
    const x68k::u16 red565 = x68k::VideoController::toRgb565(red);
    CHECK(((red565 >> 11) & 0x1F) == 31);  // R
    CHECK(((red565 >> 5) & 0x3F) == 0);    // G
    CHECK((red565 & 0x1F) == 0);           // B

    // 純緑: G=31 → 上位 5bit
    const x68k::u16 green = static_cast<x68k::u16>(31u << 11);
    const x68k::u16 green565 = x68k::VideoController::toRgb565(green);
    CHECK(((green565 >> 11) & 0x1F) == 0);
    // 緑は RGB565 で 6bit。輝度ビットが最下位に来るので 62。
    CHECK(((green565 >> 5) & 0x3F) == 62);
    CHECK((green565 & 0x1F) == 0);

    // 純青: B=31 → bit5-1
    const x68k::u16 blue = static_cast<x68k::u16>(31u << 1);
    const x68k::u16 blue565 = x68k::VideoController::toRgb565(blue);
    CHECK(((blue565 >> 11) & 0x1F) == 0);
    CHECK(((blue565 >> 5) & 0x3F) == 0);
    CHECK((blue565 & 0x1F) == 31);
}

TEST_CASE("矩形を切り出して描画できる")
{
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0);
    x68k::VideoController video;
    video.reset();

    // パレット 1 を白にする (reset で設定済みだが明示する)。
    video.write(0x200 + 1 * 2, 0xFFFF);

    // (10, 5) に色 1 のドットを置く。
    setPixel(vram, 10, 5, 1);

    constexpr x68k::u32 kW = 16;
    constexpr x68k::u32 kH = 8;
    std::vector<x68k::u16> out(kW * kH, 0);
    x68k::TextRaster::render(vram.data(), video, 0, 0, kW, kH, out.data(), kW);

    const x68k::u16 white = x68k::VideoController::toRgb565(0xFFFF);
    CHECK(out[5 * kW + 10] == white);
    // 周囲は色 0 のまま。
    CHECK(out[5 * kW + 9] != white);
    CHECK(out[4 * kW + 10] != white);
}

TEST_CASE("オフセットを指定して切り出せる")
{
    std::vector<x68k::u8> vram(x68k::kTvramSize, 0);
    x68k::VideoController video;
    video.reset();
    video.write(0x200 + 1 * 2, 0xFFFF);

    // 画面の (100, 50) にドットを置き、そこを原点に切り出す。
    setPixel(vram, 100, 50, 1);

    constexpr x68k::u32 kW = 8;
    constexpr x68k::u32 kH = 4;
    std::vector<x68k::u16> out(kW * kH, 0);
    x68k::TextRaster::render(vram.data(), video, 100, 50, kW, kH, out.data(), kW);

    const x68k::u16 white = x68k::VideoController::toRgb565(0xFFFF);
    // 切り出した原点に来る。
    CHECK(out[0] == white);
}

TEST_CASE("null を渡しても落ちない")
{
    x68k::VideoController video;
    video.reset();
    std::vector<x68k::u16> out(16, 0);
    // 起動直後など VRAM が未設定の状態で描画が呼ばれることがある。
    x68k::TextRaster::render(nullptr, video, 0, 0, 4, 4, out.data(), 4);
    x68k::TextRaster::render(nullptr, video, 0, 0, 4, 4, nullptr, 4);
}
