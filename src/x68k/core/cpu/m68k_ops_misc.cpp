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

// ORI/ANDI/EORI to CCR/SR は演算子だけが違い、対象 (CCR か SR か) が違うだけ。
// opType は即値演算グループのビット 9-11 で、0=ORI / 1=ANDI / 5=EORI。
// 呼び出し側で if/else を組むと「初期値を必ず上書きする」死んだ代入が残るので、
// 演算そのものをここに切り出して値を返す形にしている。
constexpr u16 applyLogicalOp(u32 opType, u16 current, u16 operand)
{
    if (opType == 0)
    {
        return static_cast<u16>(current | operand);
    }
    if (opType == 1)
    {
        return static_cast<u16>(current & operand);
    }
    return static_cast<u16>(current ^ operand);
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

    // MOVEP Dx,(d16,Ay) / (d16,Ay),Dx : 0000 rrr 1mm 001 aaa
    //
    // 動的ビット操作と同じ bit8=1 の空間に入っているが、mode が 1 (An 直接) の
    // ものだけが MOVEP。ビット操作は An を対象に取れないので衝突しない。
    // Why not ビット操作側の後で判定するか: 先に判定しないと BCHG/BSET として
    // 解釈され、An を書き換えてしまう。
    const bool isMovep = isDynamicBitOp && mode == 1;
    if (isMovep)
    {
        const u32 dataReg = (op >> 9) & 7u;
        const u32 opmode = (op >> 6) & 3u;  // 4=w->reg 5=l->reg 6=w->mem 7=l->mem の下位2bit
        const bool isLong = (opmode & 1u) != 0;
        const bool toMemory = (opmode & 2u) != 0;

        // 変位は命令語の直後の拡張ワード。effectiveAddress を使わないのは
        // MOVEP が (d16,An) 形式に固定されていて、他のモードを取らないため。
        const s16 disp = static_cast<s16>(fetch());
        const u32 base = st_.a[reg] + static_cast<u32>(static_cast<s32>(disp));

        // 8bit 幅の周辺デバイス向けに、1 バイトおき (アドレス +2 ずつ) に転送する。
        const u32 byteCount = isLong ? 4u : 2u;
        if (toMemory)
        {
            const u32 value = st_.d[dataReg];
            for (u32 i = 0; i < byteCount; ++i)
            {
                // 上位バイトから順に置く。
                const u32 shift = (byteCount - 1u - i) * 8u;
                write8(base + i * 2u, static_cast<u8>((value >> shift) & 0xFFu));
            }
            return isLong ? 24 : 16;
        }

        u32 value = 0;
        for (u32 i = 0; i < byteCount; ++i)
        {
            value = (value << 8) | read8(base + i * 2u);
        }
        // ワード転送は Dn の下位 16bit だけを差し替える。上位は保存される。
        if (isLong)
        {
            st_.d[dataReg] = value;
        }
        else
        {
            st_.d[dataReg] = (st_.d[dataReg] & 0xFFFF0000u) | (value & 0xFFFFu);
        }
        return isLong ? 24 : 16;
    }

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

        // BTST だけは書き戻しがないので即値 (#) を対象に取れる。
        // Why not readEaForModify に通すか: あれは書き戻し先のアドレスを
        // 確定させる関数で、即値モードを扱えない。BTST #, # のように
        // 「読むだけ」の形式はここで読み切る。
        const bool isImmediateSource = mode == 7 && reg == 4;
        if (bitOp == 0 && isImmediateSource)
        {
            const u32 immediateValue = readEa(mode, reg, size);
            st_.sr = static_cast<u16>(st_.sr & ~sr_bit::kZero);
            if ((immediateValue & mask) == 0)
            {
                st_.sr |= sr_bit::kZero;
            }
            return 4;
        }

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
        // BCHG/BCLR/BSET は書き戻しが入るぶん、レジスタ対象でもメモリ対象でも 8。
        // (BTST だけが 6/4 と分かれる。上の early return を参照)
        return 8;
    }

    const u32 sizeField = (op >> 6) & 3u;
    if (sizeField == 3)
    {
        return unimplemented(op);
    }
    const u32 size = sizeFromField(sizeField);

    // ORI/ANDI/EORI to SR は特権命令。即値を取る「前」に特権を判定する。
    //
    // Why not 即値を取ってから判定するか: 特権違反の例外フレームには「例外を
    // 起こした命令の先頭」を積む必要がある。takeException(faulting=true) は
    // 命令語 1 ワードぶんしか戻さないので、先に fetch() で即値を読んで PC を
    // 進めてしまうと、積まれる PC が 2 バイト先へずれる。
    // 実機も特権違反は即値を読む前に検出する。
    const bool isImmediateToSr =
        mode == 7 && reg == 4 && sizeField == 1 && (opType == 0 || opType == 1 || opType == 5);
    if (isImmediateToSr && !requirePrivilege())
    {
        return 34;
    }

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
            const u16 operand = static_cast<u16>(immediate & sr_bit::kCcrMask);
            const u16 next = applyLogicalOp(opType, ccr, operand);
            st_.sr = static_cast<u16>((st_.sr & ~sr_bit::kCcrMask) | next);
            return 20;
        }
        // to SR の特権判定は即値を読む前に済ませてある (この関数の冒頭を参照)。
        setSr(applyLogicalOp(opType, st_.sr, static_cast<u16>(immediate)));
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
// メモリに対する 1 ビットシフト / ローテート。
//
// レジスタ版と違い、サイズはワード固定、シフト量は 1 固定、対象は実効アドレス。
// 命令語のビット配置も違う (bit 9-10 がシフトの種類、bit 8 が方向)。
//
// Why not groupShift のループを共有するか: 回数が 1 に固定なので
// ループが要らず、フラグの決まり方もレジスタ版の「回数 0 のときの特例」が
// 存在しないぶん単純になる。共有すると分岐だらけになって読みにくい。
u32 M68k::memoryShift(u16 op)
{
    const u32 mode = (op >> 3) & 7u;
    const u32 reg = op & 7u;
    const bool isLeft = (op & 0x0100u) != 0;
    const u32 shiftType = (op >> 9) & 3u;  // 0=AS 1=LS 2=ROX 3=RO

    // データレジスタ直接とアドレスレジスタ直接は取れない形式。
    const bool isRegisterDirect = mode == 0 || mode == 1;
    if (isRegisterDirect)
    {
        return unimplemented(op);
    }

    constexpr u32 kSize = 2;  // ワード固定
    const u32 msb = alu::signBit(kSize);

    u32 addr = 0;
    const u32 before = readEaForModify(mode, reg, kSize, addr);
    u32 value = before;

    const bool extendIn = (st_.sr & sr_bit::kExtend) != 0;
    bool carry = false;
    bool extendOut = extendIn;
    bool overflow = false;

    if (isLeft)
    {
        const bool bitOut = (value & msb) != 0;
        if (shiftType == 3)
        {
            value = alu::truncate((value << 1) | (bitOut ? 1u : 0u), kSize);
        }
        else if (shiftType == 2)
        {
            value = alu::truncate((value << 1) | (extendIn ? 1u : 0u), kSize);
            extendOut = bitOut;
        }
        else
        {
            value = alu::truncate(value << 1, kSize);
            extendOut = bitOut;
        }
        carry = bitOut;
        // ASL は符号ビットが変われば V を立てる。
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
            value = alu::truncate((value >> 1) | (bitOut ? msb : 0u), kSize);
        }
        else if (shiftType == 2)
        {
            value = alu::truncate((value >> 1) | (extendIn ? msb : 0u), kSize);
            extendOut = bitOut;
        }
        else if (shiftType == 0)
        {
            // ASR は符号ビットを保つ。
            const bool sign = (value & msb) != 0;
            value = alu::truncate((value >> 1) | (sign ? msb : 0u), kSize);
            extendOut = bitOut;
        }
        else
        {
            value = alu::truncate(value >> 1, kSize);
            extendOut = bitOut;
        }
        carry = bitOut;
    }

    writeEaToAddr(mode, reg, kSize, addr, value);

    u16 sr = static_cast<u16>(
        st_.sr & ~(sr_bit::kNegative | sr_bit::kZero | sr_bit::kOverflow | sr_bit::kCarry));
    if (value == 0)
    {
        sr |= sr_bit::kZero;
    }
    if (alu::isNegative(value, kSize))
    {
        sr |= sr_bit::kNegative;
    }
    if (overflow)
    {
        sr |= sr_bit::kOverflow;
    }
    if (carry)
    {
        sr |= sr_bit::kCarry;
    }

    // ROL/ROR は X を変えない。
    if (shiftType != 3)
    {
        sr = static_cast<u16>(sr & ~sr_bit::kExtend);
        if (extendOut)
        {
            sr |= sr_bit::kExtend;
        }
    }

    st_.sr = sr;
    return 8;
}

u32 M68k::groupShift(u16 op)
{
    const u32 sizeField = (op >> 6) & 3u;

    if (sizeField == 3)
    {
        return memoryShift(op);
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
