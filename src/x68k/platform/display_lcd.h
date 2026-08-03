// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// CoreS3 の LCD へテキスト画面を出す。
//
// Human68k の標準コンソールは 768x512 だが CoreS3 の LCD は 320x240 しかない。
// そのまま縮小すると 8x16 のフォントが 3.3x7.5 ドットになって読めないので、
// 等倍で一部を切り出し、カーソルを追うようにスクロールさせる。
//
// 役割を 2 つに分けてある:
//   renderTo()  — テキスト VRAM を RGB565 へ変換する。Machine を読むので
//                 エミュレーションコア (Core1) から呼ぶ。
//   pushFrame() — できあがった RGB565 を LCD へ送る。Machine を触らないので
//                 表示コア (Core0) から呼べる。
//
// なぜ分けるか: Machine の状態を両コアから触るとデータ競合になる。
// 変換を Core1 に寄せ、Core0 へは完成したフレームだけを渡す。

#ifndef X68K_PLATFORM_DISPLAY_LCD_H
#define X68K_PLATFORM_DISPLAY_LCD_H

#include <cstdint>

#include "machine.h"

namespace x68k_platform
{

class DisplayLcd
{
public:
    // CoreS3 の LCD の大きさ。
    static constexpr x68k::u32 kScreenWidth = 320;
    static constexpr x68k::u32 kScreenHeight = 240;

    // LCD を初期化する。表示コアから 1 度だけ呼ぶ。
    void begin();

    // 表示位置を設定する。768x512 の中のどこを映すか。
    void setViewport(x68k::u32 x, x68k::u32 y);

    // 拡大率の範囲。
    //
    // 呼ぶ側が「これ以上は変わらない」を判断できるよう公開する。
    // setZoom 側だけがクランプを持つと、呼ぶ側は上限を超えた値を
    // 持ち続けてしまい、戻すときに打った回数どおりに動かない。
    static constexpr x68k::u32 kMinZoom = 1;
    static constexpr x68k::u32 kMaxZoom = 4;

    // 拡大率。1 = 等倍 (40 桁 x 15 行)、2 = 2 倍 (20 桁 x 7 行)。
    //
    // 等倍は 1 文字が 8x16 ドットのまま 2 インチの画面に出るので、
    // 桁数は稼げるが実際には読み取れない。2 倍にすると 16x32 になり、
    // 見える範囲は狭まるが文字として判別できる。
    void setZoom(x68k::u32 zoom);

    [[nodiscard]] x68k::u32 zoom() const
    {
        return zoom_;
    }

    [[nodiscard]] x68k::u32 viewportX() const
    {
        return viewX_;
    }
    [[nodiscard]] x68k::u32 viewportY() const
    {
        return viewY_;
    }

    // --- エミュレーションコア (Core1) から呼ぶ ---

    // テキスト VRAM を RGB565 へ変換して out へ書く。
    //
    // 変化が無く、全体の描き直しも要らなければ false を返して何もしない。
    // その場合は転送も要らない。
    bool renderTo(x68k::Machine& machine, const x68k::u8* textVram, x68k::u16* out);

    // 次のフレームで全体を描き直させる。表示位置を変えた後などに呼ぶ。
    void invalidateAll()
    {
        forceFullRedraw_ = true;
    }

    // --- 表示コア (Core0) から呼ぶ ---

    // できあがった RGB565 を LCD へ送る。
    void pushFrame(const x68k::u16* frame);

    // 起動時のメッセージを出す。ROM が見つからないときなどに使う。
    static void showMessage(const char* line1, const char* line2 = nullptr);

private:
    // 拡大表示。全画面を作り直す。
    void renderZoomed(x68k::Machine& machine, const x68k::u8* textVram, x68k::u16* out);

    x68k::u32 viewX_ = 0;
    x68k::u32 viewY_ = 0;
    x68k::u32 zoom_ = 1;
    bool forceFullRedraw_ = true;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_DISPLAY_LCD_H
