// SPDX-License-Identifier: MIT
#ifndef X68K_PLATFORM_RUN_PROFILE_H
#define X68K_PLATFORM_RUN_PROFILE_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace x68k_platform
{
// Slice入口の256byteページ別集計。内部で通ったPCの実行時間ではない。
class RunProfile
{
public:
    struct Bucket
    {
        std::uint32_t page = 0;
        std::uint32_t samples = 0;
        std::uint32_t firstPc = 0;
        std::uint64_t us = 0;
        std::uint64_t cycles = 0;
    };
    static constexpr std::size_t kCapacity = 32;

    void add(std::uint32_t pc, std::uint32_t cycles, std::uint64_t us)
    {
        const auto page = pc & 0x00FFFF00u;
        for (auto& bucket : buckets)
        {
            const bool matches = bucket.samples != 0 && bucket.page == page;
            const bool vacant = bucket.samples == 0;
            if (matches || vacant)
            {
                bucket.page = page;
                if (vacant)
                {
                    bucket.firstPc = pc & 0x00FFFFFFu;
                }
                accumulate(bucket, cycles, us);
                return;
            }
        }
        // evictionで古い計測を別PCへ付け替えず、溢れを明示する。
        accumulate(overflow, cycles, us);
    }

    std::array<Bucket, kCapacity> buckets{};
    Bucket overflow{};

private:
    static void accumulate(Bucket& bucket, std::uint32_t cycles, std::uint64_t us)
    {
        ++bucket.samples;
        bucket.us += us;
        bucket.cycles += cycles;
    }
};
}  // namespace x68k_platform
#endif
