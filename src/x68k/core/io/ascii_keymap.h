// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ASCII から X68000 のキーボードスキャンコードへの対応。
//
// ホストの --keys と実機のシリアルコンソールの両方が使う。
// 「打った文字が実機で正しく入るか」はどちらでも同じ問いなので、
// 表を二重に持つと片方だけ直したときに食い違う。

#ifndef X68K_CORE_IO_ASCII_KEYMAP_H
#define X68K_CORE_IO_ASCII_KEYMAP_H

#include "../cpu/m68k_types.h"

namespace x68k
{

// 対応するスキャンコードを返す。打てない文字は 0。
//
// 起動確認に要るのは英数字と改行だけなので、その範囲に絞る。
// X68000 のキーボードは押下でスキャンコード、離すと bit7 を立てた値を送る。
//
// シフトは扱わない。'A' は 'a' と同じコードを返すので、ゲストに入るのは
// 小文字。シフト面の記号 ('!' など) は 0 になる。理由は .cpp に書いた。
[[nodiscard]] u8 asciiToScanCode(char c);

}  // namespace x68k

#endif  // X68K_CORE_IO_ASCII_KEYMAP_H
