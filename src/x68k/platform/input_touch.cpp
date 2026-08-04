// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "input_touch.h"

#include <M5Unified.h>

namespace x68k_platform
{
namespace
{

// キーボードの配列。Human68k のコマンドを打つのに要る範囲。
// 4 行 x 10 列をすべて埋めてあり、「キー無し」のマスは無い。
//
// 4 行目の ' ' は空きマスではなく Space キーそのもの ("SP" と描く)。
// 最後の '\n' が Enter ("RET")。BS は入っていないので、打ち間違えたら
// Enter で確定してやり直す。
//
// Why not BS を入れないか: 10 列に収める都合で 1 つ落とす必要があり、
// `dir` を打つのに要らない BS を落とした。列を増やすとキーが細くなって
// 指で押し分けられない (320px / 10 で 1 キー 32px)。
constexpr char kLayout[TouchKeyboard::kKeyRows][TouchKeyboard::kKeyCols + 1] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm /\n",
};

// 表示上のラベル。改行とスペースは記号で示す。
const char* keyLabel(char c)
{
    switch (c)
    {
        case '\n':
            return "RET";
        case ' ':
            return "SP";
        default:
            return nullptr;  // 文字そのものを描く
    }
}

constexpr int kKeyWidth = 320 / TouchKeyboard::kKeyCols;
constexpr int kKeyHeight = (240 - TouchKeyboard::kKeyboardTop) / TouchKeyboard::kKeyRows;

}  // namespace

void TouchKeyboard::begin()
{
    lastKeyIndex_ = -1;
    draw();
}

void TouchKeyboard::setVisible(bool visible)
{
    visible_ = visible;
    if (visible_)
    {
        draw();
    }
}

void TouchKeyboard::setX68kInputEnabled(bool enabled)
{
    if (isX68kInputEnabled_ == enabled)
    {
        return;
    }
    isX68kInputEnabled_ = enabled;

    // 触っている途中の状態を捨てる。
    //
    // Why: 押したまま切り替えると、isDragging_ と lastTouchX/Y_ が
    // 切り替え前の座標を指したまま残る。戻ってきて最初に触った位置との
    // 差がそのまま移動量になり、カーソルが画面の端まで飛ぶ
    // (「触り始めは基準点を置くだけ」で防いでいるのと同じ問題が、
    //  モードをまたいで再現する)。
    //
    // lastKeyIndex_ も戻す。押しっぱなしのキーを覚えたまま顔へ抜けると、
    // 戻って同じキーを押したときに「まだ離していない」と見なして無視する。
    isDragging_ = false;
    lastKeyIndex_ = -1;
}

void TouchKeyboard::draw()
{
    if (!visible_)
    {
        return;
    }

    for (int row = 0; row < kKeyRows; ++row)
    {
        for (int col = 0; col < kKeyCols; ++col)
        {
            const char c = kLayout[row][col];
            const int x = col * kKeyWidth;
            const int y = kKeyboardTop + row * kKeyHeight;

            M5.Display.drawRect(x, y, kKeyWidth, kKeyHeight, TFT_DARKGREY);
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
            M5.Display.setTextSize(1);

            const char* label = keyLabel(c);
            if (label != nullptr)
            {
                M5.Display.setCursor(x + 4, y + kKeyHeight / 2 - 4);
                M5.Display.print(label);
            }
            else
            {
                M5.Display.setCursor(x + kKeyWidth / 2 - 3, y + kKeyHeight / 2 - 4);
                M5.Display.print(c);
            }
        }
    }
}

void TouchKeyboard::poll(KeyQueue& keys, MouseQueue& mouse)
{
    // 顔モードの間は X68000 へ何も送らない。
    //
    // Why not 何も見ずに返るか: 見ずに返っても状態は壊れない。
    // setX68kInputEnabled が切り替えの時点で isDragging_ と
    // lastKeyIndex_ を捨てるので、顔モードの間に指を離したことを
    // 覚えておく必要が無い。ここで touch を読まないぶん、顔モードでは
    // M5.Touch の読み出しも省ける。
    if (!isX68kInputEnabled_)
    {
        return;
    }

    const auto touch = M5.Touch.getDetail();
    if (!touch.isPressed())
    {
        // 離したので次のキーを受け付ける。
        lastKeyIndex_ = -1;

        // ボタンを離したことを伝える。
        //
        // Why not 何もしないか: 押したまま指を離すと、ゲストから見ると
        // ボタンが押しっぱなしになる。SX-Window ではドラッグが終わらず、
        // ウィンドウが指に貼り付いたままになる。
        if (isDragging_)
        {
            isDragging_ = false;
            mouse.push(0, 0, false, false);
        }
        return;
    }

    // キーボードを出していないときは画面全体をマウスに使う。
    //
    // Why not 出していない間も下部をキーボード扱いにするか: 描かれていない
    // キーを押せることになる。見えないものが反応する状態は、意図しない
    // 文字が入るだけで害しかない (main.cpp が setVisible(false) している
    // 理由もこれ)。
    const bool isMouseArea = !visible_ || touch.y < kKeyboardTop;
    if (isMouseArea)
    {
        // 触り始めは基準点を置くだけ。前に離した位置との差を送ると飛ぶ。
        if (!isDragging_)
        {
            isDragging_ = true;
            lastTouchX_ = touch.x;
            lastTouchY_ = touch.y;
            // 押下だけを伝える。移動量は次のループから。
            mouse.push(0, 0, true, false);
            return;
        }

        const int dx = touch.x - lastTouchX_;
        const int dy = touch.y - lastTouchY_;
        lastTouchX_ = touch.x;
        lastTouchY_ = touch.y;

        // 動いていなくてもボタンの状態は保つ。MouseQueue が
        // 「変化が無ければ送らない」を判断する。
        mouse.push(dx, dy, true, false);
        return;
    }

    const int col = touch.x / kKeyWidth;
    const int row = (touch.y - kKeyboardTop) / kKeyHeight;
    if (col < 0 || col >= kKeyCols || row < 0 || row >= kKeyRows)
    {
        return;
    }

    const int index = row * kKeyCols + col;
    if (index == lastKeyIndex_)
    {
        return;  // 押しっぱなし。連打にしない。
    }
    lastKeyIndex_ = index;

    // 押下と解放に分ける仕事は KeyQueue が持つ。ここは打たれた文字を
    // 積むだけ。
    keys.push(kLayout[row][col]);
}

}  // namespace x68k_platform
