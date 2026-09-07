// SPDX-License-Identifier: MIT
#pragma once

#include "m68k_types.h"

namespace x68k
{
namespace immediate_timing_detail
{
// Static storage avoids copying a local constexpr table on Xtensa.
inline constexpr u8 byteWordEa[] = {0, 0, 4, 4, 6, 8, 10, 0};
}  // namespace immediate_timing_detail

// MC68000UM tables 8-1/8-5: legal data-alterable EA, normal completion, no waits.
[[nodiscard]] constexpr u32 immediateMemoryEaCycles(u16 opcode, u32 size)
{
    const u32 mode = (opcode >> 3) & 7u;
    const u32 base =
        mode == 7 ? ((opcode & 7u) == 1 ? 12u : 8u) : immediate_timing_detail::byteWordEa[mode];
    return base + (size == 4 ? 4u : 0u);
}

[[nodiscard]] constexpr u32 immediateInstructionCycles(u16 opcode, u32 size)
{
    const u32 mode = (opcode >> 3) & 7u;
    const u32 operation = (opcode >> 9) & 7u;
    const bool longOperand = size == 4;
    const bool compare = operation == 6;
    const bool dataRegister = mode == 0;
    if (dataRegister)
    {
        const bool shorterLong = operation == 1 || compare;
        return longOperand ? (shorterLong ? 14u : 16u) : 8u;
    }
    const u32 base = compare ? (longOperand ? 12u : 8u) : (longOperand ? 20u : 12u);
    return base + immediateMemoryEaCycles(opcode, size);
}

[[nodiscard]] constexpr u32 quickInstructionCycles(u16 opcode, u32 size)
{
    const u32 mode = (opcode >> 3) & 7u;
    const bool registerTarget = mode <= 1;
    if (registerTarget)
    {
        // ADDQ.W An is 4, SUBQ.W An is 8 in table 8-5 (also Musashi's 68000 table).
        const bool addressSubtract = mode == 1 && (opcode & 0x100u) != 0;
        return size == 4 || addressSubtract ? 8u : 4u;
    }
    return (size == 4 ? 12u : 8u) + immediateMemoryEaCycles(opcode, size);
}
}  // namespace x68k
