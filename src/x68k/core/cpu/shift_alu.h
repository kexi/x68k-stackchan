// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_CPU_SHIFT_ALU_H
#define X68K_CORE_CPU_SHIFT_ALU_H

#include "m68k_alu.h"

namespace x68k::alu
{
struct ShiftResult
{
    u32 value;
    bool carry;
    bool extend;
    bool overflow;
};

// count is the decoded 68000 count (0..63), size is 1/2/4 bytes.
// Why not one iteration per bit: the host work must not scale with guest shift time.
inline ShiftResult shift(u32 input, u32 size, u32 count, u32 type, bool left, bool extend)
{
    const auto bits = size * 8u;
    const auto mask = truncate(0xffffffffu, size);
    const auto value = input & mask;
    ShiftResult out{value, false, extend, false};
    const bool zeroCount = count == 0;
    if (zeroCount)
    {
        out.carry = type == 2 && extend;
        return out;
    }
    const bool rotateExtend = type == 2;
    if (rotateExtend)
    {
        // A 33-bit ring is necessary for ROX.L; 32-bit shifts lose X at a full turn.
        const auto width = bits + 1u;
        const auto amount = count % width;
        const u64 ringMask = (u64{1} << width) - 1u;
        const u64 ring = static_cast<u64>(value) | (static_cast<u64>(extend) << bits);
        const u64 rotated = left ? ((ring << amount) | (ring >> (width - amount)))
                                 : ((ring >> amount) | (ring << (width - amount)));
        const auto result = rotated & ringMask;
        out.value = static_cast<u32>(result) & mask;
        out.extend = ((result >> bits) & 1u) != 0;
        out.carry = out.extend;
        return out;
    }
    const bool rotate = type == 3;
    if (rotate)
    {
        const auto amount = count & (bits - 1u);
        const bool partialTurn = amount != 0;
        if (partialTurn)
        {
            out.value = (left ? (value << amount) | (value >> (bits - amount))
                              : (value >> amount) | (value << (bits - amount))) &
                        mask;
        }
        out.carry = left ? (out.value & 1u) != 0 : (out.value & signBit(size)) != 0;
        return out;
    }
    const bool arithmetic = type == 0;
    const bool shorterThanWidth = count < bits;
    const bool withinWidth = count <= bits;
    if (left)
    {
        out.carry = withinWidth && ((value >> (bits - count)) & 1u) != 0;
        out.value = shorterThanWidth ? (value << count) & mask : 0;
        out.overflow = arithmetic &&
                       (shorterThanWidth ? (((value ^ (value << 1u)) & mask) >> (bits - count)) != 0
                                         : value != 0);
    }
    else
    {
        const bool negative = arithmetic && (value & signBit(size)) != 0;
        out.carry = withinWidth ? ((value >> (count - 1u)) & 1u) != 0 : negative;
        out.value = shorterThanWidth ? value >> count : 0;
        if (negative)
        {
            out.value |= shorterThanWidth ? mask ^ (mask >> count) : mask;
        }
    }
    out.extend = out.carry;
    return out;
}
}  // namespace x68k::alu
#endif
