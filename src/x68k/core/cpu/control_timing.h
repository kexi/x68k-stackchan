// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_CPU_CONTROL_TIMING_H
#define X68K_CORE_CPU_CONTROL_TIMING_H

#include "m68k_types.h"

namespace x68k
{
// MC68000UM table 8-10: legal LEA/PEA/JMP/JSR, normal completion, no waits.
// Operand-read EA costs cannot be reused: these instructions compute an address.
[[nodiscard]] constexpr u32 controlInstructionCycles(u16 opcode)
{
    const u32 mode = (opcode >> 3) & 7u;
    const u32 reg = opcode & 7u;
    const bool transfersControl = (opcode & 0xff80u) == 0x4e80u;
    const bool pushesStack = (opcode & 0xffc0u) == 0x4e80u || (opcode & 0xffc0u) == 0x4840u;
    const u32 base = (transfersControl ? 8u : 4u) + (pushesStack ? 8u : 0u);
    const bool indexed = mode == 6 || (mode == 7 && reg == 3);
    if (indexed)
    {
        return base + (transfersControl ? 6u : 8u);
    }
    const bool absoluteLong = mode == 7 && reg == 1;
    if (absoluteLong)
    {
        return base + (transfersControl ? 4u : 8u);
    }
    const bool displaced = mode == 5 || (mode == 7 && (reg == 0 || reg == 2));
    return base + (displaced ? (transfersControl ? 2u : 4u) : 0u);
}
}  // namespace x68k
#endif
