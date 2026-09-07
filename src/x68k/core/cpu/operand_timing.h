// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_CPU_OPERAND_TIMING_H
#define X68K_CORE_CPU_OPERAND_TIMING_H

#include "m68k_types.h"

namespace x68k
{
namespace operand_timing
{
inline constexpr u8 base[] = {0, 0, 4, 4, 6, 8, 10, 0};
inline constexpr u8 extended[] = {8, 12, 8, 10, 4, 0, 0, 0};
}  // namespace operand_timing

// MC68000 table 8-1, including the operand read, for legal EAs without bus waits.
inline constexpr u32 operandReadCycles(u16 opcode, u32 size)
{
    const auto mode = (opcode >> 3) & 7u;
    const auto reg = opcode & 7u;
    const auto base = mode == 7 ? operand_timing::extended[reg] : operand_timing::base[mode];
    const bool longRead = size == 4 && mode >= 2;
    return base + (longRead ? 4u : 0u);
}

// MC68000 section 8.7: immediate bit numbers add an extension-word fetch.
inline constexpr u32 btstInstructionCycles(u16 opcode)
{
    const bool memory = (opcode & 0x38u) != 0;
    const bool dynamic = (opcode & 0x0100u) != 0;
    return (memory ? 4u : 6u) + (dynamic ? 0u : 4u) + operandReadCycles(opcode, 1);
}

inline constexpr u32 unaryInstructionCycles(u16 opcode, u32 size)
{
    const bool memory = (opcode & 0x38u) != 0;
    const bool test = (opcode & 0xff00u) == 0x4a00u;
    const auto base = test ? 4u : memory ? (size == 4 ? 12u : 8u) : (size == 4 ? 6u : 4u);
    return base + operandReadCycles(opcode, size);
}
}  // namespace x68k
#endif
