// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "storage_flash.h"

#include <cstring>

#include <esp_log.h>
#include <esp_partition.h>

namespace x68k_platform
{
namespace
{

constexpr const char* kTag = "x68k-flash";

// tools/mkflashimage.py が書くヘッダ。すべてビッグエンディアン。
constexpr std::uint32_t kMagic = 0x58363846u;  // "X68F"
constexpr std::uint32_t kVersion = 1;
constexpr x68k::u32 kSectorSize = 256;
constexpr std::uint32_t kNoSector = 0xFFFFFFFFu;

struct FlashHeader
{
    std::uint32_t ipl_offset;
    std::uint32_t ipl_size;
    std::uint32_t cgrom_offset;
    std::uint32_t cgrom_size;
    std::uint32_t disk_sectors;
    std::uint32_t index_offset;
    std::uint32_t data_offset;
    std::uint32_t data_sectors;
};

const esp_partition_t* g_part = nullptr;
FlashHeader g_header{};
bool g_ready = false;

// ビッグエンディアンの 32bit を読む。
//
// Why not そのまま読まないか: ESP32 はリトルエンディアン。イメージは
// 68000 に合わせてビッグで書いてあるので、明示的に組み立てる。
std::uint32_t be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

bool readAt(std::uint32_t offset, void* dst, std::size_t size)
{
    if (g_part == nullptr)
    {
        return false;
    }
    return esp_partition_read(g_part, offset, dst, size) == ESP_OK;
}

}  // namespace

bool mountFlashData()
{
    g_ready = false;

    g_part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (g_part == nullptr)
    {
        ESP_LOGW(kTag, "storage パーティションが見つかりません");
        return false;
    }

    std::uint8_t head[64] = {};
    if (!readAt(0, head, sizeof(head)))
    {
        ESP_LOGW(kTag, "storage を読めません");
        return false;
    }

    if (be32(head) != kMagic)
    {
        // 焼かれていない (消去直後は全部 $FF) 場合もここへ来る。異常ではない。
        ESP_LOGI(kTag, "flash にデータが焼かれていません");
        return false;
    }
    if (be32(head + 4) != kVersion)
    {
        ESP_LOGW(kTag, "flash のデータ形式が新しすぎます (version=%u)", be32(head + 4));
        return false;
    }

    g_header.ipl_offset = be32(head + 8);
    g_header.ipl_size = be32(head + 12);
    g_header.cgrom_offset = be32(head + 16);
    g_header.cgrom_size = be32(head + 20);
    g_header.disk_sectors = be32(head + 24);
    g_header.index_offset = be32(head + 28);
    g_header.data_offset = be32(head + 32);
    g_header.data_sectors = be32(head + 36);

    g_ready = true;
    ESP_LOGI(kTag, "flash のデータを使います (IPL %u バイト / ディスク %u セクタ中 %u 実体)",
             g_header.ipl_size, g_header.disk_sectors, g_header.data_sectors);
    return true;
}

std::size_t loadFlashIplRom(std::uint8_t* buffer, std::size_t bufferSize)
{
    if (!g_ready || g_header.ipl_size == 0 || g_header.ipl_size > bufferSize)
    {
        return 0;
    }
    if (!readAt(g_header.ipl_offset, buffer, g_header.ipl_size))
    {
        return 0;
    }
    return g_header.ipl_size;
}

std::size_t loadFlashCgRom(std::uint8_t* buffer, std::size_t bufferSize)
{
    if (!g_ready || g_header.cgrom_size == 0 || g_header.cgrom_size > bufferSize)
    {
        return 0;
    }
    if (!readAt(g_header.cgrom_offset, buffer, g_header.cgrom_size))
    {
        return 0;
    }
    return g_header.cgrom_size;
}

bool FlashDisk::open()
{
    present_ = g_ready && g_header.disk_sectors > 0;
    return present_;
}

bool FlashDisk::readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount)
{
    if (!present_)
    {
        return false;
    }
    if (lba + sectorCount > g_header.disk_sectors)
    {
        return false;
    }

    for (x68k::u32 i = 0; i < sectorCount; ++i)
    {
        std::uint8_t raw[4];
        if (!readAt(g_header.index_offset + (lba + i) * 4u, raw, sizeof(raw)))
        {
            return false;
        }
        const std::uint32_t slot = be32(raw);

        std::uint8_t* dst = buffer + static_cast<std::size_t>(i) * kSectorSize;
        if (slot == kNoSector)
        {
            // 索引に無いセクタはゼロ。イメージのほとんどはここへ来る。
            std::memset(dst, 0, kSectorSize);
            continue;
        }
        if (slot >= g_header.data_sectors)
        {
            return false;
        }
        if (!readAt(g_header.data_offset + slot * kSectorSize, dst, kSectorSize))
        {
            return false;
        }
    }
    return true;
}

bool FlashDisk::writeSector(x68k::u32 lba, const x68k::u8* buffer, x68k::u32 sectorCount)
{
    // 書き込みは受け付けない。
    //
    // Why: flash は消去がブロック単位 (4KB) で、1 セクタ (256 バイト) を
    // 書くたびに読んで消して書き直すことになる。寿命にも速度にも見合わない。
    // 書き込みが要るソフトは SD を使う。
    (void)lba;
    (void)buffer;
    (void)sectorCount;
    return false;
}

bool FlashDisk::isPresent() const
{
    return present_;
}

}  // namespace x68k_platform
