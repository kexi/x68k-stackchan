// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// CRTC ($E80000) とビデオコントローラ ($E82000)。
//
// CRTC は画面のタイミングを決めるチップで、垂直帰線のタイミングもここが源。
// ビデオコントローラはパレットと画面モード、表示プライオリティを持つ。
//
// 実装範囲: レジスタの保持と、垂直/水平同期のタイミング生成、
// テキストパレット 16 色。ラスタコピーや高速クリアといった加速機能は
// Human68k のコンソール表示には不要なので後回しにする。

#ifndef X68K_CORE_DEV_VIDEO_H
#define X68K_CORE_DEV_VIDEO_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Crtc
{
public:
    // CRTC のレジスタは R00-R23。ワード単位で並ぶ。
    static constexpr u32 kRegCount = 24;

    // 1 フレームあたりの CPU サイクル数。
    //
    // X68000 の高解像度モードは 55.45Hz。CPU 10MHz なので
    // 10,000,000 / 55.45 ≒ 180,342 サイクル。
    // 垂直帰線のタイミングだけ合っていればよいので、この概算で足りる。
    static constexpr u32 kCyclesPerFrame = 180342;
    // 垂直帰線期間。全体の約 1 割。
    static constexpr u32 kVBlankCycles = 18000;

    void reset();

    [[nodiscard]] u16 read(u32 regIndex) const;
    void write(u32 regIndex, u16 value);

    // CPU サイクルぶん時間を進める。戻り値は垂直帰線の状態が変化したかどうか。
    bool tick(u32 cycles);

    [[nodiscard]] bool inVerticalBlank() const
    {
        return inVBlank_;
    }

    // 現在のラスタ番号。ラスタ割り込みや $E80028 の読み出しに使う。
    [[nodiscard]] u32 rasterNumber() const;

private:
    std::array<u16, kRegCount> reg_{};
    u32 frameCycles_ = 0;
    bool inVBlank_ = false;
};

// ビデオコントローラ (VIPS/CATHY)。パレットと画面制御。
class VideoController
{
public:
    // テキスト/スプライト用パレットは 16 色。$E82200 から。
    static constexpr u32 kTextPaletteCount = 16;
    // グラフィック用パレットは 256 色。$E82000 から。
    static constexpr u32 kGraphicPaletteCount = 256;

    void reset();

    [[nodiscard]] u16 read(u32 addr) const;
    void write(u32 addr, u16 value);

    // テキストパレットの色を X68000 形式 (GGGGGRRRRRBBBBBI) で返す。
    [[nodiscard]] u16 textPalette(u32 index) const
    {
        return index < kTextPaletteCount ? textPalette_[index] : 0u;
    }

    // X68000 の色形式を RGB565 に変換する。
    //
    // X68000: G5 R5 B5 I1 (上位から緑・赤・青・輝度)
    // RGB565: R5 G6 B5
    // 輝度ビットは各色の最下位ビットとして扱う。
    [[nodiscard]] static u16 toRgb565(u16 x68kColor);

private:
    std::array<u16, kTextPaletteCount> textPalette_{};
    std::array<u16, kGraphicPaletteCount> graphicPalette_{};
    // R0: 画面モード、R1: プライオリティ、R2: 表示制御。
    u16 screenMode_ = 0;
    u16 priority_ = 0;
    u16 displayControl_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_VIDEO_H
