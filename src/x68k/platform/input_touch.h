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

#include "key_queue.h"
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

    // タッチを読んで、押されたキーがあれば queue へ積む。毎ループ呼ぶ。
    //
    // Why not Machine へ直接送るか: Machine の状態はエミュレーション
    // コアが所有する。表示コアから触るとデータ競合になるうえ、MFP の
    // 受信レジスタは 1 バイトしか保持しないので、押下と解放を続けて
    // 書くと入力が消える。queue に積んでおけば、エミュレーションコアが
    // 自分のペースで間隔を空けて送れる。
    void poll(KeyQueue& keys);

    // キーボードを描画する。表示が上書きされた後に呼び直す。
    void draw();

    // キーボードを表示するか。false のときは poll してもキーを送らない。
    void setVisible(bool visible);

    [[nodiscard]] bool isVisible() const
    {
        return visible_;
    }

private:
    // 押しっぱなしで連打にならないよう、離すまで次を送らない。
    int lastKeyIndex_ = -1;
    bool visible_ = true;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_INPUT_TOUCH_H
