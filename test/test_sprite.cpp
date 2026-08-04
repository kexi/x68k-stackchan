// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: スプライトコントローラ (CYNTHIA) のレジスタが IPL-ROM から
// 読み取った割り当てどおりに解釈され、PCG のパターンが 4bpp として解け、
// スプライトと BG がテキスト/グラフィックとの重ね合わせに正しく入ること。
//
// レジスタのビット割り当てを間違えると「絵は出るが位置や色がずれる」状態に
// なり、動かしたソフト側のバグと区別がつかなくなる。特に本プロジェクトでは
// 過去に 2 度、間違ったビット割り当てがテストを通ってしまっている。
// そのためここでは「ROM から確かめた事実」を明示的に検査対象にする。

#include <vector>

#include "dev/sprite.h"
#include "dev/video.h"
#include "doctest.h"
#include "machine.h"
#include "video/sprite_raster.h"

namespace
{

using x68k::Sprite;
using x68k::SpriteRaster;
using x68k::VideoController;

// ビデオコントローラのレジスタのオフセット ($E82000 からの相対)。
constexpr x68k::u32 kTextPaletteOffset = 0x200;
constexpr x68k::u32 kPriorityOffset = 0x500;
constexpr x68k::u32 kDisplayCtrlOffset = 0x600;

// スプライトレジスタのワードオフセット ($EB0000 からの相対)。
constexpr x68k::u32 kBgScrollOffset = 0x800;
constexpr x68k::u32 kBgControlOffset = 0x808;

// スプライト n のレジスタ (word 0-3) のバイトオフセット。
constexpr x68k::u32 spriteReg(x68k::u32 index, x68k::u32 word)
{
    return index * Sprite::kSpriteStride + word * 2u;
}

// スプライト 1 個を設定する。座標は「画面上の位置」で指定する
// (レジスタには 16 の下駄を足して書く)。
void placeSprite(Sprite& sprite, x68k::u32 index, int x, int y, x68k::u16 attr, x68k::u8 priority)
{
    const x68k::u16 rawX = static_cast<x68k::u16>(x + Sprite::kCoordOffset);
    const x68k::u16 rawY = static_cast<x68k::u16>(y + Sprite::kCoordOffset);
    sprite.write(spriteReg(index, 0), rawX);
    sprite.write(spriteReg(index, 1), rawY);
    sprite.write(spriteReg(index, 2), attr);
    sprite.write(spriteReg(index, 3), priority);
}

// 16x16 PCG パターンを 1 色で塗る。
//
// 16x16 は 8x8 を 4 つ並べたもの (左上→右上→左下→右下) なので、
// 8x8 パターン 4 個ぶんの 128 バイトを埋めれば全体が塗れる。
void fillPattern16(Sprite& sprite, x68k::u32 pattern, x68k::u8 colorIndex)
{
    const x68k::u8 both = static_cast<x68k::u8>((colorIndex << 4) | colorIndex);
    const x68k::u32 base = pattern * Sprite::kPcg16Bytes;
    for (x68k::u32 i = 0; i < Sprite::kPcg16Bytes; ++i)
    {
        sprite.vramWrite8(base + i, both);
    }
}

// 16x16 パターンの 1 ドットだけを指定の色にする。周りは 0 (透明) のまま。
void setPatternPixel16(Sprite& sprite, x68k::u32 pattern, x68k::u32 x, x68k::u32 y,
                       x68k::u8 colorIndex)
{
    // pcgPixel() と同じ組み立て: 16x16 の n 番は 8x8 の n*4 から 4 個。
    const x68k::u32 quadrant = ((y >= 8) ? 2u : 0u) | ((x >= 8) ? 1u : 0u);
    const x68k::u32 pattern8 = pattern * 4u + quadrant;
    const x68k::u32 offset = pattern8 * Sprite::kPcg8Bytes + (y & 7u) * 4u + ((x & 7u) >> 1);

    const x68k::u8 old = sprite.vramRead8(offset);
    const bool isHighNibble = (x & 1u) == 0u;
    const x68k::u8 next = isHighNibble ? static_cast<x68k::u8>((old & 0x0Fu) | (colorIndex << 4))
                                       : static_cast<x68k::u8>((old & 0xF0u) | colorIndex);
    sprite.vramWrite8(offset, next);
}

// BG ネームテーブルの 1 セルを設定する。
void setBgCell(Sprite& sprite, x68k::u32 area, x68k::u32 cellX, x68k::u32 cellY, x68k::u16 name)
{
    const x68k::u32 base = area == 0 ? Sprite::kBg0NameOffset : Sprite::kBg1NameOffset;
    const x68k::u32 offset = base + cellY * (Sprite::kBgCellsX * 2u) + cellX * 2u;
    sprite.vramWrite8(offset, static_cast<x68k::u8>(name >> 8));
    sprite.vramWrite8(offset + 1, static_cast<x68k::u8>(name & 0xFFu));
}

// テキスト/スプライトパレットに色を入れ、スプライト面を表示可能にした
// ビデオコントローラを作る。
VideoController makeVideo()
{
    VideoController video;
    video.reset();
    // 色番号 n に、区別できる値を入れておく。
    for (x68k::u32 i = 0; i < VideoController::kTextPaletteCount; ++i)
    {
        video.write(kTextPaletteOffset + i * 2u, static_cast<x68k::u16>(0x0100u * i + 0x0002u));
    }
    // $E82600 の bit6 = スプライト表示許可。
    video.write(kDisplayCtrlOffset, 0x0040u);
    return video;
}

// スプライト面が出るようにした Sprite を作る ($EB0808 の bit9)。
Sprite makeSprite()
{
    Sprite sprite;
    sprite.reset();
    sprite.write(kBgControlOffset, 0x0200u);
    return sprite;
}

constexpr x68k::u32 kOutW = 64;
constexpr x68k::u32 kOutH = 64;

std::vector<x68k::u16> makeOut()
{
    return std::vector<x68k::u16>(kOutW * kOutH, 0);
}

x68k::u16 at(const std::vector<x68k::u16>& out, x68k::u32 x, x68k::u32 y)
{
    return out[y * kOutW + x];
}

}  // namespace

