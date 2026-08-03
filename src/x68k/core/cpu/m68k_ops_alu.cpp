// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 算術・論理演算グループ (ADD/SUB/CMP/AND/OR/EOR とその派生)。

#include "m68k.h"
#include "m68k_alu.h"

namespace x68k
{
namespace
{

using alu::kByte;
using alu::kLong;
using alu::kWord;

// opmode の下位 2bit からサイズを得る。0=byte, 1=word, 2=long。
constexpr u32 sizeFromOpmode(u32 opmode)
{
    const u32 s = opmode & 3u;
    if (s == 0)
    {
        return kByte;
    }
    if (s == 1)
    {
        return kWord;
    }
    return kLong;
}

}  // namespace

// フラグを結果から立て直す共通処理。X は指定があるときだけ書く。
static u16 applyFlags(u16 sr, const alu::Result& r, bool writeExtend)
{
    u16 out = static_cast<u16>(
        sr & ~(sr_bit::kNegative | sr_bit::kZero | sr_bit::kOverflow | sr_bit::kCarry));
    if (r.n)
    {
        out |= sr_bit::kNegative;
    }
    if (r.z)
    {
        out |= sr_bit::kZero;
    }
    if (r.v)
    {
        out |= sr_bit::kOverflow;
    }
    if (r.c)
    {
        out |= sr_bit::kCarry;
    }
    if (writeExtend)
    {
        out = static_cast<u16>(out & ~sr_bit::kExtend);
        if (r.c)
        {
            out |= sr_bit::kExtend;
        }
    }
    return out;
}

// ADD / ADDA / ADDX (1101)
u32 M68k::groupAdd(u16 op)
{
    const u32 reg = (op >> 9) & 7u;
    const u32 opmode = (op >> 6) & 7u;
    const u32 mode = (op >> 3) & 7u;
    const u32 rm = op & 7u;

    // ADDA: opmode 3 (word) / 7 (long)。フラグを変えない。
    if (opmode == 3 || opmode == 7)
    {
        const u32 size = opmode == 3 ? kWord : kLong;
        const u32 src = readEa(mode, rm, size);
        const u32 value =
            size == kWord ? static_cast<u32>(static_cast<s32>(static_cast<s16>(src))) : src;
        st_.a[reg] = (st_.a[reg] + value) & 0xFFFFFFFFu;
        return 8;
    }

    const u32 size = sizeFromOpmode(opmode);
    const bool toMemory = (opmode & 4u) != 0;

    // ADDX: opmode が「メモリ方向」かつ mode が 0 か 1 のとき。
    if (toMemory && (mode == 0 || mode == 1))
    {
        const bool memoryMode = mode == 1;
        const u32 x = (st_.sr & sr_bit::kExtend) != 0 ? 1u : 0u;

        u32 src = 0;
        u32 dst = 0;
        u32 dstAddr = 0;
        if (memoryMode)
        {
            // -(Ay),-(Ax) 形式。両方ともプリデクリメント。
            const u32 srcAddr = effectiveAddress(4, rm, size);
            src = size == kByte ? read8(srcAddr)
                                : (size == kWord ? read16(srcAddr) : read32(srcAddr));
            dstAddr = effectiveAddress(4, reg, size);
            dst = size == kByte ? read8(dstAddr)
                                : (size == kWord ? read16(dstAddr) : read32(dstAddr));
        }
        else
        {
            src = alu::truncate(st_.d[rm], size);
            dst = alu::truncate(st_.d[reg], size);
        }

        const alu::Result first = alu::add(dst, src, size);
        const alu::Result second = alu::add(first.value, x, size);
        alu::Result r{};
        r.value = second.value;
        r.n = alu::isNegative(r.value, size);
        // Z は累積: 結果がゼロでも前の Z が false なら false のまま。
        // 多倍長加算で「全ワードがゼロのときだけ Z」を実現するための規則。
        r.z = alu::accumulateZero((st_.sr & sr_bit::kZero) != 0, r.value == 0);
        r.v = first.v || second.v;
        r.c = first.c || second.c;
        st_.sr = applyFlags(st_.sr, r, true);

        if (memoryMode)
        {
            if (size == kByte)
            {
                write8(dstAddr, static_cast<u8>(r.value));
            }
            else if (size == kWord)
            {
                write16(dstAddr, static_cast<u16>(r.value));
            }
            else
            {
                write32(dstAddr, r.value);
            }
            return size == kLong ? 30 : 18;
        }
        writeEa(0, reg, size, r.value);
        return size == kLong ? 8 : 4;
    }

    if (toMemory)
    {
        // ADD Dn,<ea>
        u32 addr = 0;
        const u32 dst = readEaForModify(mode, rm, size, addr);
        const alu::Result r = alu::add(dst, st_.d[reg], size);
        st_.sr = applyFlags(st_.sr, r, true);
        writeEaToAddr(mode, rm, size, addr, r.value);
        return 12;
    }

    // ADD <ea>,Dn
    const u32 src = readEa(mode, rm, size);
    const alu::Result r = alu::add(st_.d[reg], src, size);
    st_.sr = applyFlags(st_.sr, r, true);
    writeEa(0, reg, size, r.value);
    return 4;
}

// SUB / SUBA / SUBX (1001)
u32 M68k::groupSub(u16 op)
{
    const u32 reg = (op >> 9) & 7u;
    const u32 opmode = (op >> 6) & 7u;
    const u32 mode = (op >> 3) & 7u;
    const u32 rm = op & 7u;

    if (opmode == 3 || opmode == 7)
    {
        // SUBA
        const u32 size = opmode == 3 ? kWord : kLong;
        const u32 src = readEa(mode, rm, size);
        const u32 value =
            size == kWord ? static_cast<u32>(static_cast<s32>(static_cast<s16>(src))) : src;
        st_.a[reg] = (st_.a[reg] - value) & 0xFFFFFFFFu;
        return 8;
    }

    const u32 size = sizeFromOpmode(opmode);
    const bool toMemory = (opmode & 4u) != 0;

    if (toMemory && (mode == 0 || mode == 1))
    {
        // SUBX
        const bool memoryMode = mode == 1;
        const u32 x = (st_.sr & sr_bit::kExtend) != 0 ? 1u : 0u;

        u32 src = 0;
        u32 dst = 0;
        u32 dstAddr = 0;
        if (memoryMode)
        {
            const u32 srcAddr = effectiveAddress(4, rm, size);
            src = size == kByte ? read8(srcAddr)
                                : (size == kWord ? read16(srcAddr) : read32(srcAddr));
            dstAddr = effectiveAddress(4, reg, size);
            dst = size == kByte ? read8(dstAddr)
                                : (size == kWord ? read16(dstAddr) : read32(dstAddr));
        }
        else
        {
            src = alu::truncate(st_.d[rm], size);
            dst = alu::truncate(st_.d[reg], size);
        }

        const alu::Result first = alu::sub(dst, src, size);
        const alu::Result second = alu::sub(first.value, x, size);
        alu::Result r{};
        r.value = second.value;
        r.n = alu::isNegative(r.value, size);
        r.z = alu::accumulateZero((st_.sr & sr_bit::kZero) != 0, r.value == 0);
        r.v = first.v || second.v;
        r.c = first.c || second.c;
        st_.sr = applyFlags(st_.sr, r, true);

        if (memoryMode)
        {
            if (size == kByte)
            {
                write8(dstAddr, static_cast<u8>(r.value));
            }
            else if (size == kWord)
            {
                write16(dstAddr, static_cast<u16>(r.value));
            }
            else
            {
                write32(dstAddr, r.value);
            }
            return size == kLong ? 30 : 18;
        }
        writeEa(0, reg, size, r.value);
        return size == kLong ? 8 : 4;
    }

    if (toMemory)
    {
        u32 addr = 0;
        const u32 dst = readEaForModify(mode, rm, size, addr);
        const alu::Result r = alu::sub(dst, st_.d[reg], size);
        st_.sr = applyFlags(st_.sr, r, true);
        writeEaToAddr(mode, rm, size, addr, r.value);
        return 12;
    }

    const u32 src = readEa(mode, rm, size);
    const alu::Result r = alu::sub(st_.d[reg], src, size);
    st_.sr = applyFlags(st_.sr, r, true);
    writeEa(0, reg, size, r.value);
    return 4;
}

// CMP / CMPA / CMPM / EOR (1011)
u32 M68k::groupCmpEor(u16 op)
{
    const u32 reg = (op >> 9) & 7u;
    const u32 opmode = (op >> 6) & 7u;
    const u32 mode = (op >> 3) & 7u;
    const u32 rm = op & 7u;

    if (opmode == 3 || opmode == 7)
    {
        // CMPA: An と比較。結果は捨ててフラグだけ立てる。
        const u32 size = opmode == 3 ? kWord : kLong;
        const u32 src = readEa(mode, rm, size);
        const u32 value =
            size == kWord ? static_cast<u32>(static_cast<s32>(static_cast<s16>(src))) : src;
        const alu::Result r = alu::sub(st_.a[reg], value, kLong);
        st_.sr = applyFlags(st_.sr, r, false);
        return 6;
    }

    const u32 size = sizeFromOpmode(opmode);

    if ((opmode & 4u) == 0)
    {
        // CMP <ea>,Dn
        const u32 src = readEa(mode, rm, size);
        const alu::Result r = alu::sub(st_.d[reg], src, size);
        st_.sr = applyFlags(st_.sr, r, false);
        return 4;
    }

    if (mode == 1)
    {
        // CMPM (Ay)+,(Ax)+
        const u32 srcAddr = effectiveAddress(3, rm, size);
        const u32 src =
            size == kByte ? read8(srcAddr) : (size == kWord ? read16(srcAddr) : read32(srcAddr));
        const u32 dstAddr = effectiveAddress(3, reg, size);
        const u32 dst =
            size == kByte ? read8(dstAddr) : (size == kWord ? read16(dstAddr) : read32(dstAddr));
        const alu::Result r = alu::sub(dst, src, size);
        st_.sr = applyFlags(st_.sr, r, false);
        return 12;
    }

    // EOR Dn,<ea>
    u32 addr = 0;
    const u32 dst = readEaForModify(mode, rm, size, addr);
    const u32 value = alu::truncate(dst ^ st_.d[reg], size);
    setLogicFlags(value, size);
    writeEaToAddr(mode, rm, size, addr, value);
    return 8;
}

// OR / DIVU / DIVS / SBCD (1000)
u32 M68k::groupOrDiv(u16 op)
{
    const u32 reg = (op >> 9) & 7u;
    const u32 opmode = (op >> 6) & 7u;
    const u32 mode = (op >> 3) & 7u;
    const u32 rm = op & 7u;

    if (opmode == 3 || opmode == 7)
    {
        // DIVU (3) / DIVS (7): 32bit ÷ 16bit → 商 16bit, 余り 16bit
        const u32 divisor16 = readEa(mode, rm, kWord);
        if (divisor16 == 0)
        {
            takeException(vector::kZeroDivide);
            return 38;
        }

        const bool isSigned = opmode == 7;
        const u32 dividend = st_.d[reg];

        s64 quotient = 0;
        s64 remainder = 0;
        if (isSigned)
        {
            const s32 n = static_cast<s32>(dividend);
            const s32 d = static_cast<s16>(divisor16);
            quotient = n / d;
            remainder = n % d;
        }
        else
        {
            const u32 n = dividend;
            const u32 d = divisor16;
            quotient = static_cast<s64>(n / d);
            remainder = static_cast<s64>(n % d);
        }

        // 商が 16bit に収まらない場合は V を立てて結果を書かない。
        const bool overflow =
            isSigned ? (quotient > 32767 || quotient < -32768) : (quotient > 65535);
        if (overflow)
        {
            st_.sr |= sr_bit::kOverflow;
            return 70;
        }

        const u32 q = static_cast<u32>(quotient) & 0xFFFFu;
        const u32 r = static_cast<u32>(remainder) & 0xFFFFu;
        st_.d[reg] = (r << 16) | q;

        u16 sr = static_cast<u16>(
            st_.sr & ~(sr_bit::kNegative | sr_bit::kZero | sr_bit::kOverflow | sr_bit::kCarry));
        if (q == 0)
        {
            sr |= sr_bit::kZero;
        }
        if ((q & 0x8000u) != 0)
        {
            sr |= sr_bit::kNegative;
        }
        st_.sr = sr;
        return 140;
    }

    const u32 size = sizeFromOpmode(opmode);
    const bool toMemory = (opmode & 4u) != 0;

    if (toMemory && mode < 2)
    {
        // SBCD。BCD 演算は Human68k の起動には出てこないので後回し。
        return unimplemented(op);
    }

    if (toMemory)
    {
        u32 addr = 0;
        const u32 dst = readEaForModify(mode, rm, size, addr);
        const u32 value = alu::truncate(dst | st_.d[reg], size);
        setLogicFlags(value, size);
        writeEaToAddr(mode, rm, size, addr, value);
        return 12;
    }

    const u32 src = readEa(mode, rm, size);
    const u32 value = alu::truncate(st_.d[reg] | src, size);
    setLogicFlags(value, size);
    writeEa(0, reg, size, value);
    return 4;
}

// AND / MULU / MULS / ABCD / EXG (1100)
u32 M68k::groupAndMul(u16 op)
{
    const u32 reg = (op >> 9) & 7u;
    const u32 opmode = (op >> 6) & 7u;
    const u32 mode = (op >> 3) & 7u;
    const u32 rm = op & 7u;

    if (opmode == 3 || opmode == 7)
    {
        // MULU (3) / MULS (7): 16bit × 16bit → 32bit
        const u32 src16 = readEa(mode, rm, kWord);
        const bool isSigned = opmode == 7;

        u32 result = 0;
        if (isSigned)
        {
            const s32 a = static_cast<s16>(src16);
            const s32 b = static_cast<s16>(st_.d[reg] & 0xFFFFu);
            result = static_cast<u32>(a * b);
        }
        else
        {
            result = (src16 & 0xFFFFu) * (st_.d[reg] & 0xFFFFu);
        }

        st_.d[reg] = result;
        setLogicFlags(result, kLong);
        return 70;
    }

    const u32 size = sizeFromOpmode(opmode);
    const bool toMemory = (opmode & 4u) != 0;

    if (toMemory && mode < 2)
    {
        // ABCD (mode 0/1) と EXG。opmode で区別する。
        // EXG: opmode 5 (Dx,Dy) / 5 (Ax,Ay) / 6 (Dx,Ay)
        if (opmode == 5 && mode == 0)
        {
            const u32 tmp = st_.d[reg];
            st_.d[reg] = st_.d[rm];
            st_.d[rm] = tmp;
            return 6;
        }
        if (opmode == 5 && mode == 1)
        {
            const u32 tmp = st_.a[reg];
            st_.a[reg] = st_.a[rm];
            st_.a[rm] = tmp;
            return 6;
        }
        if (opmode == 6 && mode == 1)
        {
            const u32 tmp = st_.d[reg];
            st_.d[reg] = st_.a[rm];
            st_.a[rm] = tmp;
            return 6;
        }
        // ABCD は後回し。
        return unimplemented(op);
    }

    if (toMemory)
    {
        u32 addr = 0;
        const u32 dst = readEaForModify(mode, rm, size, addr);
        const u32 value = alu::truncate(dst & st_.d[reg], size);
        setLogicFlags(value, size);
        writeEaToAddr(mode, rm, size, addr, value);
        return 12;
    }

    const u32 src = readEa(mode, rm, size);
    const u32 value = alu::truncate(st_.d[reg] & src, size);
    setLogicFlags(value, size);
    writeEa(0, reg, size, value);
    return 4;
}

}  // namespace x68k
