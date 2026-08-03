// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "ascii_keymap.h"

#include <cstring>

namespace x68k
{

u8 asciiToScanCode(char c)
{
    // 数字列とアルファベットは並びが連続していないので表で持つ。
    //
    // 各行の先頭スキャンコードは IPL-ROM 内の変換表から読み出して確かめた。
    // 非シフト面は $FF199C + スキャンコード、シフト面は $FF1A1C + スキャンコード。
    // そこで '1'=$02, 'Q'=$11, 'A'=$1E, 'Z'=$2A, 空白=$35, CR=$1D と読める。
    static const char* kRow1 = "1234567890-^\\";
    static const char* kRow2 = "qwertyuiop@[";
    static const char* kRow3 = "asdfghjkl;:]";
    static const char* kRow4 = "zxcvbnm,./";

    if (c >= 'A' && c <= 'Z')
    {
        c = static_cast<char>(c - 'A' + 'a');
    }

    // strchr は終端 '\0' にも当たるので、'\0' を弾いてから見る。
    const bool isNul = c == '\0';
    if (isNul)
    {
        return 0;
    }

    if (const char* p = std::strchr(kRow1, c); p != nullptr)
    {
        return static_cast<u8>(0x02 + static_cast<u32>(p - kRow1));
    }
    if (const char* p = std::strchr(kRow2, c); p != nullptr)
    {
        return static_cast<u8>(0x11 + static_cast<u32>(p - kRow2));
    }
    if (const char* p = std::strchr(kRow3, c); p != nullptr)
    {
        return static_cast<u8>(0x1E + static_cast<u32>(p - kRow3));
    }
    if (const char* p = std::strchr(kRow4, c); p != nullptr)
    {
        return static_cast<u8>(0x2A + static_cast<u32>(p - kRow4));
    }
    if (c == ' ')
    {
        return 0x35;
    }
    if (c == '\n' || c == '\r')
    {
        return 0x1D;  // CR
    }
    return 0;
}

}  // namespace x68k