// --- PCG のパターン解釈 ------------------------------------------------------

TEST_CASE("8x8 PCG は 1 行 4 バイト、上位ニブルが左のドットになる")
{
    Sprite sprite;
    sprite.reset();

    // パターン 0 の 0 行目に $12 $34 $56 $78 を置く。
    // 左から順に 1,2,3,4,5,6,7,8 のドットが並ぶはず。
    sprite.vramWrite8(0, 0x12);
    sprite.vramWrite8(1, 0x34);
    sprite.vramWrite8(2, 0x56);
    sprite.vramWrite8(3, 0x78);

    for (x68k::u32 x = 0; x < 8; ++x)
    {
        CHECK(SpriteRaster::pcgPixel8(sprite.vram(), 0, x, 0) == x + 1);
    }
}

TEST_CASE("8x8 PCG のパターン番号は 32 バイト刻み")
{
    Sprite sprite;
    sprite.reset();

    // パターン 3 の先頭 = 3 * 32 = 96 バイト目。
    sprite.vramWrite8(3 * Sprite::kPcg8Bytes, 0xA0);

    CHECK(SpriteRaster::pcgPixel8(sprite.vram(), 3, 0, 0) == 0xA);
    // 隣のパターンには影響しない。
    CHECK(SpriteRaster::pcgPixel8(sprite.vram(), 2, 0, 0) == 0);
    CHECK(SpriteRaster::pcgPixel8(sprite.vram(), 4, 0, 0) == 0);
}

TEST_CASE("16x16 PCG は 8x8 を 4 つ (左上→右上→左下→右下) 並べたもの")
{
    Sprite sprite;
    sprite.reset();

    // 16x16 のパターン 0 は 8x8 のパターン 0-3 を使う。
    // 各 8x8 の先頭ドットに別々の色を置き、どの象限に出るかを見る。
    sprite.vramWrite8(0 * Sprite::kPcg8Bytes, 0x10);  // 8x8 #0 → 左上
    sprite.vramWrite8(1 * Sprite::kPcg8Bytes, 0x20);  // 8x8 #1 → 右上
    sprite.vramWrite8(2 * Sprite::kPcg8Bytes, 0x30);  // 8x8 #2 → 左下
    sprite.vramWrite8(3 * Sprite::kPcg8Bytes, 0x40);  // 8x8 #3 → 右下

    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 0, 0, 0) == 1);
    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 0, 8, 0) == 2);
    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 0, 0, 8) == 3);
    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 0, 8, 8) == 4);
}

TEST_CASE("16x16 PCG のパターン番号は 128 バイト刻み")
{
    Sprite sprite;
    sprite.reset();

    // SP_DEFCG ($FFC0CA) が lsl.w #7 (= x128) でアドレスを作ることに対応する。
    sprite.vramWrite8(1 * Sprite::kPcg16Bytes, 0x50);

    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 1, 0, 0) == 5);
    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 0, 0, 0) == 0);
}

TEST_CASE("PCG の範囲外の座標は 0 を返す")
{
    Sprite sprite;
    sprite.reset();
    fillPattern16(sprite, 0, 7);

    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 0, 16, 0) == 0);
    CHECK(SpriteRaster::pcgPixel(sprite.vram(), 0, 0, 16) == 0);
    CHECK(SpriteRaster::pcgPixel(nullptr, 0, 0, 0) == 0);
}

// --- レジスタの割り当て ------------------------------------------------------

