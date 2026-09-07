// SPDX-License-Identifier: MIT
#pragma once

#include "m68k_types.h"

namespace x68k
{
namespace standard_timing_detail
{
inline constexpr u8 eaBase[] = {0, 0, 4, 4, 6, 8, 10, 0};
inline constexpr u8 extendedEa[] = {8, 12, 8, 10, 4, 0, 0, 0};
}  // namespace standard_timing_detail

// MC68000UM tables 8-1/8-4, normal completion of legal standard ALU forms only.
// Special ADDX/SUBX, BCD, MUL/DIV, CMPM and EXG use their own timing contracts.
[[nodiscard]] constexpr u32 standardInstructionCycles(u16 opcode, u32 size)
{
    const u32 mode = (opcode >> 3) & 7u;
    const u32 reg = opcode & 7u;
    const u32 opmode = (opcode >> 6) & 7u;
    const bool longOperand = size == 4;
    const bool registerSource = mode <= 1;
    const bool immediateSource = mode == 7 && reg == 4;
    const u32 ea =
        mode == 7 ? standard_timing_detail::extendedEa[reg] : standard_timing_detail::eaBase[mode];
    const u32 eaCycles = ea + (longOperand && !registerSource ? 4u : 0u);
    const bool addressTarget = opmode == 3 || opmode == 7;
    const bool compareGroup = (opcode >> 12) == 0xb;
    if (addressTarget)
    {
        const u32 base = compareGroup                        ? 6u
                         : !longOperand                      ? 8u
                         : registerSource || immediateSource ? 8u
                                                             : 6u;
        return base + eaCycles;
    }
    const bool writesEa = (opmode & 4u) != 0;
    if (writesEa)
    {
        const bool eorRegister = compareGroup && mode == 0;
        if (eorRegister)
        {
            return longOperand ? 8u : 4u;
        }
        return (longOperand ? 12u : 8u) + eaCycles;
    }
    const bool extraInternalCycles = !compareGroup && (registerSource || immediateSource);
    return (longOperand ? (extraInternalCycles ? 8u : 6u) : 4u) + eaCycles;
}
}  // namespace x68k
