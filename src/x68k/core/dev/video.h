// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// CRTC ($E80000) とビデオコントローラ ($E82000)。
//
// CRTC は画面のタイミングを決めるチップで、垂直帰線のタイミングもここが源。
// ビデオコントローラはパレットと画面モード、表示プライオリティを持つ。
//
// 実装範囲: レジスタの保持と、垂直/水平同期のタイミング生成、
// テキストパレット 16 色とグラフィックパレット 256 色、および
// 画面モード / プライオリティ / 表示制御レジスタの解釈。
// ラスタコピーや高速クリアといった加速機能は Human68k のコンソール表示には
// 不要なので後回しにする。
//
// レジスタの中身をビット単位で解釈するアクセサをここに置くのは、
// $E82400 / $E82500 / $E82600 のビット割り当てを知る場所を 1 箇所に
// 閉じ込めるため。ラスタ側が生の u16 をシフトして読むと、同じビット定義が
// 描画コードへ散らばる。

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
    // フレーム内の位置を進める。垂直帰線に入った / 出たなら true。
    //
    // 命令単位で呼ぶ前提。1 フレーム (kCyclesPerFrame) を超える値を渡しても
    // 位置は範囲に収まるが、その間に通過した垂直帰線の開始と終了は
    // 報告しない (最終状態が呼ぶ前と同じなら false になる)。
    // まとめて進める呼び方をするなら、境界ごとに刻むか、通過したエッジを
    // 返せる形へ作り直す必要がある。
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

    // グラフィック画面の色数モード ($E82400 の bit1-0)。
    //
    // 実機の R0 は bit1-0 が色数、bit2 が実画面サイズ (0=512x512,
    // 1=1024x1024) を表す。1024x1024 は 16 色専用で、4 ページぶんの
    // VRAM を 1 枚の大きな画面として使う。
    enum class GraphicColorMode : u8
    {
        k16Color = 0,     // 4bpp。512x512 が 4 ページ、または 1024x1024 が 1 ページ
        k256Color = 1,    // 8bpp。512x512 が 2 ページ
        kReserved = 2,    // 未定義。実機では 65536 色と同じ挙動をする
        k65536Color = 3,  // 16bpp。512x512 が 1 ページ。パレットを介さない
    };

    void reset();

    [[nodiscard]] u16 read(u32 addr) const;
    void write(u32 addr, u16 value);

    // テキストパレットの色を X68000 形式 (GGGGGRRRRRBBBBBI) で返す。
    [[nodiscard]] u16 textPalette(u32 index) const
    {
        return index < kTextPaletteCount ? textPalette_[index] : 0u;
    }

    // グラフィックパレットの色を X68000 形式 (GGGGGRRRRRBBBBBI) で返す。
    [[nodiscard]] u16 graphicPalette(u32 index) const
    {
        return index < kGraphicPaletteCount ? graphicPalette_[index] : 0u;
    }

    // --- $E82400: 画面モード (R0) ---

    [[nodiscard]] u16 screenMode() const
    {
        return screenMode_;
    }

    [[nodiscard]] GraphicColorMode graphicColorMode() const
    {
        return static_cast<GraphicColorMode>(screenMode_ & 0x03u);
    }

    // 実画面が 1024x1024 か (16 色モードのときだけ意味を持つ)。
    [[nodiscard]] bool isGraphic1024() const
    {
        return (screenMode_ & 0x04u) != 0;
    }

    // --- $E82500: プライオリティ (R1) ---
    //
    // bit1-0 がスプライト、bit3-2 がテキスト、bit5-4 がグラフィックの
    // 表示順位。値が小さいほど手前。同値のときの実機の挙動は
    // スプライト > テキスト > グラフィックの順。
    [[nodiscard]] u16 priority() const
    {
        return priority_;
    }

    [[nodiscard]] u8 spritePriority() const
    {
        return static_cast<u8>(priority_ & 0x03u);
    }

    [[nodiscard]] u8 textPriority() const
    {
        return static_cast<u8>((priority_ >> 2) & 0x03u);
    }

    [[nodiscard]] u8 graphicPriority() const
    {
        return static_cast<u8>((priority_ >> 4) & 0x03u);
    }

    // --- $E82600: 表示制御 (R2) ---
    //
    // bit4 がグラフィック画面、bit5 がテキスト画面、bit6 がスプライト画面の
    // 表示許可。bit3-0 は 16 色モードでのページ単位の表示許可
    // (bit0 がページ 0 …… bit3 がページ 3)。
    [[nodiscard]] u16 displayControl() const
    {
        return displayControl_;
    }

    [[nodiscard]] bool graphicEnabled() const
    {
        return (displayControl_ & 0x0010u) != 0;
    }

    [[nodiscard]] bool textEnabled() const
    {
        return (displayControl_ & 0x0020u) != 0;
    }

    [[nodiscard]] bool spriteEnabled() const
    {
        return (displayControl_ & 0x0040u) != 0;
    }

    // 16 色モードでページ page が表示対象か。
    //
    // Why not 256 色 / 65536 色でもこのビットを見ないか: bit3-0 は
    // 4 プレーン構成を前提にしたビットで、実機でも 16 色モード以外では
    // ページ選択に使われない。256 色モードのページ選択は
    // 「どちらのページを描くか」をラスタ側が引数で受け取る形にしてある。
    [[nodiscard]] bool graphicPageEnabled(u32 page) const
    {
        return page < 4 && (displayControl_ & (1u << page)) != 0;
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
