// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "sram.h"

#include <cstring>

namespace x68k
{
namespace
{

// IPL-ROM がマジックとして検査する 8 バイト。"X68000" の後ろは
// メモリチェックが完了したことを示す $57。
constexpr char kMagicText[] = "X68000";
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
    for (std::uint32_t i = 0; i < 6; ++i)
    {
        if (data_[kOffsetMagic + i] != static_cast<std::uint8_t>(kMagicText[i]))
        {
            return false;
        }
    }
    // 7 バイト目は $00、8 バイト目が $57。
    return data_[kOffsetMagic + 7] == kMagicTail;
}

void Sram::formatDefaults()
{
    data_.fill(0);

    // マジック。IPL-ROM はこれを見て「初期化済みの SRAM」と判断する。
    std::memcpy(data_.data() + kOffsetMagic, kMagicText, 6);
    data_[kOffsetMagic + 6] = 0x00;
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

    // 初期化直後は「保存すべき変更」ではない (工場出荷状態そのもの)。
    dirty_ = false;
}

}  // namespace x68k
