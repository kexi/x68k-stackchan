// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 即値演算 (0000)、その他 (0100)、クイック演算 (0101)、シフト (1110)。

#include "m68k.h"
#include "m68k_alu.h"

namespace x68k
{
namespace
{

using alu::kByte;
using alu::kLong;
using alu::kWord;

constexpr u32 sizeFromField(u32 field)
{
    if (field == 0)
    {
        return kByte;
    }
    if (field == 1)
    {
        return kWord;
    }
    return kLong;
}

u16 applyResultFlags(u16 sr, const alu::Result& r, bool writeExtend)
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

}  // namespace

// 0000: ORI/ANDI/SUBI/ADDI/EORI/CMPI と、ビット操作 (BTST/BCHG/BCLR/BSET)。
u32 M68k::groupImmediate(u16 op)
{
    const u32 mode = (op >> 3) & 7u;
    const u32 reg = op & 7u;

    // ビット操作の動的形式 (Dn をビット番号に使う) は bit8 が 1。
    const bool isDynamicBitOp = (op & 0x0100u) != 0;
    const u32 opType = (op >> 9) & 7u;

    if (isDynamicBitOp || opType == 4)
    {
        // BTST/BCHG/BCLR/BSET。対象が Dn なら 32bit、メモリなら 8bit で回る。
        u32 bitNumber = 0;
        if (isDynamicBitOp)
        {
            bitNumber = st_.d[(op >> 9) & 7u];
        }
        else
        {
            bitNumber = fetch() & 0xFFu;
        }

        const u32 bitOp = (op >> 6) & 3u;  // 0=BTST 1=BCHG 2=BCLR 3=BSET
        const bool targetIsRegister = mode == 0;
        const u32 size = targetIsRegister ? kLong : kByte;
        bitNumber &= targetIsRegister ? 31u : 7u;
        const u32 mask = 1u << bitNumber;

        u32 addr = 0;
        const u32 value = readEaForModify(mode, reg, size, addr);

        // Z は「テストしたビットが 0 だったか」を表す。他のフラグは変わらない。
        st_.sr = static_cast<u16>(st_.sr & ~sr_bit::kZero);
        if ((value & mask) == 0)
        {
            st_.sr |= sr_bit::kZero;
        }

        if (bitOp == 0)
        {
            return targetIsRegister ? 6 : 4;
        }

        u32 next = value;
        if (bitOp == 1)
        {
            next ^= mask;
        }
        else if (bitOp == 2)
        {
            next &= ~mask;
        }
        else
        {
            next |= mask;
        }
        writeEaToAddr(mode, reg, size, addr, next);
        return targetIsRegister ? 8 : 8;
    }

    const u32 sizeField = (op >> 6) & 3u;
    if (sizeField == 3)
    {
        return unimplemented(op);
    }
    const u32 size = sizeFromField(sizeField);

    // 即値を先に取る。命令語の直後に置かれている。
    u32 immediate = 0;
    if (size == kLong)
    {
        const u32 hi = fetch();
        const u32 lo = fetch();
        immediate = (hi << 16) | lo;
    }
    else
    {
        const u32 v = fetch();
        immediate = size == kByte ? (v & 0xFFu) : v;
    }

    // ORI/ANDI/EORI to CCR/SR は実効アドレスが #immediate (mode 7, reg 4)。
    if (mode == 7 && reg == 4 && (opType == 0 || opType == 1 || opType == 5))
    {
        if (size == kByte)
        {
            // to CCR
            const u16 ccr = static_cast<u16>(st_.sr & sr_bit::kCcrMask);
            u16 next = ccr;
            if (opType == 0)
            {
                next = static_cast<u16>(ccr | (immediate & sr_bit::kCcrMask));
            }
            else if (opType == 1)
            {
                next = static_cast<u16>(ccr & (immediate & sr_bit::kCcrMask));
            }
            else
            {
                next = static_cast<u16>(ccr ^ (immediate & sr_bit::kCcrMask));
            }
            st_.sr = static_cast<u16>((st_.sr & ~sr_bit::kCcrMask) | next);
            return 20;
        }
        // to SR は特権命令。
        if (!requirePrivilege())
        {
            return 34;
        }
        u16 next = st_.sr;
        if (opType == 0)
        {
            next = static_cast<u16>(st_.sr | immediate);
        }
        else if (opType == 1)
        {
            next = static_cast<u16>(st_.sr & immediate);
        }
        else
        {
            next = static_cast<u16>(st_.sr ^ immediate);
        }
        setSr(next);
        return 20;
    }

    u32 addr = 0;
    const u32 dst = readEaForModify(mode, reg, size, addr);

    switch (opType)
    {
        case 0:  // ORI
        {
            const u32 value = alu::truncate(dst | immediate, size);
            setLogicFlags(value, size);
            writeEaToAddr(mode, reg, size, addr, value);
            return 8;
        }
        case 1:  // ANDI
        {
            const u32 value = alu::truncate(dst & immediate, size);
            setLogicFlags(value, size);
            writeEaToAddr(mode, reg, size, addr, value);
            return 8;
        }
        case 2:  // SUBI
        {
            const alu::Result r = alu::sub(dst, immediate, size);
            st_.sr = applyResultFlags(st_.sr, r, true);
            writeEaToAddr(mode, reg, size, addr, r.value);
            return 8;
        }
        case 3:  // ADDI
        {
            const alu::Result r = alu::add(dst, immediate, size);
            st_.sr = applyResultFlags(st_.sr, r, true);
            writeEaToAddr(mode, reg, size, addr, r.value);
            return 8;
        }
        case 5:  // EORI
        {
            const u32 value = alu::truncate(dst ^ immediate, size);
            setLogicFlags(value, size);
            writeEaToAddr(mode, reg, size, addr, value);
            return 8;
        }
        case 6:  // CMPI: 結果を書かずフラグだけ
        {
            const alu::Result r = alu::sub(dst, immediate, size);
            st_.sr = applyResultFlags(st_.sr, r, false);
            return 8;
        }
        default:
            return unimplemented(op);
    }
}

// 0101: ADDQ/SUBQ/Scc/DBcc
u32 M68k::groupQuickAlu(u16 op)
{
    const u32 sizeField = (op >> 6) & 3u;
    const u32 mode = (op >> 3) & 7u;
    const u32 reg = op & 7u;

    if (sizeField == 3)
    {
        // Scc または DBcc。
        const u32 cond = (op >> 8) & 0xFu;
        if (mode == 1)
        {
            // DBcc: 条件が偽ならカウンタを減らし、-1 でなければ分岐。
            const s16 disp = static_cast<s16>(fetch());
            if (testCondition(cond))
            {
                return 12;
            }
            const u32 counter = (st_.d[reg] & 0xFFFFu);
            const u32 next = (counter - 1) & 0xFFFFu;
            st_.d[reg] = (st_.d[reg] & 0xFFFF0000u) | next;
            if (next == 0xFFFFu)
            {
                // カウンタが尽きた。ループを抜ける。
                return 14;
            }
            // 分岐先の基準はディスプレースメントワード自身のアドレス。
            // 命令語の fetch (step) と拡張ワードの fetch (上) で 2 ワード進んでいるので、
            // そのワードは pc - 6 にある。
            const u32 base = st_.pc - 6;
            refillPrefetch(base + static_cast<u32>(static_cast<s32>(disp)));
            return 10;
        }

        // Scc: 条件が真なら $FF、偽なら $00 を書く。
        const u32 value = testCondition(cond) ? 0xFFu : 0x00u;
        u32 addr = 0;
        readEaForModify(mode, reg, kByte, addr);
        writeEaToAddr(mode, reg, kByte, addr, value);
        return 4;
    }

    const u32 size = sizeFromField(sizeField);
    // データフィールドの 0 は 8 を意味する。
    u32 data = (op >> 9) & 7u;
    if (data == 0)
    {
        data = 8;
    }
    const bool isSub = (op & 0x0100u) != 0;

    if (mode == 1)
    {
        // An に対する ADDQ/SUBQ はフラグを変えず、常に 32bit で作用する。
        st_.a[reg] = isSub ? (st_.a[reg] - data) : (st_.a[reg] + data);
        return 8;
    }

    u32 addr = 0;
    const u32 dst = readEaForModify(mode, reg, size, addr);
    const alu::Result r = isSub ? alu::sub(dst, data, size) : alu::add(dst, data, size);
    st_.sr = applyResultFlags(st_.sr, r, true);
    writeEaToAddr(mode, reg, size, addr, r.value);
    return 8;
}

// 1110: シフトとローテート
u32 M68k::groupShift(u16 op)
{
    const u32 sizeField = (op >> 6) & 3u;

    if (sizeField == 3)
    {
        // メモリに対する 1bit シフト。Human68k の起動には出てこないので後回し。
        return unimplemented(op);
    }

    const u32 size = sizeFromField(sizeField);
    const u32 reg = op & 7u;
    const bool isLeft = (op & 0x0100u) != 0;
    const u32 shiftType = (op >> 3) & 3u;  // 0=AS 1=LS 2=ROX 3=RO
    const bool countInRegister = (op & 0x0020u) != 0;

    u32 count = 0;
    if (countInRegister)
    {
        // レジスタ指定のシフト量は 64 で剰余を取る。
        count = st_.d[(op >> 9) & 7u] % 64u;
    }
    else
    {
        count = (op >> 9) & 7u;
        if (count == 0)
        {
            count = 8;
        }
    }

    u32 value = alu::truncate(st_.d[reg], size);
    const u32 bits = size * 8;
    const u32 msb = alu::signBit(size);

    bool carry = false;
    bool overflow = false;
    const bool extendIn = (st_.sr & sr_bit::kExtend) != 0;
    bool extendOut = extendIn;

    for (u32 i = 0; i < count; ++i)
    {
        if (isLeft)
        {
            const bool bitOut = (value & msb) != 0;
            const u32 before = value;
            if (shiftType == 3)
            {
                // ROL: 押し出したビットが下位へ回る
                value = alu::truncate((value << 1) | (bitOut ? 1u : 0u), size);
            }
            else if (shiftType == 2)
            {
                // ROXL: X を経由して回る
                value = alu::truncate((value << 1) | (extendOut ? 1u : 0u), size);
                extendOut = bitOut;
            }
            else
            {
                value = alu::truncate(value << 1, size);
                if (shiftType != 3)
                {
                    extendOut = bitOut;
                }
            }
            carry = bitOut;
            // ASL は符号ビットが変化したら V を立てる。
            if (shiftType == 0 && ((before ^ value) & msb) != 0)
            {
                overflow = true;
            }
        }
        else
        {
            const bool bitOut = (value & 1u) != 0;
            if (shiftType == 3)
            {
                // ROR
                value = alu::truncate((value >> 1) | (bitOut ? msb : 0u), size);
            }
            else if (shiftType == 2)
            {
                // ROXR
                value = alu::truncate((value >> 1) | (extendOut ? msb : 0u), size);
                extendOut = bitOut;
            }
            else if (shiftType == 0)
            {
                // ASR: 符号ビットを保つ
                const bool sign = (value & msb) != 0;
                value = alu::truncate((value >> 1) | (sign ? msb : 0u), size);
                extendOut = bitOut;
            }
            else
            {
                // LSR
                value = alu::truncate(value >> 1, size);
                extendOut = bitOut;
            }
            carry = bitOut;
        }
    }

    writeEa(0, reg, size, value);

    u16 sr = static_cast<u16>(
        st_.sr & ~(sr_bit::kNegative | sr_bit::kZero | sr_bit::kOverflow | sr_bit::kCarry));
    if (value == 0)
    {
        sr |= sr_bit::kZero;
    }
    if (alu::isNegative(value, size))
    {
        sr |= sr_bit::kNegative;
    }
    if (overflow)
    {
        sr |= sr_bit::kOverflow;
    }
    // シフト量が 0 のとき C はクリアされる (ROX 系は X の値が入る)。
    if (count != 0 && carry)
    {
        sr |= sr_bit::kCarry;
    }
    else if (count == 0 && shiftType == 2 && extendIn)
    {
        sr |= sr_bit::kCarry;
    }

    // ROL/ROR は X を変えない。
    if (shiftType != 3)
    {
        sr = static_cast<u16>(sr & ~sr_bit::kExtend);
        if (count != 0 ? extendOut : extendIn)
        {
            sr |= sr_bit::kExtend;
        }
    }
    else
    {
        sr =
            static_cast<u16>((sr & clearMask(sr_bit::kExtend)) | (extendIn ? sr_bit::kExtend : 0u));
    }

    st_.sr = sr;
    (void)bits;
    return 6 + 2 * count;
}

}  // namespace x68k
