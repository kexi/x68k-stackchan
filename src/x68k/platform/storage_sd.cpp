// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "storage_sd.h"

#include <cstdio>
#include <string>

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

bool saveFile(const char* path, const std::uint8_t* buffer, std::size_t size)
{
    // いったん一時ファイルへ書き切ってから rename で置き換える。
    //
    // Why not 目的のファイルを直接 "wb" で開くか: fopen した時点で既存の
    // sram.dat が長さ 0 に切り詰められる。書いている途中で電源が落ちると
    // 半端な長さのファイルだけが残り、次回起動では大きさ検査に落ちて
    // 工場出荷値になる。つまり「保存しようとしたせいで、それまで正しく
    // 保存できていた設定まで失う」。バッテリバックアップの代替としては
    // 最悪の壊れ方で、保存しない方がまだましになってしまう。
    //
    // rename は FAT でもディレクトリエントリの書き換えだけで済み、
    // 「古い内容」か「新しい内容」のどちらかが残る。中間状態にならない。
    std::string tempPath(path);
    tempPath += ".tmp";

    std::FILE* f = std::fopen(tempPath.c_str(), "wb");
    if (f == nullptr)
    {
        return false;
    }

    const std::size_t written = std::fwrite(buffer, 1, size, f);
    // fflush だけでは足りない。FATFS は fclose で FAT とディレクトリエントリを
    // 確定させるので、閉じずに電源が切れると長さ 0 のファイルが残る。
    const bool isFlushed = std::fflush(f) == 0;
    const bool isClosed = std::fclose(f) == 0;
    if (written != size || !isFlushed || !isClosed)
    {
        std::remove(tempPath.c_str());
        return false;
    }

    // FATFS の rename は既存の宛先があると失敗するので、先に消す。
    // ここで電源が落ちると sram.dat は消えるが .tmp に完全な内容が残る。
    std::remove(path);
    if (std::rename(tempPath.c_str(), path) != 0)
    {
        std::remove(tempPath.c_str());
        return false;
    }
    return true;
}

// --- SRAM --------------------------------------------------------------------

bool loadSram(x68k::Machine& machine)
{
    // 16KB なのでスタックには置かず static にする。
    //
    // Why not スタックに置くか: この関数はエミュレーションタスク (スタック 8KB)
    // からも呼べる位置にあり、16KB の配列を積むとその場でオーバーフローする。
    // 呼ばれるのは起動時の 1 回だけなので、常駐しても惜しくない大きさでもない。
    static std::uint8_t image[x68k::kSramSize];

    const std::size_t size = loadFile(kSramPath, image, sizeof(image));
    if (size == 0)
    {
        // ファイルが無い (初回起動) か、16KB を超えている。
        return false;
    }

    // 長さとマジックの検査は core 側が行う。拒否されたら SRAM は変わらない。
    return machine.sram().loadImage(image, size);
}

bool saveSramIfDirty(x68k::Machine& machine)
{
    if (!machine.sram().isDirty())
    {
        return false;
    }

    // dirty は書き込みに成功したときだけ落とす。
    //
    // Why not 先に落として失敗を握り潰さないか: 「次に SRAM が書かれれば
    // また dirty が立つ」は、その書き込みが最後の設定変更だった場合に
    // 成立しない。SD の一時的な I/O エラーで 1 回失敗しただけで、以降
    // 誰も SRAM を書かなければ変更は永久に保存されない。設定を変えて
    // すぐ電源を切る使い方はまさにこれに当たる。
    //
    // 呼び出し側が 10 秒間隔で間引いているので、失敗しても再試行は
    // 10 秒に 1 回で済む。毎スライス 16KB を書き続けることにはならない。

    const bool ok = saveFile(kSramPath, machine.sram().data(), x68k::kSramSize);
    if (!ok)
    {
        ESP_LOGW(kTag, "SRAM を保存できません: %s", kSramPath);
        return false;
    }
    machine.sram().clearDirty();
    return true;
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