TEST_CASE("スプライトレジスタは 8 バイト刻みで 128 個ぶん並ぶ")
{
    // 根拠: $FFBFDA の初期化が 128 回 x 8 バイトを埋める。
    Sprite sprite;
    sprite.reset();

    sprite.write(spriteReg(0, 0), 0x1111u);
    sprite.write(spriteReg(1, 0), 0x2222u);
    sprite.write(spriteReg(127, 0), 0x3333u);

    CHECK(sprite.read(spriteReg(0, 0)) == 0x1111u);
    CHECK(sprite.read(spriteReg(1, 0)) == 0x2222u);
    CHECK(sprite.read(spriteReg(127, 0)) == 0x3333u);
    // 8 バイト刻みなので、スプライト 1 の先頭はバイト 8。
    CHECK(sprite.read(8) == 0x2222u);
    CHECK(sprite.read(127u * 8u) == 0x3333u);
}

TEST_CASE("座標には 16 の下駄が履かせてあり、画面外の負の位置を表せる")
{
    Sprite sprite;
    sprite.reset();

    // レジスタに 16 を書くと画面左上 (0,0)。
    sprite.write(spriteReg(0, 0), 16);
    sprite.write(spriteReg(0, 1), 16);
    CHECK(sprite.spriteX(0) == 0);
    CHECK(sprite.spriteY(0) == 0);

    // 0 を書くと -16 = 完全に画面外 (16x16 なので右端がちょうど 0)。
    sprite.write(spriteReg(0, 0), 0);
    CHECK(sprite.spriteX(0) == -16);

    sprite.write(spriteReg(0, 0), 100);
    CHECK(sprite.spriteX(0) == 84);
}

TEST_CASE("属性ワードはパターン番号 8bit・反転 2bit・パレットブロック 4bit に分かれる")
{
    Sprite sprite;
    sprite.reset();

    // パレットブロック 5、垂直反転、水平反転、パターン番号 $3C。
    sprite.write(spriteReg(0, 2), 0x533Cu);

    CHECK(sprite.spritePattern(0) == 0x3Cu);
    CHECK(sprite.spriteFlipH(0));
    CHECK(sprite.spriteFlipV(0));
    CHECK(sprite.spritePaletteBlock(0) == 5);

    // 反転なし。
    sprite.write(spriteReg(0, 2), 0x0012u);
    CHECK(sprite.spritePattern(0) == 0x12u);
    CHECK_FALSE(sprite.spriteFlipH(0));
    CHECK_FALSE(sprite.spriteFlipV(0));
    CHECK(sprite.spritePaletteBlock(0) == 0);

    // 水平反転だけ (bit8)。
    sprite.write(spriteReg(0, 2), 0x0100u);
    CHECK(sprite.spriteFlipH(0));
    CHECK_FALSE(sprite.spriteFlipV(0));

    // 垂直反転だけ (bit9)。
    sprite.write(spriteReg(0, 2), 0x0200u);
    CHECK_FALSE(sprite.spriteFlipH(0));
    CHECK(sprite.spriteFlipV(0));
}

TEST_CASE("プライオリティは 2bit で、0 は非表示")
{
    // 根拠: SP_REGST ($FFC158) が 4 ワード目を andi.w #$3 で丸める。
    Sprite sprite;
    sprite.reset();

    CHECK_FALSE(sprite.spriteVisible(0));
    CHECK(sprite.visibleSpriteCount() == 0);

    sprite.write(spriteReg(0, 3), 2);
    CHECK(sprite.spritePriority(0) == 2);
    CHECK(sprite.spriteVisible(0));
    CHECK(sprite.visibleSpriteCount() == 1);

    // 上位ビットは無視される。
    sprite.write(spriteReg(0, 3), 0xFFFCu);
    CHECK(sprite.spritePriority(0) == 0);
    CHECK_FALSE(sprite.spriteVisible(0));
    CHECK(sprite.visibleSpriteCount() == 0);

    // bit2 以上は 2bit のフィールドに入らない。
    //
    // ここを 3bit 以上として読むと、$0004 が「プライオリティ 4」に化けて
    // 表示扱いになる。実機は andi.w #$3 で丸めるので 0 = 非表示のまま。
    sprite.write(spriteReg(0, 3), 0x0004u);
    CHECK(sprite.spritePriority(0) == 0);
    CHECK_FALSE(sprite.spriteVisible(0));
    CHECK(sprite.visibleSpriteCount() == 0);

    // 取りうる値は 0-3 に収まる。
    sprite.write(spriteReg(0, 3), 0x0007u);
    CHECK(sprite.spritePriority(0) == 3);
}

