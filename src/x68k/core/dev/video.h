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
    // 上位バイトが「面」の並び、下位バイトが「グラフィックの 4 ページ」の並び。
    //
    //   bit13-12: スプライト面 (SP)
    //   bit11-10: テキスト面   (TX)
    //   bit9-8  : グラフィック面 (GR)
    //   bit7-6 / bit5-4 / bit3-2 / bit1-0: GP3 / GP2 / GP1 / GP0
    //
    // どちらも 2bit で、値が小さいほど手前。GP3-GP0 は「ページ n を何番目に
    // 置くか」ではなく「n 番目の位置にどのページを置くか」でもなく、
    // ページ n 自身の順位を GPn が持つ形 (下の graphicPagePriority() 参照)。
    //
    // 根拠: IPL-ROM の起動処理が $FF6436 で `move.w #$06E4, $E82500` を書く
    // (生ワード列 $FF642E: 33fc 0020 00e8 2600 33fc 06e4 00e8 2500)。
    // $06E4 = 0000_01_10_11_10_01_00 なので SP=0, TX=1, GR=2、
    // GP3=3, GP2=2, GP1=1, GP0=0 となり、面はスプライト→テキスト→グラフィック、
    // ページは 0,1,2,3 の順という実機の既定値にちょうど一致する。
    //
    // Why not 低位 6bit を SP/TX/GR とする読み方にしないか: その読み方でも
    // $06E4 からは偶然 SP=0/TX=1/GR=2 が得られてしまう ($E4 の低 6bit が
    // 100100b で同じ並びになるため) が、それは GP フィールドを面の順位と
    // 誤読しているだけで、GP3-GP0 が消える。SX-Window は 16 色 4 ページを
    // 個別に重ねてウィンドウを描くので、ページの前後関係を失うと
    // 表示順がページ番号順に固定され、実機と違う絵になる。
    [[nodiscard]] u16 priority() const
    {
        return priority_;
    }

    [[nodiscard]] u8 spritePriority() const
    {
        return static_cast<u8>((priority_ >> 12) & 0x03u);
    }

    [[nodiscard]] u8 textPriority() const
    {
        return static_cast<u8>((priority_ >> 10) & 0x03u);
    }

    [[nodiscard]] u8 graphicPriority() const
    {
        return static_cast<u8>((priority_ >> 8) & 0x03u);
    }

    // グラフィックページ page (0-3) の、4 ページ内での表示順位。
    // 値が小さいほど手前。GP0 が bit1-0、GP3 が bit7-6。
    [[nodiscard]] u8 graphicPagePriority(u32 page) const
    {
        if (page >= 4)
        {
            return 0;
        }
        return static_cast<u8>((priority_ >> (page * 2u)) & 0x03u);
    }

    // --- $E82600: 表示制御 (R2) ---
    //
    // bit5 がテキスト画面、bit6 がスプライト画面の表示許可。
    // グラフィックは bit4 (GS4) と bit3-0 (GS3-GS0) の 2 段構えで、
    // GS4 は 1024x1024 のとき、GS3-GS0 は 512x512 のときに効く。
    [[nodiscard]] u16 displayControl() const
    {
        return displayControl_;
    }

    // グラフィック画面全体が表示されるか。
    //
    // 1024x1024 モードでは GS4 (bit4) が唯一の表示許可。512x512 モードでは
    // GS3-GS0 (bit3-0) がページ単位の許可を兼ねていて、1 つでも立っていれば
    // グラフィック面は出る。
    //
    // 根拠: IPL-ROM の IOCS グラフィックページ表示制御 ($FFB2C8 付近) は
    // ページ指定 d1 (bit3-0) から $E82600 を組み立てる際、
    //   $FFB2D2: tst.b d1 / beq → 非 0 なら or.b #$10,d1   (GS4 を足す)
    //   $FFB2DA: move.w $E82600,d0 / and.w #$FFE0,d0 / or.w d1,d0 / 書き戻し
    // という手順を踏む (生ワード列 $FFB2D2: 4a01 6704 823c 0010 3039 00e8
    // 2600 c07c ffe0 8041 33c0 00e8)。マスクが $FFE0 = 下位 5bit をまとめて
    // 差し替える形であること、そして GS4 が「ページが 1 つでも有効なら
    // 立てる」従属ビットとして扱われていることから、512x512 では GS3-GS0 が
    // 実体で GS4 は連動する印にすぎないと読める。
    // さらに $FFB2F2 以降は色数で GS3-GS0 の意味が変わることを示す:
    // 2 ページ (256 色) では指定 bit1 → $0C (bit3-2)、bit0 → $03 (bit1-0) へ
    // 展開し、1 ページ (65536 色) では $FFB30C で moveq #$F,d1 と 4bit すべてを
    // 立てる。ページ数は $93D にあり、CRTC モードテーブル ($FF65E0、1 件 32
    // バイト) の該当ワードが 16 色=4 / 256 色=2 / 65536 色・1024x1024=1 を持つ。
    //
    // Why not 常に bit4 だけを見ないか: 512x512 でページビットだけを立てる
    // プログラム (IOCS を通さず $E82600 を直接書くものは珍しくない) が
    // グラフィック面ごと消える。ROM 自身も $FF6466 で `and.w #$1F,d0` と
    // 下位 5bit をまとめて「グラフィックが出ているか」の判定に使っており
    // (生ワード列 $FF6454: 3f39 00e8 2600 33fc 0000 00e8 2600 6128 3017 c07c
    // 001f 6704)、bit4 単独では判定していない。
    [[nodiscard]] bool graphicEnabled() const
    {
        if (isGraphic1024())
        {
            return (displayControl_ & 0x0010u) != 0;
        }
        return (displayControl_ & 0x000Fu) != 0;
    }

    [[nodiscard]] bool textEnabled() const
    {
        return (displayControl_ & 0x0020u) != 0;
    }

    [[nodiscard]] bool spriteEnabled() const
    {
        return (displayControl_ & 0x0040u) != 0;
    }

    // 512x512 モードでページ page が表示対象か。
    //
    // GS3-GS0 は色数によって「1 ページあたり何ビット」かが変わる。
    // 16 色は 4 ページで 1bit ずつ、256 色は 2 ページで 2bit ずつ
    // (ページ 0 が bit1-0、ページ 1 が bit3-2)、65536 色は 1 ページで 4bit
    // すべて。ROM が $FFB2F2 以降で行う展開 (bit1 → $0C、bit0 → $03、
    // 65536 色なら moveq #$F) の逆変換にあたる。
    //
    // Why not 常に 1bit = 1 ページとして読まないか: 256 色モードで
    // ページ 1 を表示するプログラムは IOCS 経由だと $0C が書かれる。
    // 1bit = 1 ページで読むと「ページ 2 と 3 が有効」に化けて、
    // 存在しないページを描こうとする。
    [[nodiscard]] bool graphicPageEnabled(u32 page) const
    {
        switch (graphicColorMode())
        {
            case GraphicColorMode::k16Color:
                return page < 4 && (displayControl_ & (1u << page)) != 0;

            case GraphicColorMode::k256Color:
                return page < 2 && (displayControl_ & (0x03u << (page * 2u))) != 0;

            case GraphicColorMode::kReserved:
            case GraphicColorMode::k65536Color:
            default:
                return page == 0 && (displayControl_ & 0x000Fu) != 0;
        }
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
