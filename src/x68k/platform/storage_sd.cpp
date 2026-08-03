// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "storage_sd.h"

#include <cstdio>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace x68k_platform
{
namespace
{

constexpr char kTag[] = "x68k.sd";

// CoreS3 の microSD は SPI 接続。M5Stack の回路図より。
constexpr int kPinMosi = 37;
constexpr int kPinMiso = 35;
constexpr int kPinSclk = 36;
constexpr int kPinCs = 4;

sdmmc_card_t* g_card = nullptr;

}  // namespace

bool mountSd()
{
    if (g_card != nullptr)
    {
        return true;  // マウント済み
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // CoreS3 の LCD と SPI バスを共有する。M5Unified が先に初期化している
    // 場合があるので、バスの初期化は失敗しても続行する。
    host.slot = SPI2_HOST;

    spi_bus_config_t busConfig = {};
    busConfig.mosi_io_num = kPinMosi;
    busConfig.miso_io_num = kPinMiso;
    busConfig.sclk_io_num = kPinSclk;
    busConfig.quadwp_io_num = -1;
    busConfig.quadhd_io_num = -1;
    busConfig.max_transfer_sz = 4096;

    const esp_err_t busResult = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot),
                                                   &busConfig, SDSPI_DEFAULT_DMA);
    if (busResult != ESP_OK && busResult != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(kTag, "spi_bus_initialize failed: %s", esp_err_to_name(busResult));
        return false;
    }

    sdspi_device_config_t slotConfig = SDSPI_DEVICE_CONFIG_DEFAULT();
    slotConfig.gpio_cs = static_cast<gpio_num_t>(kPinCs);
    slotConfig.host_id = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mountConfig = {};
    // 起動できないカードをフォーマットしてしまうと利用者の ROM が消える。
    mountConfig.format_if_mount_failed = false;
    mountConfig.max_files = 4;
    mountConfig.allocation_unit_size = 16 * 1024;

    const esp_err_t result =
        esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slotConfig, &mountConfig, &g_card);
    if (result != ESP_OK)
    {
        ESP_LOGE(kTag, "SD のマウントに失敗: %s", esp_err_to_name(result));
        g_card = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "SD をマウントしました");
    return true;
}

std::size_t loadFile(const char* path, std::uint8_t* buffer, std::size_t bufferSize)
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        return 0;
    }

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (size <= 0 || static_cast<std::size_t>(size) > bufferSize)
    {
        // 部分読み込みは許さない。ROM が途中までしか読めていない状態で
        // 起動すると、原因の分からない暴走になる。
        std::fclose(f);
        return 0;
    }

    const std::size_t read = std::fread(buffer, 1, static_cast<std::size_t>(size), f);
    std::fclose(f);
    return read == static_cast<std::size_t>(size) ? read : 0;
}

// --- SdDisk ------------------------------------------------------------------

SdDisk::~SdDisk()
{
    close();
}

bool SdDisk::open(const char* path)
{
    close();
    file_ = std::fopen(path, "r+b");
    if (file_ == nullptr)
    {
        // 書き込み不可でも読めれば起動はできる。
        file_ = std::fopen(path, "rb");
    }
    return file_ != nullptr;
}

void SdDisk::close()
{
    if (file_ != nullptr)
    {
        std::fclose(static_cast<std::FILE*>(file_));
        file_ = nullptr;
    }
}

bool SdDisk::isPresent() const
{
    return file_ != nullptr;
}

bool SdDisk::readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount)
{
    if (file_ == nullptr)
    {
        return false;
    }
    auto* f = static_cast<std::FILE*>(file_);
    const long offset = static_cast<long>(lba) * static_cast<long>(kSectorSize);
    if (std::fseek(f, offset, SEEK_SET) != 0)
    {
        return false;
    }
    const std::size_t want = static_cast<std::size_t>(sectorCount) * kSectorSize;
    return std::fread(buffer, 1, want, f) == want;
}

bool SdDisk::writeSector(x68k::u32 lba, const x68k::u8* buffer, x68k::u32 sectorCount)
{
    if (file_ == nullptr)
    {
        return false;
    }
    auto* f = static_cast<std::FILE*>(file_);
    const long offset = static_cast<long>(lba) * static_cast<long>(kSectorSize);
    if (std::fseek(f, offset, SEEK_SET) != 0)
    {
        return false;
    }
    const std::size_t want = static_cast<std::size_t>(sectorCount) * kSectorSize;
    const bool ok = std::fwrite(buffer, 1, want, f) == want;
    std::fflush(f);
    return ok;
}

}  // namespace x68k_platform
