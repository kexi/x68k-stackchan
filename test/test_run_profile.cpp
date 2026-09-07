// SPDX-License-Identifier: MIT
#include "doctest.h"
#include "run_profile.h"

TEST_CASE("slice入口の同じ24bitページだけを集計する")
{
    x68k_platform::RunProfile profile;
    profile.add(0, 60000, 5000);
    profile.add(0xAB0000FE, 60004, 7000);
    profile.add(0x100, 10, 1);
    CHECK(profile.buckets[0].samples == 2);
    CHECK(profile.buckets[0].firstPc == 0);
    CHECK(profile.buckets[0].cycles == 120004);
    CHECK(profile.buckets[0].us == 12000);
    CHECK(profile.buckets[1].page == 0x100);
}

TEST_CASE("容量を超えたページは既存ページの時間へ混入しない")
{
    x68k_platform::RunProfile profile;
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        profile.add(i * 256, 60000, 1000);
    }
    profile.add(0, 1, 2);
    CHECK(profile.overflow.samples == 8);
    CHECK(profile.overflow.cycles == 480000);
    CHECK(profile.overflow.us == 8000);
    CHECK(profile.buckets[0].samples == 2);
    CHECK(profile.buckets[0].us == 1002);
    profile = {};
    CHECK(profile.overflow.samples == 0);
    CHECK(profile.buckets[0].samples == 0);
}
