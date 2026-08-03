// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MOVE / MOVEQ / 分岐命令。実行頻度が最も高いグループ。

#include "m68k.h"
#include "m68k_alu.h"

namespace x68k
{

using alu::kByte;
using alu::kLong;
using alu::kWord;

// MOVE <ea>,<ea> と MOVEA <ea>,An。
//
// 命令語のビット配置が独特で、転送先が「レジスタ番号・モード」の順に
// 並んでいる (転送元は通常どおりモード・レジスタ番号の順)。
u32 M68k::groupMove(u16 op, u32 size)
{
    const u32 srcMode = (op >> 3) & 7u;
    const u32 srcReg = op & 7u;
    const u32 dstReg = (op >> 9) & 7u;
    const u32 dstMode = (op >> 6) & 7u;

    const u32 value = readEa(srcMode, srcReg, size);
    if (st_.halted)
    {
        return 0;
    }

    if (dstMode == 1)
    {
        // MOVEA はフラグを変えない。ワードサイズなら符号拡張して 32bit で入る。
        st_.a[dstReg] =
            size == kWord ? static_cast<u32>(static_cast<s32>(static_cast<s16>(value))) : value;
        return 4;
    }

    writeEa(dstMode, dstReg, size, value);
    setLogicFlags(value, size);
    return 4;
}

// MOVEQ #<data>,Dn
// 8bit の即値を符号拡張して 32bit で Dn に入れる。
u32 M68k::groupMoveq(u16 op)
{
    // bit8 が 1 の符号は MOVEQ ではない (68000 には該当命令が無い)。
    if ((op & 0x0100u) != 0)
    {
        takeException(vector::kIllegalInstruction, true);
        return 34;
    }

    const u32 reg = (op >> 9) & 7u;
    const u32 value = static_cast<u32>(static_cast<s32>(static_cast<s8>(op & 0xFFu)));
    st_.d[reg] = value;
    setLogicFlags(value, kLong);
    return 4;
}

// Bcc / BRA / BSR
u32 M68k::groupBranch(u16 op)
{
    const u32 cond = (op >> 8) & 0xFu;
    const s8 disp8 = static_cast<s8>(op & 0xFFu);

    // 分岐先 = 「命令語の次のワードのアドレス」+ ディスプレースメント。
    //
    // PC の位置関係: step() が命令語を fetch() した時点で PC は 1 ワード進む。
    // 元々 PC は irc の次を指していた (= 命令語 + 4) ので、ここでは命令語 + 6。
    // よって命令語の次のワードは pc - 4 にある。
    const u32 base = st_.pc - 4;

    u32 target = 0;
    u32 returnAddr = 0;
    if (disp8 == 0)
    {
        // 16bit ディスプレースメント形式。拡張ワードが base の位置にあり、
        // 分岐先はその位置からの相対。命令全体は 4 バイト。
        const s16 disp16 = static_cast<s16>(fetch());
        target = base + static_cast<u32>(static_cast<s32>(disp16));
        returnAddr = base + 2;
    }
    else
    {
        // 8bit 形式は命令が 2 バイトで完結する。分岐先の基準は同じく base。
        target = base + static_cast<u32>(static_cast<s32>(disp8));
        returnAddr = base;
    }

    if (cond == 1)
    {
        // BSR: 戻り先を積んでから飛ぶ。
        st_.a[7] = st_.a[7] - 4;
        write32(st_.a[7], returnAddr);
        refillPrefetch(target);
        return 18;
    }

    // cond == 0 は BRA (常に真)。
    if (testCondition(cond))
    {
        refillPrefetch(target);
        return 10;
    }

    return disp8 == 0 ? 12 : 8;
}

}  // namespace x68k
