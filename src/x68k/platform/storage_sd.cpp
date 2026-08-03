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
#include "sram_persist.h"

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

namespace
{

// SramFileOps を FATFS で実装したもの。方針は sram_persist.h が持ち、
// ここは「実際にカードを触る」部分だけを引き受ける。
//
// Why not 方針もここに書くか: FATFS を直に叩く関数は電源断のタイミングを
// 再現できず、ホストのテストに載らない。壊れ方が「利用者の設定が全部消える」
// である以上、検査できない場所に判断を置きたくない。
class FatFileOps final : public SramFileOps
{
public:
    std::size_t read(const char* path, std::uint8_t* buffer, std::size_t bufferSize) override
    {
        return loadFile(path, buffer, bufferSize);
    }

    bool write(const char* path, const std::uint8_t* buffer, std::size_t size) override
    {
        std::FILE* f = std::fopen(path, "wb");
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
            // 半端なファイルを残さないのが SramFileOps::write の契約。
            std::remove(path);
            return false;
        }
        return true;
    }

    bool remove(const char* path) override
    {
        // 元から無い場合も成功として扱う。呼び出し側は区別しない。
        return std::remove(path) == 0 || !exists(path);
    }

    bool rename(const char* from, const char* to) override
    {
        return std::rename(from, to) == 0;
    }

    [[nodiscard]] bool exists(const char* path) override
    {
        std::FILE* f = std::fopen(path, "rb");
        if (f == nullptr)
        {
            return false;
        }
        std::fclose(f);
        return true;
    }
};

}  // namespace

bool saveFile(const char* path, const std::uint8_t* buffer, std::size_t size)
{
    FatFileOps ops;
    return saveSramImage(ops, path, buffer, size);
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

    // 本体が無い/壊れているときは .tmp からの復旧まで面倒を見る。
    // 保存は remove(本体) → rename(.tmp) の順に進むので、その隙間で電源が
    // 落ちると完全な .tmp だけが残る。ここで拾わないと、原子的に書いた意味が
    // 無くなって工場出荷値へ落ちる。並び順の根拠は sram_persist.h に書いた。
    FatFileOps ops;
    const SramLoadResult result =
        loadSramImage(ops, kSramPath, machine.sram(), image, sizeof(image));

    if (result == SramLoadResult::kRecovered)
    {
        ESP_LOGW(kTag, "本体が失われていたため %s%s から復旧しました", kSramPath, kTempSuffix);
    }
    return result != SramLoadResult::kNone;
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
