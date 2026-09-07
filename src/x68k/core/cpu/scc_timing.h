// SPDX-License-Identifier: MIT
#pragma once

#include "m68k_types.h"

namespace x68k
{
// MC68000UM tables 8-1/8-6, legal data-alterable EA, normal completion without waits.
[[nodiscard]] constexpr u32 sccInstructionCycles(u16 opcode, bool condition)
{
    const u32 mode = (opcode >> 3) & 7u;
    const bool dataRegister = mode == 0;
    if (dataRegister)
    {
        return condition ? 6u : 4u;
    }
    const bool simpleIndirect = mode == 2 || mode == 3;
    if (simpleIndirect)
    {
        return 12;
    }
    const bool predecrement = mode == 4;
    if (predecrement)
    {
        return 14;
    }
    const bool indexed = mode == 6;
    if (indexed)
    {
        return 18;
    }
    const bool absoluteLong = mode == 7 && (opcode & 7u) == 1;
    return absoluteLong ? 20u : 16u;
}
}  // namespace x68k
