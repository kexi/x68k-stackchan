// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "sram.h"

#include <cstring>

namespace x68k
{
namespace
{

// IPL-ROM がマジックとして検査する 8 バイト。
//
// 先頭は半角の 'X' ではなく Shift_JIS の全角「Ｘ」($82 $77)。実機の
// IPL-ROM が書く値を読み出して確かめた。半角で置くとマジック不正と
// 判断され、SRAM 全体を工場出荷状態へ書き戻される。そのとき起動デバイスも
// $0000 (標準優先順位) に戻るため、SASI 起動の設定が消えてしまう。
constexpr std::uint8_t kMagicText[] = {0x82, 0x77, '6', '8', '0', '0', '0'};
constexpr std::uint8_t kMagicTail = 0x57;
constexpr std::uint32_t kMagicLength = 8;

}  // namespace

void Sram::write32(std::uint32_t offset, std::uint32_t value)
{
    write8(offset, static_cast<std::uint8_t>(value >> 24));
    write8(offset + 1, static_cast<std::uint8_t>(value >> 16));
    write8(offset + 2, static_cast<std::uint8_t>(value >> 8));
    write8(offset + 3, static_cast<std::uint8_t>(value));
}

bool Sram::hasValidMagic() const
{
    for (std::uint32_t i = 0; i < 7; ++i)
    {
        if (data_[kOffsetMagic + i] != kMagicText[i])
        {
            return false;
        }
    }
    // 8 バイト目はメモリチェック完了を示す $57。
    return data_[kOffsetMagic + 7] == kMagicTail;
}

void Sram::formatDefaults()
{
    data_.fill(0);

    // マジック。IPL-ROM はこれを見て「初期化済みの SRAM」と判断する。
    std::memcpy(data_.data() + kOffsetMagic, kMagicText, sizeof(kMagicText));
    data_[kOffsetMagic + 7] = kMagicTail;
    static_assert(kMagicLength == 8, "magic layout assumes 8 bytes");

    // メインメモリの実装容量。バスが実際に返す範囲と一致させないと、
    // IPL-ROM のメモリチェックが通らないか、Human68k が存在しない領域を使う。
    write32(kOffsetRamSize, kDefaultRamSize);

    write32(kOffsetRomBoot, kDefaultRomBootAddr);
    write32(kOffsetSramBoot, kDefaultSramBootAddr);

    // 起動デバイスは SASI を最優先にする。
    //
    // Why not 標準優先順位 ($0000): 標準だと FD から探し始める。本エミュレータの
    // FDC はドライブ未接続を表すスタブでしかなく、IPL-ROM の
    // 「RQM|DIO|CB が揃うのを待つ」ループ ($FF89DE) はタイムアウトを持たない。
    // 応答しなければ永久に待ち、応答すれば「FD がある」と誤認されて
    // コマンド処理へ進みエラー停止する。どちらにも倒せないので、
    // FD を経由せず直接 SASI から起動させる。
    //
    // FDC を正しく実装すれば標準優先順位に戻せる。
    write16(kOffsetBootDevice, kBootDeviceSasi0);

    data_[kOffsetScreenMode] = kDefaultScreenMode;

    // テキストパレットの規定値。
    //
    // IPL-ROM $FF63F0 はここを $E82200 へ丸ごと転送する。0 のままだと
    // 16 色すべてが黒になり、Human68k が文字を書いても画面は真っ黒のまま。
    // 実際 TVRAM には字形が入っているのに PPM が全黒だった。
    //
    // 色は GGGGG RRRRR BBBBB I。色 0 は背景 (黒)、色 1 は通常文字。
    // 並びは「色 1 = 白、色 2 = 黄、色 3 = シアン」。
    // (docs/knowledge/x68000-boot-sequence.md)。
    //
    // Why not 全色を白にする: 色番号は文字属性として意味を持つ。
    // 同じ色にすると反転表示や強調が区別できなくなる。
    {
        constexpr std::uint16_t kDefaultTextPalette[kTextPaletteCount] = {
            0x0000,  //  0 黒 (背景)
            0xFFFF,  //  1 白
            0x07FF,  //  2 黄 (G+R)
            0xF83F,  //  3 シアン (G+B)
            0x07C1,  //  4 赤
            0xF801,  //  5 緑
            0x003F,  //  6 青
            0xFFFF,  //  7 白
            0x0000,  //  8 黒
            0xFFFF,  //  9 白
            0x07FF,  // 10 黄
            0xF83F,  // 11 シアン
            0x07C1,  // 12 赤
            0xF801,  // 13 緑
            0x003F,  // 14 青
            0xFFFF,  // 15 白
        };
        for (std::uint32_t i = 0; i < kTextPaletteCount; ++i)
        {
            write16(kOffsetTextPalette + i * 2, kDefaultTextPalette[i]);
        }
    }

    // SASI を 1 台繋がっていることにする。
    data_[kOffsetSasiCount] = kDefaultSasiCount;

    // 初期化直後は「保存すべき変更」ではない (工場出荷状態そのもの)。
    dirty_ = false;
}

}  // namespace x68k
