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

    // 大文字は小文字へ畳んで、シフト無しの同じキーとして打つ。
    //
    // 制限: 実機で 'A' を入力するにはシフトの押下と解放で挟む必要がある。
    // ここはそれをせず、ゲストには 'a' が入る。同様に記号のシフト面
    // ('!' や '?' など) も打てない (0 を返す)。
    //
    // Why not シフトを送れる形にしないか: この関数は「1 文字 → 1
    // スキャンコード」を返す契約で、呼び出し元 (host/main.cpp の --keys と
    // key_queue.cpp) はどちらも押下と解放を交互に送るだけの状態機械に
    // なっている。シフトを扱うには「シフト押下・キー押下・キー解放・
    // シフト解放」の 4 イベントを返す形へ変え、両方の状態機械を作り直す
    // 必要がある。
    //
    // それに見合う利得が今は無い。用途は起動確認で、Human68k の
    // コマンド名は大小どちらでも通る (`--keys "DIR"` で dir が動くことを
    // 実測済み)。大文字や記号のシフト面が要るユースケースが出てきたら
    // そのとき作り直す。
    const bool isUpperAlpha = c >= 'A' && c <= 'Z';
    if (isUpperAlpha)
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
