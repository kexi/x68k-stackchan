// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// タッチパネルの仮想キーボード。
//
// CoreS3 には物理キーボードが無いので、画面下部にキーを並べて打てるようにする。
// Human68k のプロンプトで `dir` と打てれば PoC の目的は達せられるので、
// 英数字と記号、Enter / BS / Space があれば足りる。
//
// スキャンコードについて:
//   X68000 のキーボードは MFP のシリアルへスキャンコードを送る。押下で
//   コードそのもの、離すとコード | $80 が届く。ASCII ではないので変換表が要る。

#ifndef X68K_PLATFORM_INPUT_TOUCH_H
#define X68K_PLATFORM_INPUT_TOUCH_H

#include <cstdint>

#include "machine.h"

namespace x68k_platform
{

class TouchKeyboard
{
public:
    // キーボードの表示領域。画面下部に置く。
    static constexpr int kKeyboardTop = 150;
    static constexpr int kKeyRows = 4;
    static constexpr int kKeyCols = 10;

    void begin();

    // タッチを読んで、押されたキーがあれば machine へ送る。
    // 毎ループ呼ぶ。
    void poll(x68k::Machine& machine);

    // キーボードを描画する。表示が上書きされた後に呼び直す。
    void draw();

    // キーボードを表示するか。false のときは poll してもキーを送らない。
    void setVisible(bool visible);

    [[nodiscard]] bool isVisible() const
    {
        return visible_;
    }

    // ASCII 文字を X68000 のスキャンコードへ変換する。
    // 対応しない文字は 0 を返す。
    [[nodiscard]] static x68k::u8 asciiToScanCode(char ascii);

private:
    // 押しっぱなしで連打にならないよう、離すまで次を送らない。
    int lastKeyIndex_ = -1;
    bool visible_ = true;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_INPUT_TOUCH_H
