#include <array>
#include <cstring>
#include <limits>
#include "doctest.h"
#include "../src/x68k/platform/storage_flash.cpp"

namespace
{
std::array<std::uint8_t, 1024> flashBytes{};
esp_partition_t partition{flashBytes.size()};
bool mapFails = false;
unsigned reads = 0;
unsigned unmaps = 0;
std::size_t mappedBytes = 0;
void put32(std::size_t at, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
    {
        flashBytes[at + i] = static_cast<std::uint8_t>(value >> (24 - i * 8));
    }
}
void prepareFlash()
{
    flashBytes.fill(0);
    put32(0, 0x58363846);
    put32(4, 1);
    put32(8, 64);
    put32(12, 4);
    put32(24, 2);
    put32(28, 68);
    put32(32, 76);
    put32(36, 1);
    put32(64, 0x12345678);
    put32(68, 0xFFFFFFFF);
    put32(72, 0);
    std::fill(flashBytes.begin() + 76, flashBytes.begin() + 332, 0xA5);
    reads = 0;
    mapFails = false;
}
}  // namespace
const esp_partition_t* esp_partition_find_first(int, int, const char*)
{
    return &partition;
}
esp_err_t esp_partition_read(const esp_partition_t*, std::size_t offset, void* dst,
                             std::size_t size)
{
    ++reads;
    const bool outside = offset > flashBytes.size() || size > flashBytes.size() - offset;
    if (outside)
    {
        return -1;
    }
    std::memcpy(dst, flashBytes.data() + offset, size);
    return ESP_OK;
}
esp_err_t esp_partition_mmap(const esp_partition_t*, std::size_t offset, std::size_t size, int,
                             const void** ptr, esp_partition_mmap_handle_t* handle)
{
    if (mapFails)
    {
        return -1;
    }
    mappedBytes = size;
    *ptr = flashBytes.data() + offset;
    *handle = 1;
    return ESP_OK;
}
void esp_partition_munmap(esp_partition_mmap_handle_t)
{
    ++unmaps;
}
std::int64_t esp_timer_get_time()
{
    return 0;
}

TEST_CASE("Flash mapping preserves sparse sectors and avoids repeated partition reads")
{
    prepareFlash();
    REQUIRE(x68k_platform::mountFlashData());
    CHECK(mappedBytes == 332);
    const auto readsAfterMount = reads;
    x68k_platform::FlashDisk disk;
    REQUIRE(disk.open());
    std::array<std::uint8_t, 512> data{};
    REQUIRE(disk.readSector(0, data.data(), 2));
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        CHECK(data[i] == (i < 256 ? 0 : 0xA5));
    }
    CHECK(reads == readsAfterMount);
    CHECK_FALSE(disk.readSector(std::numeric_limits<std::uint32_t>::max(), data.data(), 2));
    CHECK_FALSE(disk.readSector(1, data.data(), std::numeric_limits<std::uint32_t>::max()));
    std::array<std::uint8_t, 4> rom{};
    CHECK(x68k_platform::loadFlashIplRom(rom.data(), rom.size()) == 4);
    CHECK(rom[0] == 0x12);
    CHECK(reads == readsAfterMount);
}
TEST_CASE("Flash remount releases mapping and preserves fallback reads")
{
    prepareFlash();
    REQUIRE(x68k_platform::mountFlashData());
    const auto previousUnmaps = unmaps;
    mapFails = true;
    REQUIRE(x68k_platform::mountFlashData());
    CHECK(unmaps == previousUnmaps + 1);
    x68k_platform::FlashDisk disk;
    REQUIRE(disk.open());
    std::array<std::uint8_t, 256> data{};
    const auto previousReads = reads;
    REQUIRE(disk.readSector(1, data.data(), 1));
    CHECK(data[0] == 0xA5);
    CHECK(reads == previousReads + 2);
}
TEST_CASE("Malformed flash ranges and sparse slots are rejected")
{
    prepareFlash();
    put32(36, std::numeric_limits<std::uint32_t>::max());
    CHECK_FALSE(x68k_platform::mountFlashData());
    prepareFlash();
    put32(72, 1);
    REQUIRE(x68k_platform::mountFlashData());
    x68k_platform::FlashDisk disk;
    REQUIRE(disk.open());
    std::array<std::uint8_t, 256> data{};
    CHECK_FALSE(disk.readSector(1, data.data(), 1));
}
