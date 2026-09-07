// SPDX-License-Identifier: MIT
#ifndef X68K_HOST_RUN_DEADLINE_H
#define X68K_HOST_RUN_DEADLINE_H

#include <algorithm>
#include <cstdint>

namespace x68k_host
{
// Due events must be delivered before running, not postponed by a full chunk.
inline std::uint64_t limitRunToEvent(std::uint64_t budget, std::uint64_t spent,
                                     std::uint64_t eventCycle)
{
    const bool due = eventCycle <= spent;
    if (due)
    {
        return 0;
    }
    return std::min(budget, eventCycle - spent);
}
}  // namespace x68k_host
#endif
