// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_CPU_GPIP_POLL_LOOP_H
#define X68K_CORE_CPU_GPIP_POLL_LOOP_H

#include <algorithm>
#include "m68k.h"
#include "m68k_alu.h"

namespace x68k
{
// Caller certifies GPIP is invariant strictly before the scheduler deadline,
// and that the bus has no retained fault from a previous access.
// No I/O, scheduler mutation, cached code pointers, or fixed guest PC is used here.
inline u32 tryGpipPollLoop(M68k& cpu, u8 gpip, std::int32_t debt)
{
    auto& state = cpu.state();
    const bool unavailable = state.ir != 0x1039 || state.irc != 0x00e8 || cpu.mustDeferToStep() ||
                             (state.sr & 0x8000u) != 0 || debt >= 0 || state.d[1] < 2 ||
                             state.d[1] > 18000;
    if (unavailable)
    {
        return 0;
    }
    constexpr u32 loopCycles = 52;
    const auto beforeDeadline = static_cast<u32>(-static_cast<std::int64_t>(debt) - 1);
    const auto loops = std::min(beforeDeadline / loopCycles, state.d[1] - 1);
    const bool tooShort = loops < 2;
    if (tooShort)
    {
        return 0;
    }
    const auto pc = (state.pc - 4) & M68k::kAddrMask;
    constexpr u16 expected[] = {0x1039, 0x00e8, 0x8001, 0x0800, 4, 0, 0x5381, 0x66f0};
    u16 branch = 0;
    for (u32 word = 0; word < 10; ++word)
    {
        u16 actual = 0;
        const bool readable = cpu.peekCodeWord(pc + word * 2, actual);
        if (!readable)
        {
            return 0;
        }
        const bool exitBranch = word == 5;
        if (exitBranch)
        {
            const bool supported = actual == 0x6604 || actual == 0x6704;
            if (!supported)
            {
                return 0;
            }
            branch = actual;
        }
        else if (word < 8 && actual != expected[word])
        {
            return 0;
        }
    }
    const bool bitSet = (gpip & 0x10u) != 0;
    const bool exits = branch == 0x6604 ? bitSet : !bitSet;
    if (exits)
    {
        return 0;
    }
    const auto result = alu::sub(state.d[1] - (loops - 1), 1, 4);
    state.d[0] = (state.d[0] & 0xffffff00u) | gpip;
    state.d[1] = result.value;
    state.sr = static_cast<u16>((state.sr & 0xffe0u) | (result.n ? 8u : 0u) | (result.z ? 4u : 0u) |
                                (result.v ? 2u : 0u) | (result.c ? 0x11u : 0u));
    return loops * loopCycles;
}
}  // namespace x68k
#endif
