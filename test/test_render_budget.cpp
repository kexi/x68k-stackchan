// SPDX-License-Identifier: MIT
#include "doctest.h"
#include "render_budget.h"

TEST_CASE("軽い描画でも設定した最大fpsを超えない")
{
    x68k_platform::RenderBudget budget;
    CHECK(budget.mayStart(0));
    budget.completed(0, 1000);
    CHECK_FALSE(budget.mayStart(99999));
    CHECK(budget.mayStart(100000));
}

TEST_CASE("重い描画の後には実測コストに比例するCPU時間が残る")
{
    x68k_platform::RenderBudget budget;
    budget.completed(1000000, 1100000);
    CHECK_FALSE(budget.mayStart(1100000));
    CHECK_FALSE(budget.mayStart(1249999));
    CHECK(budget.mayStart(1250000));
    budget.completed(1250000, 1450000);
    CHECK_FALSE(budget.mayStart(1749999));
    CHECK(budget.mayStart(1750000));
}

TEST_CASE("長時間休止しても復帰時に描画を連発しない")
{
    x68k_platform::RenderBudget budget(33333);
    budget.completed(0, 1000);
    CHECK(budget.mayStart(1000000000));
    budget.completed(1000000000, 1000040000);
    CHECK_FALSE(budget.mayStart(1000099999));
    CHECK(budget.mayStart(1000100000));
}
