// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// バッテリバックアップ SRAM ($ED0000, 16KB)。
//
// IPL-ROM は起動時にここのマジックを検査し、メモリ容量や起動デバイスの設定を
// 読む。初期値が正しくないと起動シーケンスが先へ進まないため、エミュレータは
// 「工場出荷状態の SRAM」を用意する必要がある。
//
// 実機ではバッテリで内容が保持される。エミュレータではファイルに保存して
// 次回起動時に復元する (保存は platform 層の責務)。

#ifndef X68K_CORE_DEV_SRAM_H
#define X68K_CORE_DEV_SRAM_H

#include <array>
#include <cstdint>

#include "../memmap.h"

namespace x68k
{

class Sram
{
public:
    // 主要オフセット (SRAM 先頭からの相対)。
    // 出典は docs/knowledge/x68000-boot-sequence.md。
    static constexpr std::uint32_t kOffsetMagic = 0x000;       // "X68000" + $57 (8B)
    static constexpr std::uint32_t kOffsetRamSize = 0x008;     // メインメモリ実装容量 (4B)
    static constexpr std::uint32_t kOffsetRomBoot = 0x00C;     // ROM 起動アドレス (4B)
    static constexpr std::uint32_t kOffsetSramBoot = 0x010;    // SRAM 起動アドレス (4B)
    static constexpr std::uint32_t kOffsetBootDevice = 0x018;  // 最優先起動デバイス (2B)
    static constexpr std::uint32_t kOffsetRs232c = 0x01A;      // RS-232C 設定 (2B)
    static constexpr std::uint32_t kOffsetScreenMode = 0x01D;  // 起動時画面モード (1B)

    // 工場出荷状態の値。
    static constexpr std::uint32_t kDefaultRamSize = kMainRamSize;
    static constexpr std::uint32_t kDefaultRomBootAddr = 0x00FC0000;
    static constexpr std::uint32_t kDefaultSramBootAddr = 0x00ED0100;
    // $0000 = 標準優先順位 (FD → SASI → ROM → RAM)。
    static constexpr std::uint16_t kBootDeviceStandard = 0x0000;
    static constexpr std::uint8_t kDefaultScreenMode = 16;

    Sram()
    {
        formatDefaults();
    }

    // 工場出荷状態に初期化する。マジックが壊れているときに IPL-ROM がやることを
    // エミュレータ側で先回りして行う。
    void formatDefaults();

    [[nodiscard]] std::uint8_t read8(std::uint32_t offset) const
    {
        return offset < data_.size() ? data_[offset] : 0u;
    }

    void write8(std::uint32_t offset, std::uint8_t value)
    {
        if (offset < data_.size())
        {
            data_[offset] = value;
            dirty_ = true;
        }
    }

    [[nodiscard]] std::uint16_t read16(std::uint32_t offset) const
    {
        return static_cast<std::uint16_t>((read8(offset) << 8) | read8(offset + 1));
    }

    void write16(std::uint32_t offset, std::uint16_t value)
    {
        write8(offset, static_cast<std::uint8_t>(value >> 8));
        write8(offset + 1, static_cast<std::uint8_t>(value & 0xFFu));
    }

    // ファイルへ保存すべき変更があるか。書き込みのたびに保存すると
    // SD カードの寿命を削るので、platform 層がこれを見て間引く。
    [[nodiscard]] bool isDirty() const
    {
        return dirty_;
    }

    void clearDirty()
    {
        dirty_ = false;
    }

    [[nodiscard]] const std::uint8_t* data() const
    {
        return data_.data();
    }

    [[nodiscard]] std::uint8_t* data()
    {
        return data_.data();
    }

    [[nodiscard]] static constexpr std::uint32_t size()
    {
        return kSramSize;
    }

    // マジックが正しいか。壊れていれば IPL-ROM が初期化しにかかる。
    [[nodiscard]] bool hasValidMagic() const;

private:
    void write32(std::uint32_t offset, std::uint32_t value);

    std::array<std::uint8_t, kSramSize> data_{};
    bool dirty_ = false;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_SRAM_H