TEST_CASE("表示中のスプライト数はプライオリティの書き込みで追随する")
{
    Sprite sprite;
    sprite.reset();

    sprite.write(spriteReg(0, 3), 1);
    sprite.write(spriteReg(5, 3), 3);
    sprite.write(spriteReg(127, 3), 2);
    CHECK(sprite.visibleSpriteCount() == 3);

    sprite.write(spriteReg(5, 3), 0);
    CHECK(sprite.visibleSpriteCount() == 2);

    // 座標の書き込みでは数が変わらない。
    sprite.write(spriteReg(0, 0), 100);
    CHECK(sprite.visibleSpriteCount() == 2);
}

// --- BG レジスタ -------------------------------------------------------------

TEST_CASE("BG スクロールは BG0 が $EB0800、BG1 が $EB0804")
{
    // 根拠: BGSCRLST ($FFC198 / $FFC1A0) が btst #0,d1 で選ぶ 2 つのアドレス。
    Sprite sprite;
    sprite.reset();

    sprite.write(kBgScrollOffset + 0, 0x0011u);  // BG0 X
    sprite.write(kBgScrollOffset + 2, 0x0022u);  // BG0 Y
    sprite.write(kBgScrollOffset + 4, 0x0033u);  // BG1 X
    sprite.write(kBgScrollOffset + 6, 0x0044u);  // BG1 Y

    CHECK(sprite.bgScrollX(0) == 0x0011u);
    CHECK(sprite.bgScrollY(0) == 0x0022u);
    CHECK(sprite.bgScrollX(1) == 0x0033u);
    CHECK(sprite.bgScrollY(1) == 0x0044u);
}

TEST_CASE("BG 制御は 3bit ずつで bit0 が表示・bit1 がネームテーブル番号")
{
    // 根拠: BGCTRLST ($FFC1F6-$FFC216)。表示ビット (d3) はシフトなしで bit0、
    // ネームテーブル番号 (d2) は add.l d2,d2 で bit1。BG1 は 3bit 左へずれる。
    Sprite sprite;
    sprite.reset();

    // BG0 表示 ON、ネームテーブル 0。
    sprite.write(kBgControlOffset, 0x0001u);
    CHECK(sprite.bgEnabled(0));
    CHECK(sprite.bgTextArea(0) == 0);
    CHECK_FALSE(sprite.bgEnabled(1));

    // BG0 表示 ON、ネームテーブル 1。
    sprite.write(kBgControlOffset, 0x0003u);
    CHECK(sprite.bgEnabled(0));
    CHECK(sprite.bgTextArea(0) == 1);

    // BG1 表示 ON、ネームテーブル 0 (bit3)。
    sprite.write(kBgControlOffset, 0x0008u);
    CHECK_FALSE(sprite.bgEnabled(0));
    CHECK(sprite.bgEnabled(1));
    CHECK(sprite.bgTextArea(1) == 0);

    // BG1 ネームテーブル 1 (bit4) だけ = 表示は OFF。
    // これは起動時に IPL-ROM が書く値 ($FF6426 の move.w #$10,$EB0808)。
    sprite.write(kBgControlOffset, 0x0010u);
    CHECK_FALSE(sprite.bgEnabled(1));
    CHECK(sprite.bgTextArea(1) == 1);
}

TEST_CASE("スプライト面の表示は $EB0808 の bit9")
{
    // 根拠: SP_ON ($FFC05C) の ori.w #$200,$EB0808 と
    //       SP_OFF ($FFC06E) の andi.w #$FDFF,$EB0808。
    Sprite sprite;
    sprite.reset();

    CHECK_FALSE(sprite.displayEnabled());
    sprite.write(kBgControlOffset, 0x0200u);
    CHECK(sprite.displayEnabled());
    sprite.write(kBgControlOffset, 0x0000u);
    CHECK_FALSE(sprite.displayEnabled());
}

TEST_CASE("スプライト面は $E82600 の bit6 と $EB0808 の bit9 の両方が要る")
{
    // IOCS は必ず対で操作する。片方だけでは出ない。
    Sprite sprite;
    sprite.reset();
    sprite.write(spriteReg(0, 3), 1);  // 表示中のスプライトを 1 つ用意

    VideoController video;
    video.reset();

    // どちらも OFF。
    CHECK_FALSE(SpriteRaster::hasVisibleContent(sprite, video));

    // $E82600 だけ ON。
    video.write(kDisplayCtrlOffset, 0x0040u);
    CHECK_FALSE(SpriteRaster::hasVisibleContent(sprite, video));

    // $EB0808 だけ ON。
    video.write(kDisplayCtrlOffset, 0x0000u);
    sprite.write(kBgControlOffset, 0x0200u);
    CHECK_FALSE(SpriteRaster::hasVisibleContent(sprite, video));

    // 両方 ON。
    video.write(kDisplayCtrlOffset, 0x0040u);
    CHECK(SpriteRaster::hasVisibleContent(sprite, video));
}

// --- スプライトの描画 --------------------------------------------------------

