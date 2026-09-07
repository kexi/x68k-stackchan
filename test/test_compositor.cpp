// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: 画面に出る面 (グラフィック・テキスト・BG・スプライト) が
// 1 回の呼び出しで漏れなく重なり、その前後関係が決まった順になること。
//
// なぜこのテストが要るか: SpriteRaster は実装もテストも揃っていたのに、
// 合成の呼び出し側が誰も呼んでいなかったため、スプライトを使うソフトの絵は
// ホストでも実機でも 1 ドットも出なかった。個々のラスタライザが正しいことは
// 「画面に出る」ことを意味しない。面が繋がっていること自体を検査対象にする。

#include <vector>

#include "dev/sprite.h"
#include "dev/video.h"
#include "doctest.h"
#include "video/compositor.h"
#include "video/graphic_raster.h"

namespace
{

using x68k::Compositor;
using x68k::Sprite;
using x68k::VideoController;

constexpr x68k::u32 kTextPaletteOffset = 0x200;
constexpr x68k::u32 kDisplayCtrlOffset = 0x600;
constexpr x68k::u32 kBgControlOffset = 0x808;

constexpr x68k::u32 kOutW = 64;
constexpr x68k::u32 kOutH = 64;

constexpr x68k::u32 spriteReg(x68k::u32 index, x68k::u32 word)
{
    return index * Sprite::kSpriteStride + word * 2u;
}

// テキストとスプライトの両方を表示許可したビデオコントローラ。
// $E82600 の bit5 = テキスト、bit6 = スプライト。
VideoController makeVideo()
{
    VideoController video;
    video.reset();
    for (x68k::u32 i = 0; i < VideoController::kTextPaletteCount; ++i)
    {
        video.write(kTextPaletteOffset + i * 2u, static_cast<x68k::u16>(0x0100u * i + 0x0002u));
    }
    video.write(kDisplayCtrlOffset, 0x0060u);
    return video;
}

Sprite makeSprite()
{
    Sprite sprite;
    sprite.reset();
    sprite.write(kBgControlOffset, 0x0200u);
    return sprite;
}

// 16x16 PCG パターンを 1 色で塗る (8x8 を 4 つぶん = 128 バイト)。
void fillPattern16(Sprite& sprite, x68k::u32 pattern, x68k::u8 colorIndex)
{
    const x68k::u8 packed = static_cast<x68k::u8>((colorIndex << 4) | colorIndex);
    const x68k::u32 base = pattern * 128u;
    for (x68k::u32 i = 0; i < 128u; ++i)
    {
        sprite.vramWrite8(base + i, packed);
    }
}

void placeSprite(Sprite& sprite, x68k::u32 index, int x, int y, x68k::u8 priority)
{
    sprite.write(spriteReg(index, 0), static_cast<x68k::u16>(x + Sprite::kCoordOffset));
    sprite.write(spriteReg(index, 1), static_cast<x68k::u16>(y + Sprite::kCoordOffset));
    sprite.write(spriteReg(index, 2), 0x0000u);
    sprite.write(spriteReg(index, 3), priority);
}

x68k::u16 at(const std::vector<x68k::u16>& out, x68k::u32 x, x68k::u32 y)
{
    return out[y * kOutW + x];
}

std::vector<x68k::u16> makeOut()
{
    return std::vector<x68k::u16>(kOutW * kOutH, 0);
}

}  // namespace

