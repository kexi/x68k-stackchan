// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// テキスト VRAM を RGB565 に変換する。
//
// X68000 のテキスト画面は 4 プレーン × 1bit/dot のビットマップで、
// 4 枚を重ねた 4bit がパレット番号になる。キャラクタ VRAM ではないので、
// 「どの文字が書かれているか」は VRAM からは分からない。
//
// 変換の範囲を指定できるようにしてあるのは、CoreS3 の LCD が 320x240 しかなく
// Human68k のコンソール (768x512) の一部だけを切り出して表示するため。
// 画面全体を毎フレーム変換すると PSRAM 帯域を使い切ってしまう。

#ifndef X68K_CORE_VIDEO_TEXT_RASTER_H
#define X68K_CORE_VIDEO_TEXT_RASTER_H

#include <cstdint>

#include "../cpu/m68k_types.h"
#include "../dev/video.h"
#include "../memmap.h"

namespace x68k
{

class TextRaster
{
public:
    // テキスト VRAM の指定矩形を RGB565 に変換して out へ書く。
    //
    //   vram    : テキスト VRAM の先頭 (4 プレーンが kTvramPlaneSize 間隔で並ぶ)
    //   video   : パレットを引くために使う
    //   srcX/Y  : 切り出す位置 (X68000 の画面座標)
    //   width   : 切り出す幅 (ピクセル)
    //   height  : 切り出す高さ (ピクセル)
    //   out     : 変換先。width * height 個の u16 が必要
    //   outStride: out の 1 行あたりの要素数 (width と違う場合に指定)
    static void render(const u8* vram, const VideoController& video, u32 srcX, u32 srcY, u32 width,
                       u32 height, u16* out, u32 outStride);

    // 1 ピクセルぶんのパレット番号を取り出す。
    //
    // 4 プレーンの同じビット位置を集めて 4bit にする。プレーンは
    // kTvramPlaneSize (128KB) 間隔で並んでいる。
    [[nodiscard]] static u8 pixelIndex(const u8* vram, u32 x, u32 y);
};

}  // namespace x68k

#endif  // X68K_CORE_VIDEO_TEXT_RASTER_H