TEST_CASE("スプライトは指定した座標に 16x16 で描かれる")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    fillPattern16(sprite, 0, 1);
    placeSprite(sprite, 0, 10, 20, 0x0000u, 1);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    const x68k::u16 expected = VideoController::toRgb565(video.textPalette(1));

    // 内側は塗られている。
    CHECK(at(out, 10, 20) == expected);
    CHECK(at(out, 25, 35) == expected);
    // 外側は触られていない。
    CHECK(at(out, 9, 20) == 0);
    CHECK(at(out, 10, 19) == 0);
    CHECK(at(out, 26, 20) == 0);
    CHECK(at(out, 10, 36) == 0);
}

TEST_CASE("色番号 0 は透明で背後が透ける")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();

    // パターン全体は透明のまま、1 ドットだけ色 3 にする。
    setPatternPixel16(sprite, 0, 4, 6, 3);
    placeSprite(sprite, 0, 0, 0, 0x0000u, 1);

    std::vector<x68k::u16> out = makeOut();
    // 背景を非 0 で埋めておき、透明ドットで残ることを見る。
    for (auto& v : out)
    {
        v = 0xBEEFu;
    }

    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 4, 6) == VideoController::toRgb565(video.textPalette(3)));
    CHECK(at(out, 3, 6) == 0xBEEFu);
    CHECK(at(out, 0, 0) == 0xBEEFu);
}

TEST_CASE("水平反転はパターンを左右に、垂直反転は上下に入れ替える")
{
    VideoController video = makeVideo();

    // 左上 (1,2) にだけ色を置く。反転すると対角の位置へ動く。
    const x68k::u32 px = 1;
    const x68k::u32 py = 2;

    SUBCASE("反転なし")
    {
        Sprite sprite = makeSprite();
        setPatternPixel16(sprite, 0, px, py, 4);
        placeSprite(sprite, 0, 0, 0, 0x0000u, 1);

        std::vector<x68k::u16> out = makeOut();
        SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);
        CHECK(at(out, px, py) != 0);
    }

    SUBCASE("水平反転 (bit8)")
    {
        Sprite sprite = makeSprite();
        setPatternPixel16(sprite, 0, px, py, 4);
        placeSprite(sprite, 0, 0, 0, 0x0100u, 1);

        std::vector<x68k::u16> out = makeOut();
        SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);
        CHECK(at(out, 15 - px, py) != 0);
        CHECK(at(out, px, py) == 0);
    }

    SUBCASE("垂直反転 (bit9)")
    {
        Sprite sprite = makeSprite();
        setPatternPixel16(sprite, 0, px, py, 4);
        placeSprite(sprite, 0, 0, 0, 0x0200u, 1);

        std::vector<x68k::u16> out = makeOut();
        SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);
        CHECK(at(out, px, 15 - py) != 0);
        CHECK(at(out, px, py) == 0);
    }

    SUBCASE("両方反転")
    {
        Sprite sprite = makeSprite();
        setPatternPixel16(sprite, 0, px, py, 4);
        placeSprite(sprite, 0, 0, 0, 0x0300u, 1);

        std::vector<x68k::u16> out = makeOut();
        SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);
        CHECK(at(out, 15 - px, 15 - py) != 0);
        CHECK(at(out, px, py) == 0);
    }
}

TEST_CASE("プライオリティ 0 のスプライトは描かれない")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    fillPattern16(sprite, 0, 1);
    placeSprite(sprite, 0, 0, 0, 0x0000u, 0);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 0, 0) == 0);
}

TEST_CASE("プライオリティが大きいスプライトほど手前に描かれる")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();

    // 同じ位置に 2 つ重ねる。手前 (priority 3) の色が残るはず。
    fillPattern16(sprite, 0, 1);
    fillPattern16(sprite, 1, 2);
    placeSprite(sprite, 0, 0, 0, 0x0000u, 3);  // パターン 0、色 1、手前
    placeSprite(sprite, 1, 0, 0, 0x0001u, 1);  // パターン 1、色 2、奥

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 5, 5) == VideoController::toRgb565(video.textPalette(1)));
}

TEST_CASE("プライオリティが同値なら番号の小さいスプライトが手前")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();

    fillPattern16(sprite, 0, 1);
    fillPattern16(sprite, 1, 2);
    placeSprite(sprite, 0, 0, 0, 0x0000u, 2);  // 番号 0 = 手前
    placeSprite(sprite, 1, 0, 0, 0x0001u, 2);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 5, 5) == VideoController::toRgb565(video.textPalette(1)));
}

TEST_CASE("画面外にはみ出したスプライトは切り取られる")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    fillPattern16(sprite, 0, 1);

    // 左上へはみ出す。右下の 8x8 ぶんだけが見える。
    placeSprite(sprite, 0, -8, -8, 0x0000u, 1);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 0, 0) != 0);
    CHECK(at(out, 7, 7) != 0);
    CHECK(at(out, 8, 8) == 0);  // スプライトの右下端の外
}

