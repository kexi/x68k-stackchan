#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
using esp_partition_mmap_handle_t = std::uint32_t;
constexpr int ESP_OK = 0;
constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_ANY = 0;
constexpr int ESP_PARTITION_MMAP_DATA = 0;
struct esp_partition_t
{
    std::size_t size;
};
const esp_partition_t* esp_partition_find_first(int, int, const char*);
esp_err_t esp_partition_read(const esp_partition_t*, std::size_t, void*, std::size_t);
esp_err_t esp_partition_mmap(const esp_partition_t*, std::size_t, std::size_t, int, const void**,
                             esp_partition_mmap_handle_t*);
void esp_partition_munmap(esp_partition_mmap_handle_t);
