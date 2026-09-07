// SPDX-License-Identifier: MIT
#include "../host/run_deadline.h"
#include "doctest.h"
#include <limits>

TEST_CASE("入力台本の期限でrunを区切り100000cycle先まで入力を遅らせない")
{
    using x68k_host::limitRunToEvent;
    CHECK(limitRunToEvent(100000, 0, 17) == 17);
    CHECK(limitRunToEvent(100000, 17, 18) == 1);
    CHECK(limitRunToEvent(100000, 18, 18) == 0);
    CHECK(limitRunToEvent(100000, 20, 18) == 0);
    CHECK(limitRunToEvent(100000, 0, 200000) == 100000);
    CHECK(limitRunToEvent(5, 10, 20) == 5);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(limitRunToEvent(100000, maximum - 7, maximum) == 7);
    CHECK(limitRunToEvent(100000, maximum, 0) == 0);
}