TEST_CASE("完全に画面外のスプライトは何も書かない")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    fillPattern16(sprite, 0, 1);
    placeSprite(sprite, 0, -16, 0, 0x0000u, 1);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    for (const auto v : out)
    {
        CHECK(v == 0);
    }
}

TEST_CASE("切り出し位置 (srcX/srcY) をずらすとスプライトも相対に動く")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    fillPattern16(sprite, 0, 1);
    placeSprite(sprite, 0, 30, 30, 0x0000u, 1);

    std::vector<x68k::u16> out = makeOut();
    // (20,20) から切り出せば、スプライトは出力の (10,10) に来る。
    SpriteRaster::renderSprites(sprite, video, 20, 20, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 10, 10) != 0);
    CHECK(at(out, 9, 10) == 0);
    CHECK(at(out, 10, 9) == 0);
}

// --- スプライトを出さないときの費用 ------------------------------------------

TEST_CASE("スプライトが無効なら出力バッファに一切触らない")
{
    // ESP32-S3 の実効 3MHz では、使っていない機能が毎フレーム走るだけで
    // フレームレートに効く。Human68k のコンソールはこの経路しか通らない。
    Sprite sprite;
    sprite.reset();
    VideoController video = makeVideo();
    fillPattern16(sprite, 0, 1);
    placeSprite(sprite, 0, 0, 0, 0x0000u, 1);

    std::vector<x68k::u16> out = makeOut();
    for (auto& v : out)
    {
        v = 0x1234u;
    }

    // $EB0808 の bit9 が立っていないので何も描かない。
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);
    SpriteRaster::renderPlane(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    for (const auto v : out)
    {
        CHECK(v == 0x1234u);
    }
}

TEST_CASE("表示中のスプライトが 1 つも無ければ走査しない")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    fillPattern16(sprite, 0, 1);
    // 座標とパターンは設定するが、プライオリティは 0 のまま。
    sprite.write(spriteReg(0, 0), 16);
    sprite.write(spriteReg(0, 1), 16);

    CHECK_FALSE(sprite.anySpriteVisible());

    std::vector<x68k::u16> out = makeOut();
    for (auto& v : out)
    {
        v = 0x4321u;
    }
    SpriteRaster::renderSprites(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    for (const auto v : out)
    {
        CHECK(v == 0x4321u);
    }
}

TEST_CASE("1 ラインに重ねられるスプライトは 32 個まで")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();

    // 33 個を同じラインの別々の X に並べる。33 個目は出ない。
    fillPattern16(sprite, 0, 1);
    for (x68k::u32 i = 0; i < 33; ++i)
    {
        placeSprite(sprite, i, static_cast<int>(i * 16u), 0, 0x0000u, 1);
    }

    std::vector<x68k::u16> out(1024 * 16, 0);
    SpriteRaster::renderSprites(sprite, video, 0, 0, 1024, 16, out.data(), 1024);

    // 32 個目 (index 31) までは出る。
    CHECK(out[31u * 16u] != 0);
    // 33 個目 (index 32) は上限で落ちる。
    CHECK(out[32u * 16u] == 0);
}

// --- BG の描画 ---------------------------------------------------------------

TEST_CASE("BG はネームテーブルの指すパターンをセルに並べる")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    sprite.write(kBgControlOffset, 0x0201u);  // スプライト面 ON + BG0 表示

    fillPattern16(sprite, 1, 6);
    // BG0 のネームテーブル (エリア 0) のセル (1,2) にパターン 1 を置く。
    setBgCell(sprite, 0, 1, 2, 0x0001u);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);

    const x68k::u16 expected = VideoController::toRgb565(video.textPalette(6));
    // セル (1,2) は画面座標 (16,32) から 16x16。
    CHECK(at(out, 16, 32) == expected);
    CHECK(at(out, 31, 47) == expected);
    CHECK(at(out, 15, 32) == 0);
    CHECK(at(out, 16, 31) == 0);
}

TEST_CASE("BG のスクロールはセルの位置をずらす")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    sprite.write(kBgControlOffset, 0x0201u);

    fillPattern16(sprite, 1, 6);
    setBgCell(sprite, 0, 1, 0, 0x0001u);  // セル (1,0) = 画面 (16,0)

    // X に 8 スクロールすると、絵は左へ 8 動く。
    sprite.write(kBgScrollOffset + 0, 8);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 8, 0) != 0);  // 元の 16 が 8 へ
    CHECK(at(out, 7, 0) == 0);
    CHECK(at(out, 23, 0) != 0);  // 右端 31 が 23 へ
    CHECK(at(out, 24, 0) == 0);
}

