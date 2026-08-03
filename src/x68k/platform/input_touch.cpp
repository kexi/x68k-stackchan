// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "input_touch.h"

#include <M5Unified.h>

namespace x68k_platform
{
namespace
{

// キーボードの配列。Human68k のコマンドを打つのに要る範囲。
// 空白は「キー無し」。
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

void TouchKeyboard::poll(KeyQueue& keys)
{
    if (!visible_)
    {
        return;
    }

    const auto touch = M5.Touch.getDetail();
    if (!touch.isPressed())
    {
        // 離したので次のキーを受け付ける。
        lastKeyIndex_ = -1;
        return;
    }

    if (touch.y < kKeyboardTop)
    {
        return;  // 画面側のタッチ
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
