// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_CPU_MOVE_TIMING_H
#define X68K_CORE_CPU_MOVE_TIMING_H

#include "m68k_types.h"

namespace x68k
{
namespace move_timing_detail
{
// Local constexpr arrays were copied onto the Xtensa stack on every MOVE.
// Static immutable storage needs neither those copies nor an initialization guard.
inline constexpr u8 sourceBase[8] = {0, 0, 4, 4, 6, 8, 10, 0};
inline constexpr u8 sourceExtended[8] = {8, 12, 8, 10, 4, 0, 0, 0};
inline constexpr u8 destinationBase[8] = {0, 0, 4, 4, 4, 8, 10, 0};
inline constexpr u8 destinationExtended[8] = {8, 12, 0, 0, 0, 0, 0, 0};
}  // namespace move_timing_detail

// MC68000UM section 8.2, tables 8-2/8-3 (16-bit bus, no wait states).
// Legal MOVE/MOVEA only; decoding and exceptions stay with the caller.
// Why not a flat four cycles: operand transfers and extension fetches advance
// the same guest clock as device timers and audio, even on a fast RAM path.
[[nodiscard]] constexpr u32 moveInstructionCycles(u16 opcode, u32 size)
{
    using namespace move_timing_detail;
    const u32 sourceMode = (opcode >> 3) & 7u;
    const u32 destinationMode = (opcode >> 6) & 7u;
    const u32 source = sourceMode == 7 ? sourceExtended[opcode & 7u] : sourceBase[sourceMode];
    const u32 destination = destinationMode == 7 ? destinationExtended[(opcode >> 9) & 7u]
                                                 : destinationBase[destinationMode];
    const bool longSource = size == 4 && sourceMode >= 2;
    const bool longDestination = size == 4 && destinationMode >= 2;
    return 4u + source + destination + (longSource ? 4u : 0u) + (longDestination ? 4u : 0u);
}
}  // namespace x68k
#endif
