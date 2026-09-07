// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "storage_flash.h"

#include <algorithm>
#include <cstring>

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>

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
const std::uint8_t* g_mapped = nullptr;
std::size_t g_mappedSize = 0;
esp_partition_mmap_handle_t g_mapping = 0;

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
    const bool isOutsidePartition = offset > g_part->size || size > g_part->size - offset;
    if (isOutsidePartition)
    {
        return false;
    }
    const bool isMapped =
        g_mapped != nullptr && offset <= g_mappedSize && size <= g_mappedSize - offset;
    if (isMapped)
    {
        std::memcpy(dst, g_mapped + offset, size);
        return true;
    }
    return esp_partition_read(g_part, offset, dst, size) == ESP_OK;
}

}  // namespace

bool mountFlashData()
{
    g_ready = false;
    const bool hasMapping = g_mapped != nullptr;
    if (hasMapping)
    {
        esp_partition_munmap(g_mapping);
    }
    g_mapped = nullptr;
    g_mappedSize = 0;

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

    std::uint64_t imageEnd = sizeof(head);
    const auto includeRange = [&](std::uint32_t offset, std::uint64_t size)
    {
        const auto end = static_cast<std::uint64_t>(offset) + size;
        imageEnd = std::max(imageEnd, end);
        return end <= g_part->size;
    };
    const bool hasValidRanges =
        includeRange(g_header.ipl_offset, g_header.ipl_size) &&
        includeRange(g_header.cgrom_offset, g_header.cgrom_size) &&
        includeRange(g_header.index_offset,
                     static_cast<std::uint64_t>(g_header.disk_sectors) * 4) &&
        includeRange(g_header.data_offset,
                     static_cast<std::uint64_t>(g_header.data_sectors) * kSectorSize);
    if (!hasValidRanges)
    {
        ESP_LOGW(kTag, "flash のデータ範囲がパーティション外です");
        return false;
    }

    // 全パーティションを写すと未使用領域にもMMU枠を使うため、実データ末尾までに限る。
    const void* mapped = nullptr;
    const auto mapResult = esp_partition_mmap(g_part, 0, static_cast<std::size_t>(imageEnd),
                                              ESP_PARTITION_MMAP_DATA, &mapped, &g_mapping);
    const bool didMap = mapResult == ESP_OK;
    if (didMap)
    {
        g_mapped = static_cast<const std::uint8_t*>(mapped);
        g_mappedSize = static_cast<std::size_t>(imageEnd);
        ESP_LOGI(kTag, "[mapped-read] bytes=%u", static_cast<unsigned>(g_mappedSize));
    }
    else
    {
        ESP_LOGW(kTag, "[mapped-read-fallback] error=%d", static_cast<int>(mapResult));
    }

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
    const bool isOutsideDisk =
        lba > g_header.disk_sectors || sectorCount > g_header.disk_sectors - lba;
    if (isOutsideDisk)
    {
        return false;
    }

    const auto startedUs = esp_timer_get_time();
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
    const auto readUs = esp_timer_get_time() - startedUs;
    readTimeUs_ += readUs;
    // 全要求のログは再生を妨げるため、音声1ブロックの時間を超えた要求だけ出す。
    const bool exceedsAudioBlock = readUs >= 32768;
    if (exceedsAudioBlock)
    {
        ESP_LOGI(kTag, "[slow-read] lba=%u sectors=%u us=%lld", static_cast<unsigned>(lba),
                 static_cast<unsigned>(sectorCount), readUs);
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
