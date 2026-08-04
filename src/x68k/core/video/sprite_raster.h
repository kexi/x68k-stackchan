// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// スプライト面と BG 面を RGB565 に変換する。
//
// スプライトと BG はどちらも PCG (4bpp のキャラクタパターン) を並べたもので、
// 色はテキスト/スプライトパレット ($E82200 の 16 色) を 16 ブロックに
// 区切って引く。グラフィックパレットは使わない。
//
// --- パレットの引き方 --------------------------------------------------------
//
// PCG の 1 ドットは 4bit で、これがパレットブロック内の色番号になる。
// 実際に引く色は「ブロック番号 x 16 + 色番号」だが、VideoController が
// 持つテキストパレットは 16 色しかない。
//
// 【裏付けの限界】実機のテキスト/スプライトパレットは $E82200 から
// 16 色 x 16 ブロック = 256 エントリある。現状の VideoController は
// kTextPaletteCount = 16 しか持たず、$E82200 の上位ブロックを保持していない。
// video.h は本タスクの担当範囲外なので拡張していない。ここではブロック番号を
// 無視して下位 4bit だけでテキストパレットを引く。単一ブロック (ブロック 0)
// しか使わないソフトなら実機と一致するが、複数ブロックを使い分けるソフトでは
// 色が化ける。video.h が 256 エントリを持つようになったら
// paletteIndexOf() を「block * 16 + index」へ直すこと。
//
// --- 透明 --------------------------------------------------------------------
//
// PCG の色番号 0 は透明。グラフィック画面と同じ規則で、背後の面が透ける。
// ブロックが何であっても 0 は透明 (ブロック x 16 の色を引かない)。
//
// --- 性能 --------------------------------------------------------------------
//
// ESP32-S3 の実効 3MHz では、128 スプライトをドットごとに走査する実装は
// 到底間に合わない。ここでは 2 段構えで費用を抑える:
//
//   1. スプライトも BG も出ていないなら、関数の入口で即座に返る。
//      Human68k のコンソールはこの経路しか通らないので、
//      スプライト機能を持つこと自体の費用がほぼゼロになる。
//   2. 出ているときは、ラインごとに「そのラインに掛かるスプライト」だけを
//      集めてから走査する。128 個ぶんの矩形判定は 1 ライン 1 回で済み、
//      ドットごとのループは実際に掛かっている数しか回らない。

#ifndef X68K_CORE_VIDEO_SPRITE_RASTER_H
#define X68K_CORE_VIDEO_SPRITE_RASTER_H

#include <cstdint>

#include "../cpu/m68k_types.h"
#include "../dev/sprite.h"
#include "../dev/video.h"

namespace x68k
{

class SpriteRaster
{
public:
    // 透明として扱う色番号。GraphicRaster と同じ規則。
    static constexpr u8 kTransparentIndex = 0;

    // 1 ラインに重ねられるスプライトの上限。
    //
    // 実機の CYNTHIA は 1 ラインあたり 32 個までしか出せず、それを超えた
    // ぶんは表示されない (スプライトが消える / ちらつくのはこの制限)。
    // 上限を設けることは実機の再現であると同時に、1 ライン当たりの
    // 最悪計算量を固定する意味も持つ。
    //
    // 【裏付けの限界】32 という数は IPL-ROM からは確かめられない
    // (ROM はスプライトを 1 つも表示しない)。一般的な資料の値を採った。
    static constexpr u32 kMaxSpritesPerLine = 32;

    // PCG から 1 ドットの色番号 (0-15) を取り出す。
    //
    //   vram    : スプライト VRAM の先頭 (32KB)
    //   pattern : 16x16 パターン番号 (0-127 が PCG 領域に収まる)
    //   x, y    : パターン内の座標 (0-15)
    //
    // 16x16 パターンは 8x8 を 4 つ並べた形で VRAM に載る。並び順は
    // 左上 → 右上 → 左下 → 右下 で、それぞれ 32 バイトずつ。
    //
    // Why not 16x16 を素直に「1 行 8 バイト x 16 行」としないか: 実機の PCG は
    // 8x8 が基本単位で、16x16 はその 4 つ組として定義される。SP_DEFCG
    // ($FFC0C4) が 16x16 を 128 バイトの連続として転送することとは矛盾しない
    // (連続していることと、その中の並びが 8x8 単位であることは両立する)。
    // 素直な 1 行 8 バイトにすると、同じ VRAM を 8x8 パターン 4 つとして
    // 読んだときに絵が食い違う。
    [[nodiscard]] static u8 pcgPixel(const u8* vram, u32 pattern, u32 x, u32 y);

    // 8x8 の PCG から 1 ドットの色番号を取り出す。
    //
    // 8x8 パターン番号は 32 バイト刻み。1 行は 4 バイト (8 ドット x 4bit)。
    [[nodiscard]] static u8 pcgPixel8(const u8* vram, u32 pattern, u32 x, u32 y);

    // スプライト面を out へ重ねる。透明でないドットだけを書く。
    //
    //   sprite   : レジスタと VRAM
    //   video    : パレットを引くために使う
    //   srcX/Y   : 切り出す位置 (スプライト面の座標。画面左上が (0,0))
    //   width    : 切り出す幅
    //   height   : 切り出す高さ
    //   out      : 変換先。nullptr なら何もしない
    //   outStride: out の 1 行あたりの要素数
    //
    // 表示が許可されていない、または表示中のスプライトが 1 つも無いなら
    // 何もせずに返る。
    static void renderSprites(const Sprite& sprite, const VideoController& video, u32 srcX,
                              u32 srcY, u32 width, u32 height, u16* out, u32 outStride);

    // BG 1 面を out へ重ねる。透明でないドットだけを書く。
    //
    //   plane : 0 または 1
    //
    // 表示が許可されていなければ何もしない。
    static void renderBg(const Sprite& sprite, const VideoController& video, u32 plane, u32 srcX,
                         u32 srcY, u32 width, u32 height, u16* out, u32 outStride);

    // スプライト面全体 (BG 2 面 + スプライト) を out へ重ねる。
    //
    // 奥から順に BG1 → BG0 → スプライトの順で描く。
    //
    // Why not スプライトを BG より奥に置けるようにしないか: 実機の
    // スプライトレジスタのプライオリティ (2bit) は「スプライトどうしの
    // 前後」と「BG との前後」を兼ねるとされるが、その対応は IPL-ROM からは
    // 確かめられなかった。BG より手前という最も一般的な並びに固定する。
    // 詳細は sprite_raster.cpp の renderPlane() のコメントを参照。
    static void renderPlane(const Sprite& sprite, const VideoController& video, u32 srcX, u32 srcY,
                            u32 width, u32 height, u16* out, u32 outStride);

    // スプライト面が何か出すものを持っているか。
    //
    // 合成側が「スプライト面を描くかどうか」を決めるのに使う。
    // $E82600 の bit6 と $EB0808 の bit9 が両方立っていて、かつ
    // 実際に表示中のスプライトか BG があるときだけ true。
    [[nodiscard]] static bool hasVisibleContent(const Sprite& sprite, const VideoController& video);
};

}  // namespace x68k

#endif  // X68K_CORE_VIDEO_SPRITE_RASTER_H
