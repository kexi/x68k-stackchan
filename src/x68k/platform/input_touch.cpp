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

void TouchKeyboard::poll(x68k::Machine& machine)
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

    const char c = kLayout[row][col];
    const x68k::u8 code = asciiToScanCode(c);
    if (code == 0)
    {
        return;
    }

    // 押下と解放をまとめて送る。X68000 のキーボードは押下でコード、
    // 解放でコード | $80 を送る。
    machine.pressKey(code);
    machine.pressKey(static_cast<x68k::u8>(code | 0x80u));
}

x68k::u8 TouchKeyboard::asciiToScanCode(char ascii)
{
    // X68000 キーボードのスキャンコード。
    // 出典: X68000 のキーマトリクス表。ASCII ではないので変換が要る。
    switch (ascii)
    {
        // 数字段
        case '1':
            return 0x02;
        case '2':
            return 0x03;
        case '3':
            return 0x04;
        case '4':
            return 0x05;
        case '5':
            return 0x06;
        case '6':
            return 0x07;
        case '7':
            return 0x08;
        case '8':
            return 0x09;
        case '9':
            return 0x0A;
        case '0':
            return 0x0B;

        // 上段
        case 'q':
            return 0x10;
        case 'w':
            return 0x11;
        case 'e':
            return 0x12;
        case 'r':
            return 0x13;
        case 't':
            return 0x14;
        case 'y':
            return 0x15;
        case 'u':
            return 0x16;
        case 'i':
            return 0x17;
        case 'o':
            return 0x18;
        case 'p':
            return 0x19;

        // 中段
        case 'a':
            return 0x1E;
        case 's':
            return 0x1F;
        case 'd':
            return 0x20;
        case 'f':
            return 0x21;
        case 'g':
            return 0x22;
        case 'h':
            return 0x23;
        case 'j':
            return 0x24;
        case 'k':
            return 0x25;
        case 'l':
            return 0x26;

        // 下段
        case 'z':
            return 0x2A;
        case 'x':
            return 0x2B;
        case 'c':
            return 0x2C;
        case 'v':
            return 0x2D;
        case 'b':
            return 0x2E;
        case 'n':
            return 0x2F;
        case 'm':
            return 0x30;

        // 記号と制御
        case '.':
            return 0x32;
        case '/':
            return 0x33;
        case ' ':
            return 0x35;
        case '\n':
            return 0x1D;  // RETURN

        default:
            return 0;
    }
}

}  // namespace x68k_platform
