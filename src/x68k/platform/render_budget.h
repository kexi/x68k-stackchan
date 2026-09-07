// SPDX-License-Identifier: MIT
#ifndef X68K_PLATFORM_RENDER_BUDGET_H
#define X68K_PLATFORM_RENDER_BUDGET_H

#include <algorithm>
#include <cstdint>

namespace x68k_platform
{

// Core1専用。描画の実時間を返済してから次を許可し、休止中の予算は貯めない。
// 開始間隔だけでは、描画時間がその間隔を超えるとCPUを使い切るため。
class RenderBudget
{
public:
    explicit RenderBudget(std::int64_t minIntervalUs = 100000) : minIntervalUs_(minIntervalUs) {}

    [[nodiscard]] bool mayStart(std::int64_t nowUs) const
    {
        return nowUs >= nextStartUs_;
    }

    void completed(std::int64_t startUs, std::int64_t endUs)
    {
        const auto costUs = std::max<std::int64_t>(0, endUs - startUs);
        // 40%の時間予算。描画後に実測コストの1.5倍を他の処理へ残す。
        // 後追いで省略フレームを描くと、空けたCPU時間をまた使い切る。
        nextStartUs_ = std::max(startUs + minIntervalUs_, endUs + (costUs * 3 + 1) / 2);
    }

private:
    std::int64_t minIntervalUs_;
    std::int64_t nextStartUs_ = 0;
};

}  // namespace x68k_platform
#endif
