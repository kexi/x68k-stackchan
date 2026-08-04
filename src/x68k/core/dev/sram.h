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
#include <cstddef>
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
    // テキストパレット 0-15 のシステム規定値 (16 ワード = 32B)。
    // IPL-ROM $FF63F0 が LEA $ED002E,A0 → MOVE.W (A0)+,(A1)+ ×16 で
    // $E82200 (テキストパレット) へそのまま転送する。
    static constexpr std::uint32_t kOffsetTextPalette = 0x02E;
    static constexpr std::uint32_t kTextPaletteCount = 16;
    static constexpr std::uint32_t kOffsetSasiCount = 0x05A;  // SASI 接続台数 (1B)

    // 工場出荷状態の値。
    static constexpr std::uint32_t kDefaultRamSize = kMainRamSize;
    // ROM 起動アドレス。実機の IPL-ROM が書く値を読み出して確かめた。
    // $FC0000 (SCSI ROM の先頭) ではなく $BFFFFC を指す。
    static constexpr std::uint32_t kDefaultRomBootAddr = 0x00BFFFFC;
    static constexpr std::uint32_t kDefaultSramBootAddr = 0x00ED0100;
    // 起動デバイスの指定。
    //   $0000 = 標準優先順位 (FD → SASI → ROM → RAM)
    //   $8x00 = SASI の x 番目を最優先
    //   $9x70 = FD の x 番目を最優先
    //   $A000 = ROM / $B000 = RAM
    static constexpr std::uint16_t kBootDeviceStandard = 0x0000;
    static constexpr std::uint16_t kBootDeviceSasi0 = 0x8000;
    static constexpr std::uint8_t kDefaultScreenMode = 16;
    // SASI の接続台数。IPL-ROM は $FF0B00 でこれを $000CB4 へ写し、
    // SASI ドライバ ($FF9684) が「ID < 台数か」で接続の有無を判定する。
    // 0 のままだと ID 0 すら範囲外となり、READ コマンドが一度も発行されない。
    static constexpr std::uint8_t kDefaultSasiCount = 1;

    Sram()
    {
        formatDefaults();
    }

    // 工場出荷状態に初期化する。マジックが壊れているときに IPL-ROM がやることを
    // エミュレータ側で先回りして行う。
    void formatDefaults();

    // 保存しておいたイメージを取り込む。受け入れたら true。
    //
    // 受け入れる条件は「ちょうど kSramSize バイト」かつ「マジックが正しい」。
    // 拒否した場合は現在の内容を一切変更しない。
    //
    // Why not ファイルを直接読ませるか: core/ は ESP32 非依存を保つ必要があり、
    // ファイルシステムの都合を持ち込めない。バイト列を受け取る形にすれば
    // 検査の中身はホストのテストで確かめられ、実機側は読むだけで済む。
    //
    // Why not 短いイメージを 0 埋めで受け入れるか: 途中で切れた sram.dat は
    // 「起動デバイスだけ読めて画面設定はゼロ」のような中途半端な状態を作る。
    // 症状は起動しないか画面が出ないかで、原因が SRAM だと気付きにくい。
    // 工場出荷値へ落とす方が復帰できる。
    bool loadImage(const std::uint8_t* image, std::size_t length);

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
