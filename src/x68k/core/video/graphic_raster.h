// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// グラフィック VRAM (G-VRAM) を RGB565 に変換する。
//
// テキスト画面と違い、グラフィック画面はプレーン分割ではなくワード単位の
// パックドピクセルで持つ。実 VRAM は 512KB = 262144 ワードで、
// 512 ライン x 512 ワード (1 ラインあたり 1024 バイト) として並ぶ。
// 色数モードによって 1 ワードの意味が変わる:
//
//   16 色   (4bpp)  : 1 ワードに 4 ページぶんの 4bit が詰まる
//   256 色  (8bpp)  : 1 ワードに 2 ページぶんの 8bit が詰まる
//   65536 色(16bpp) : 1 ワードがそのまま色 (パレットを介さない)
//
// Why not ページごとに VRAM を区切った線形配置にしないか: 実機は
// 「同じ座標のワードを読めば全ページのドットが同時に取れる」構造になっていて、
// 16 色モードで 4 ページを一括クリアする、256 色モードを 16 色 2 ページと
// 同じアドレスで扱う、といった操作がアドレス計算なしに成立する。
// ページごとに 128KB ずつ区切る配置にすると、この重ね合わせが崩れて
// 16 色モードで書いた絵が 256 色モードで別物に見えるようになり、
// 実機のソフト (SX-Window を含む) が期待する見え方と食い違う。
//
// テキスト画面と同じく、変換する矩形を指定できるようにしてある。
// CoreS3 の LCD は 320x240 しかなく、512x512 の全体を毎フレーム変換すると
// PSRAM 帯域を使い切ってしまうため。

#ifndef X68K_CORE_VIDEO_GRAPHIC_RASTER_H
#define X68K_CORE_VIDEO_GRAPHIC_RASTER_H

#include <cstdint>

#include "../cpu/m68k_types.h"
#include "../dev/video.h"
#include "../memmap.h"

namespace x68k
{

// --- G-VRAM の形 -----------------------------------------------------------

// 実 VRAM の 1 ラインは 512 ワード = 1024 バイト。
inline constexpr u32 kGvramBytesPerLine = 1024u;
// 512x512 モードの実画面の大きさ。
inline constexpr u32 kGvramPageWidth = 512u;
inline constexpr u32 kGvramPageHeight = 512u;
// 1024x1024 モード (16 色専用) の実画面の大きさ。
inline constexpr u32 kGvramLargeWidth = 1024u;
inline constexpr u32 kGvramLargeHeight = 1024u;

class GraphicRaster
{
public:
    // 透明として扱うパレット番号。
    //
    // X68000 のグラフィック画面はパレット番号 0 が透明で、背後のプレーンが
    // 透ける。65536 色モードだけは色そのものが並ぶのでこの規則が効かず、
    // 色コード $0000 を透明とみなす。
    static constexpr u8 kTransparentIndex = 0;

    // 1 ドットぶんのパレット番号 (16 色 / 256 色) を取り出す。
    //
    //   vram : G-VRAM の先頭 (512KB)
    //   mode : 色数モード
    //   page : 読むページ。16 色なら 0-3、256 色なら 0-1、65536 色なら無視
    //   x, y : 実画面の座標
    //
    // 65536 色モードでは色コードの下位 8bit を返す。番号として意味を持たない
    // ので、この関数ではなく pixelColor() を使うこと。
    [[nodiscard]] static u8 pixelIndex(const u8* vram, VideoController::GraphicColorMode mode,
                                       u32 page, u32 x, u32 y);

    // 1 ドットぶんの X68000 形式の色 (GGGGGRRRRRBBBBBI) を取り出す。
    //
    // 16 色 / 256 色モードはパレットを引いた結果を、65536 色モードは
    // VRAM のワードをそのまま返す。透明かどうかは呼ぶ側では判定できないので、
    // 透明を区別したい場合は pixelIndex() を併用する。
    [[nodiscard]] static u16 pixelColor(const u8* vram, const VideoController& video, u32 page,
                                        u32 x, u32 y);

    // 1024x1024 モード (16 色) の 1 ドットぶんのパレット番号を取り出す。
    //
    // 4 ページを 2x2 に並べて 1 枚の大きな画面として扱う。
    [[nodiscard]] static u8 pixelIndexLarge(const u8* vram, u32 x, u32 y);

    // グラフィック画面の指定矩形を RGB565 に変換して out へ書く。
    //
    //   vram     : G-VRAM の先頭。nullptr なら何もしない
    //   video    : パレットと画面モードを引くために使う
    //   srcX/Y   : 切り出す位置 (実画面の座標)
    //   width    : 切り出す幅 (ピクセル)
    //   height   : 切り出す高さ (ピクセル)
    //   out      : 変換先。width * height 個の u16 が必要
    //   outStride: out の 1 行あたりの要素数 (width と違う場合に指定)
    //
    // 透明ドットには何も書かない (out の元の内容が残る)。背後のプレーンを
    // 先に描いてからこれを呼べば重ね合わせになる。
    static void render(const u8* vram, const VideoController& video, u32 srcX, u32 srcY, u32 width,
                       u32 height, u16* out, u32 outStride);

    // テキスト画面とグラフィック画面を優先順位に従って合成する。
    //
    //   graphicVram : G-VRAM の先頭。nullptr ならグラフィック面は無いものとして扱う
    //   textVram    : テキスト VRAM の先頭。nullptr ならテキスト面は無いものとして扱う
    //
    // $E82500 のプライオリティと $E82600 の表示許可を見て、奥から順に描く。
    // 透明ドット (パレット番号 0) は背後が透ける。どちらの面も出ない位置は
    // 黒 (RGB565 の 0) になる。
    static void composite(const u8* graphicVram, const u8* textVram, const VideoController& video,
                          u32 srcX, u32 srcY, u32 width, u32 height, u16* out, u32 outStride);

private:
    // テキスト画面を透明を考慮して重ねる。
    //
    // Why not TextRaster::render() をそのまま使わないか: あちらはパレット番号 0
    // にも色を書く。テキスト画面だけを出すなら正しいが、重ね合わせでは
    // 背後のグラフィック面を塗り潰してしまう。X68000 のテキスト画面は
    // パレット番号 0 が透明なので、重ねるときは書かずに飛ばす必要がある。
    // TextRaster 側を透明対応に変えると、テキスト単独表示の背景が
    // 塗られなくなって既存の表示経路が壊れるため、ここに別経路を置く。
    static void renderTextOver(const u8* vram, const VideoController& video, u32 srcX, u32 srcY,
                               u32 width, u32 height, u16* out, u32 outStride);
};

}  // namespace x68k

#endif  // X68K_CORE_VIDEO_GRAPHIC_RASTER_H
