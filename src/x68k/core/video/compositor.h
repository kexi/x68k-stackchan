// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 画面に出る面をすべて 1 枚の RGB565 バッファへ重ねる、唯一の入口。
//
// X68000 の画面は G-VRAM (グラフィック)・テキスト・BG 2 面・スプライトから
// 成るが、GraphicRaster::composite() が見るのは前 2 つだけで、BG と
// スプライトは SpriteRaster が別に持っている。両方を呼ばないと
// スプライトを使うソフトの絵は出ない。
//
// Why not それぞれの呼び出し側で 2 つを順に呼ぶか: ホスト (host/main.cpp) と
// 実機 (platform/display_lcd.cpp) の 2 か所に同じ重ね順が複製され、
// 一方だけ直したときに「ホストでは出るが実機では出ない」がすぐ生まれる。
// 実際、この関数を置くまで両方ともスプライトを描いておらず、
// SpriteRaster::renderPlane() は実装もテストもあるのに呼ばれていなかった。
// 重ね順を知る場所を 1 つに閉じ込め、両方がここを通るようにする。
//
// Why not GraphicRaster::composite() にスプライトを足すか: あちらは
// G-VRAM とテキストという「VideoController だけで完結する面」を扱う。
// スプライトと BG は Sprite (CYNTHIA) が持つ別のデバイスなので、
// GraphicRaster に Sprite を持ち込むと依存が増える。重ねる責務は
// どちらにも属さないので、その上に薄い層を置く。

#ifndef X68K_CORE_VIDEO_COMPOSITOR_H
#define X68K_CORE_VIDEO_COMPOSITOR_H

#include "../cpu/m68k_types.h"
#include "../dev/sprite.h"
#include "../dev/video.h"

namespace x68k
{

class Compositor
{
public:
    // 表示中の全プレーンを out へ重ねる。
    //
    //   graphicVram : G-VRAM の先頭。nullptr ならグラフィック面は無いものとして扱う
    //   textVram    : テキスト VRAM の先頭。nullptr ならテキスト面は無いものとして扱う
    //   sprite      : スプライトと BG を持つデバイス。nullptr なら両面とも無いものとして扱う
    //   srcX/Y      : 切り出す位置 (実画面の座標)
    //   width       : 切り出す幅 (ピクセル)
    //   height      : 切り出す高さ (ピクセル)
    //   out         : 変換先
    //   outStride   : out の 1 行あたりの要素数
    //
    // 重ね順は奥から グラフィック/テキスト (両者の前後は $E82500 が決める)
    // → BG1 → BG0 → スプライト。
    //
    // Why not スプライト面も $E82500 のプライオリティで前後を決めるか:
    // レジスタにスプライト面のプライオリティ (bit13-12) は確かにあるが、
    // IPL-ROM はスプライトも BG も一切表示しないので、この 2bit が
    // 実機でどう効くかを ROM から確かめる手立てが無い。確かめられない
    // 規則を実装すると「テストは通るが実機と違う」を作り込むことになる。
    // SpriteRaster::renderPlane() が既に「スプライトは BG より手前」で
    // 固定しているのと同じ理由で、ここでは最も一般的な
    // 「スプライト面が最前面」に固定する。実機で確かめられたら見直す。
    static void render(const u8* graphicVram, const u8* textVram, const Sprite* sprite,
                       const VideoController& video, u32 srcX, u32 srcY, u32 width, u32 height,
                       u16* out, u32 outStride, const Crtc* crtc = nullptr);
};

}  // namespace x68k

#endif  // X68K_CORE_VIDEO_COMPOSITOR_H