TEST_CASE("BG の縦スクロールも同じように効く")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    sprite.write(kBgControlOffset, 0x0201u);

    fillPattern16(sprite, 1, 6);
    setBgCell(sprite, 0, 0, 1, 0x0001u);   // セル (0,1) = 画面 (0,16)
    sprite.write(kBgScrollOffset + 2, 4);  // BG0 Y

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 0, 12) != 0);
    CHECK(at(out, 0, 11) == 0);
}

TEST_CASE("BG は 64x64 セルで巻き戻る")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    sprite.write(kBgControlOffset, 0x0201u);

    fillPattern16(sprite, 1, 6);
    setBgCell(sprite, 0, 0, 0, 0x0001u);  // 左上のセル

    // BG 面は 64 セル x 16 ドット = 1024 ドット。ちょうど 1 周させると
    // 同じ絵が同じ位置に出る。
    sprite.write(kBgScrollOffset + 0, 1024);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 0, 0) != 0);
    CHECK(at(out, 15, 0) != 0);
    CHECK(at(out, 16, 0) == 0);
}

TEST_CASE("BG のネームテーブル番号で $EBC000 と $EBE000 を切り替える")
{
    VideoController video = makeVideo();

    SUBCASE("エリア 0 は $EBC000")
    {
        Sprite sprite = makeSprite();
        sprite.write(kBgControlOffset, 0x0201u);  // BG0 表示、エリア 0
        fillPattern16(sprite, 1, 6);
        setBgCell(sprite, 0, 0, 0, 0x0001u);

        std::vector<x68k::u16> out = makeOut();
        SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);
        CHECK(at(out, 0, 0) != 0);
    }

    SUBCASE("エリア 1 は $EBE000")
    {
        Sprite sprite = makeSprite();
        sprite.write(kBgControlOffset, 0x0203u);  // BG0 表示、エリア 1
        fillPattern16(sprite, 1, 6);
        // エリア 0 にだけ置いても出ない。
        setBgCell(sprite, 0, 0, 0, 0x0001u);

        std::vector<x68k::u16> out = makeOut();
        SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);
        CHECK(at(out, 0, 0) == 0);

        // エリア 1 に置けば出る。
        setBgCell(sprite, 1, 0, 0, 0x0001u);
        SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);
        CHECK(at(out, 0, 0) != 0);
    }
}

TEST_CASE("BG のネームテーブルも反転ビットを持つ")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    sprite.write(kBgControlOffset, 0x0201u);

    setPatternPixel16(sprite, 1, 1, 2, 4);
    // パターン 1 を水平反転で置く。
    setBgCell(sprite, 0, 0, 0, 0x0101u);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 14, 2) != 0);  // 15 - 1
    CHECK(at(out, 1, 2) == 0);
}

TEST_CASE("BG が無効なら何も描かない")
{
    Sprite sprite = makeSprite();  // bit9 は立っているが BG は無効
    VideoController video = makeVideo();
    fillPattern16(sprite, 1, 6);
    setBgCell(sprite, 0, 0, 0, 0x0001u);

    std::vector<x68k::u16> out = makeOut();
    for (auto& v : out)
    {
        v = 0x5555u;
    }
    SpriteRaster::renderBg(sprite, video, 0, 0, 0, kOutW, kOutH, out.data(), kOutW);

    for (const auto v : out)
    {
        CHECK(v == 0x5555u);
    }
}

// --- 面の重ね合わせ ----------------------------------------------------------

TEST_CASE("スプライトは BG より手前に描かれる")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    sprite.write(kBgControlOffset, 0x0201u);  // スプライト面 ON + BG0 表示

    fillPattern16(sprite, 1, 6);  // BG のパターン
    fillPattern16(sprite, 2, 9);  // スプライトのパターン
    setBgCell(sprite, 0, 0, 0, 0x0001u);
    placeSprite(sprite, 0, 0, 0, 0x0002u, 1);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderPlane(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    // 同じ位置ではスプライトの色 9 が勝つ。
    CHECK(at(out, 5, 5) == VideoController::toRgb565(video.textPalette(9)));
}