TEST_SUITE("compositor")
{
    TEST_CASE("MODE3 G0 scrollはgraphicsだけを動かしtextとsprite位置を保持する")
    {
        auto video = makeVideo();
        auto sprite = makeSprite();
        x68k::Crtc crtc;
        video.write(0x400, 3);
        video.write(0x500, 0x0200);  // textがgraphicより前
        video.write(0x600, 0x7F);
        fillPattern16(sprite, 0, 2);
        placeSprite(sprite, 0, 8, 12, 1);
        std::vector<x68k::u8> graphic(0x80000), text(0x80000);
        for (x68k::u32 y = 0; y < 512; ++y)
            for (x68k::u32 x = 0; x < 512; ++x)
            {
                const auto word = static_cast<x68k::u16>(((x * 31u + y * 173u) & 0xFFFEu) | 1u);
                graphic[(y * 512u + x) * 2u] = static_cast<x68k::u8>(word >> 8);
                graphic[(y * 512u + x) * 2u + 1u] = static_cast<x68k::u8>(word);
            }
        text[2 * 128] = 0x80;  // text index1 at(0,2)
        for (const x68k::u16 scroll : std::initializer_list<x68k::u16>{0, 1, 256, 511})
        {
            crtc.write(12, scroll);
            crtc.write(13, scroll);
            auto out = makeOut();
            Compositor::render(graphic.data(), text.data(), &sprite, video, 0, 0, kOutW, kOutH,
                               out.data(), kOutW, &crtc);
            for (x68k::u32 y = 0; y < kOutH; ++y)
                for (x68k::u32 x = 0; x < kOutW; ++x)
                {
                    const auto px = (x + scroll) & 511u;
                    const auto py = (y + scroll) & 511u;
                    const auto word =
                        static_cast<x68k::u16>(((px * 31u + py * 173u) & 0xFFFEu) | 1u);
                    auto expected = VideoController::toRgb565(word);
                    if (const bool onText = x == 0 && y == 2; onText)
                        expected = VideoController::toRgb565(video.textPalette(1));
                    if (const bool onSprite = x >= 8 && x < 24 && y >= 12 && y < 28; onSprite)
                        expected = VideoController::toRgb565(video.textPalette(2));
                    CHECK(at(out, x, y) == expected);
                }
        }
    }

    TEST_CASE("スプライトが合成の結果に現れる")
    {
        // これがこのファイルの主題。個々のラスタライザではなく、
        // 「1 回の呼び出しでスプライトまで届くか」を見る。
        Sprite sprite = makeSprite();
        VideoController video = makeVideo();
        fillPattern16(sprite, 0, 1);
        placeSprite(sprite, 0, 8, 12, 1);

        std::vector<x68k::u16> out = makeOut();
        Compositor::render(nullptr, nullptr, &sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

        const x68k::u16 expected = VideoController::toRgb565(video.textPalette(1));
        CHECK(at(out, 8, 12) == expected);
        CHECK(at(out, 23, 27) == expected);
        // 枠の外は背景 (黒) のまま。
        CHECK(at(out, 7, 12) == 0);
        CHECK(at(out, 24, 12) == 0);
    }

    TEST_CASE("スプライトはグラフィックとテキストより手前に出る")
    {
        Sprite sprite = makeSprite();
        VideoController video = makeVideo();
        fillPattern16(sprite, 0, 2);
        placeSprite(sprite, 0, 0, 0, 1);

        // テキスト VRAM の左上を埋めて、同じ位置でスプライトが勝つことを見る。
        std::vector<x68k::u8> textVram(x68k::kTvramSize, 0);
        for (x68k::u32 i = 0; i < 4u; ++i)
        {
            textVram[i] = 0xFFu;
        }

        std::vector<x68k::u16> out = makeOut();
        Compositor::render(nullptr, textVram.data(), &sprite, video, 0, 0, kOutW, kOutH, out.data(),
                           kOutW);

        CHECK(at(out, 0, 0) == VideoController::toRgb565(video.textPalette(2)));
    }

    TEST_CASE("スプライトが無いときの結果は従来の合成と一致する")
    {
        // 既存の表示経路 (Human68k のコンソール) を変えていないことの保証。
        // スプライト面を出さない限り、絵は以前と 1 ドットも変わってはいけない。
        Sprite sprite;
        sprite.reset();  // $EB0808 を書かない = スプライト面 OFF
        VideoController video = makeVideo();

        std::vector<x68k::u8> textVram(x68k::kTvramSize, 0);
        for (x68k::u32 i = 0; i < 8u; ++i)
        {
            textVram[i] = 0xA5u;
        }

        std::vector<x68k::u16> viaCompositor = makeOut();
        Compositor::render(nullptr, textVram.data(), &sprite, video, 0, 0, kOutW, kOutH,
                           viaCompositor.data(), kOutW);

        std::vector<x68k::u16> viaGraphic = makeOut();
        x68k::GraphicRaster::composite(nullptr, textVram.data(), video, 0, 0, kOutW, kOutH,
                                       viaGraphic.data(), kOutW);

        CHECK(viaCompositor == viaGraphic);
    }

    TEST_CASE("sprite が nullptr でも落ちず、グラフィック/テキストは出る")
    {
        // 実機の初期化前など、スプライトデバイスを渡せない経路を想定する。
        VideoController video = makeVideo();
        std::vector<x68k::u8> textVram(x68k::kTvramSize, 0);
        textVram[0] = 0xFFu;

        std::vector<x68k::u16> out = makeOut();
        Compositor::render(nullptr, textVram.data(), nullptr, video, 0, 0, kOutW, kOutH, out.data(),
                           kOutW);

        CHECK(at(out, 0, 0) != 0);
    }

    TEST_CASE("BG はスプライトより奥に出る")
    {
        Sprite sprite = makeSprite();
        VideoController video = makeVideo();

        // BG0 を色 3 のパターンで敷き詰め、その上にスプライト (色 1) を置く。
        fillPattern16(sprite, 0, 3);
        fillPattern16(sprite, 1, 1);
        // BG0 表示 + ネームテーブル 0 ($EB0808 の bit0-2)。
        sprite.write(kBgControlOffset, 0x0201u);
        for (x68k::u32 cell = 0; cell < 64u * 64u; ++cell)
        {
            const x68k::u32 offset = Sprite::kBg0NameOffset + cell * 2u;
            sprite.vramWrite8(offset, 0);
            sprite.vramWrite8(offset + 1, 0);
        }

        sprite.write(spriteReg(0, 0), static_cast<x68k::u16>(0 + Sprite::kCoordOffset));
        sprite.write(spriteReg(0, 1), static_cast<x68k::u16>(0 + Sprite::kCoordOffset));
        sprite.write(spriteReg(0, 2), 0x0001u);  // パターン 1
        sprite.write(spriteReg(0, 3), 1);

        std::vector<x68k::u16> out = makeOut();
        Compositor::render(nullptr, nullptr, &sprite, video, 0, 0, kOutW, kOutH, out.data(), kOutW);

        // スプライトが乗っている位置はスプライトの色。
        CHECK(at(out, 0, 0) == VideoController::toRgb565(video.textPalette(1)));
        // スプライトの外は BG の色。
        CHECK(at(out, 40, 40) == VideoController::toRgb565(video.textPalette(3)));
    }
}