TEST_CASE("BG0 は BG1 より手前に描かれる")
{
    Sprite sprite = makeSprite();
    VideoController video = makeVideo();
    // BG0 表示 (bit0) + BG1 表示 (bit3) + スプライト面 (bit9)。
    // BG1 はエリア 1 を使う (bit4) ので、別のネームテーブルになる。
    sprite.write(kBgControlOffset, 0x0219u);

    CHECK(sprite.bgEnabled(0));
    CHECK(sprite.bgEnabled(1));
    CHECK(sprite.bgTextArea(0) == 0);
    CHECK(sprite.bgTextArea(1) == 1);

    fillPattern16(sprite, 1, 6);  // BG0 のパターン
    fillPattern16(sprite, 2, 9);  // BG1 のパターン
    setBgCell(sprite, 0, 0, 0, 0x0001u);
    setBgCell(sprite, 1, 0, 0, 0x0002u);

    std::vector<x68k::u16> out = makeOut();
    SpriteRaster::renderPlane(sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

    CHECK(at(out, 5, 5) == VideoController::toRgb565(video.textPalette(6)));
}

TEST_CASE("スプライト面はテキスト/グラフィックと同じ $E82500 の順位を持つ")
{
    // spritePriority() は video.h の既存の読み方をそのまま使う。
    // IPL-ROM の既定値 $06E4 では SP=0 (最も手前)、TX=1、GR=2。
    VideoController video;
    video.reset();
    video.write(kPriorityOffset, 0x06E4u);

    CHECK(video.spritePriority() == 0);
    CHECK(video.textPriority() == 1);
    CHECK(video.graphicPriority() == 2);
}

// --- バス経由のアクセス ------------------------------------------------------

TEST_CASE("$EB0000 のレジスタと $EB8000 の VRAM がバス経由で読み書きできる")
{
    x68k::Machine machine;
    std::vector<x68k::u8> ram(x68k::kMainRamSize, 0);
    std::vector<x68k::u8> tvram(x68k::kTvramSize, 0);
    std::vector<x68k::u8> iplrom(x68k::kIplromSize, 0);

    x68k::MemoryMap mem;
    mem.mainRam = ram.data();
    mem.textVram = tvram.data();
    mem.iplRom = iplrom.data();
    machine.setMemory(mem);
    machine.reset();

    x68k::SystemBus& bus = machine.bus();

    SUBCASE("スプライトレジスタはワードで往復する")
    {
        bus.write16(x68k::kSpriteRegBase + 0, 0x1234u);
        CHECK(bus.read16(x68k::kSpriteRegBase + 0) == 0x1234u);
        CHECK(machine.sprite().read(0) == 0x1234u);

        // 128 個目 (最後) のレジスタ。ここまで届くことがレジスタ数の裏付け。
        const x68k::u32 last = x68k::kSpriteRegBase + 127u * 8u;
        bus.write16(last, 0x0030u);
        CHECK(bus.read16(last) == 0x0030u);
        // X 座標は 16 の下駄を引いた値になる。
        CHECK(machine.sprite().spriteX(127) == 0x30 - 16);
    }

    SUBCASE("バイトアクセスはワードの上下を切り出す")
    {
        bus.write16(x68k::kSpriteRegBase + 0, 0x1234u);
        CHECK(bus.read8(x68k::kSpriteRegBase + 0) == 0x12u);
        CHECK(bus.read8(x68k::kSpriteRegBase + 1) == 0x34u);

        bus.write8(x68k::kSpriteRegBase + 1, 0x56u);
        CHECK(machine.sprite().read(0) == 0x1256u);
    }

    SUBCASE("BG 制御レジスタへ届く")
    {
        bus.write16(x68k::kSpriteRegBase + 0x808u, 0x0201u);
        CHECK(machine.sprite().displayEnabled());
        CHECK(machine.sprite().bgEnabled(0));
    }

    SUBCASE("スプライト VRAM はバイト単位で往復する")
    {
        bus.write8(x68k::kSpriteVramBase, 0xAAu);
        bus.write8(x68k::kSpriteVramBase + 1, 0xBBu);
        CHECK(bus.read8(x68k::kSpriteVramBase) == 0xAAu);
        CHECK(bus.read16(x68k::kSpriteVramBase) == 0xAABBu);
        CHECK(machine.sprite().vramRead8(0) == 0xAAu);
    }

    SUBCASE("BG ネームテーブルは PCG と連続した 1 つの実体")
    {
        // $EBC000 は VRAM の先頭から $4000。
        bus.write16(0xEBC000u, 0x1122u);
        CHECK(machine.sprite().vramRead8(Sprite::kBg0NameOffset) == 0x11u);
        CHECK(machine.sprite().vramRead8(Sprite::kBg0NameOffset + 1) == 0x22u);

        // $EBE000 は $6000。
        bus.write16(0xEBE000u, 0x3344u);
        CHECK(machine.sprite().vramRead8(Sprite::kBg1NameOffset) == 0x33u);

        // VRAM の最終バイト ($EBFFFF)。
        bus.write8(x68k::kSpriteVramEnd - 1, 0x77u);
        CHECK(machine.sprite().vramRead8(Sprite::kVramSize - 1) == 0x77u);
    }

    SUBCASE("リセットでレジスタと VRAM が消える")
    {
        bus.write16(x68k::kSpriteRegBase + 0, 0x1234u);
        bus.write8(x68k::kSpriteVramBase, 0xAAu);
        machine.reset();
        CHECK(machine.sprite().read(0) == 0);
        CHECK(machine.sprite().vramRead8(0) == 0);
    }
}
