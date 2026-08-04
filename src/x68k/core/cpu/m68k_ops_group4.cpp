// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 0100 グループ。CLR/NEG/NOT/TST/LEA/JMP/JSR/RTS/RTE/MOVEM/SWAP/EXT/LINK/UNLK など、
// 系統の異なる命令が同じ上位 4bit に詰め込まれている。

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

}  // namespace

u32 M68k::groupMisc(u16 op)
{
    const u32 mode = (op >> 3) & 7u;
    const u32 reg = op & 7u;

    // --- 最頻の 2 系統だけ先に振り分ける ------------------------------------
    //
    // このグループ (0100) は全命令の 25.2% を占める最頻グループだが、実装は
    // 固定パターンから順に見る if の直列で末尾まで 26 段ある。命令語ごとに
    // 実測したところ (200M サイクル / groupMisc 到達 5179411 回):
    //
    //   単項演算 (NEGX/CLR/NEG/NOT/TST)  47.4%  <- チェーン最末尾 (26 段目)
    //   MOVEM                            29.6%  <- 22 段目
    //   LEA                              11.7%
    //   JSR                               3.6%
    //   その他                            7.7%
    //
    // **77% が 22 段以上の失敗比較を歩いてから**実体に着いていた。
    //
    // Why not switch へ書き直さないか: この関数は 600 行あり、TAS と ILLEGAL
    // ($4AFC)、EXT.W と NBCD ($4880 / $4800) のように「手前で捕まること」が
    // 正しさの前提になっている組が複数ある。全体を組み替えると、その順序
    // 依存を 1 つ取り落としただけで別命令として実行される。
    //
    // Why not 本体をここへ移動しないか: 移動は差分が大きく、移動漏れや
    // 変数の捕捉ミスが混ざる余地がある。goto で**既存の本体へ飛ぶ**なら、
    // 実行される命令列は今までと 1 命令も変わらない。
    //
    // 先取りしてよいことは op = $4000-$4FFF の全数で確かめた:
    //   opField ∈ {0,2,4,6,A} かつ sizeField != 3 の 960 個は、手前の分岐に
    //   **1 つも捕まらない**。よって飛ばしても到達先は変わらない。
    //   MOVEM ($FB80 マスク) も同じ条件で手前と重ならない。
    {
        const u32 opFieldFast = (op >> 8) & 0xFu;
        const bool isUnaryOpField = opFieldFast == 0x0u || opFieldFast == 0x2u ||
                                    opFieldFast == 0x4u || opFieldFast == 0x6u ||
                                    opFieldFast == 0xAu;
        const bool isUnaryFast = ((op >> 6) & 3u) != 3u && isUnaryOpField;
        if (isUnaryFast)
        {
            goto unary_ops;
        }
        // MOVEM は mode 0 (Dn 直接) を取らない。EXT.W ($4880-$4887) と
        // EXT.L ($48C0-$48C7) がこのマスクに入ってしまい、手前で捕まるべき
        // 16 個をここで奪ってしまうため、mode 0 を外す。
        //
        // 最初に mode の除外を書かずに通したところ、EXT.L D1 ($48C1) が
        // MOVEM として実行されて適合性ベクタが 105 件落ちた。
        const bool isMovemFast = (op & 0xFB80u) == 0x4880u && mode != 0;
        if (isMovemFast)
        {
            goto movem_op;
        }
    }

    // --- 固定パターンの命令から先に判定する ---------------------------------

    if (op == 0x4E71u)  // NOP
    {
        return 4;
    }

    if (op == 0x4E75u)  // RTS
    {
        const u32 addr = read32(st_.a[7]);
        st_.a[7] = st_.a[7] + 4;
        refillPrefetch(addr);
        return 16;
    }

    if (op == 0x4E73u)  // RTE: 特権命令
    {
        if (!requirePrivilege())
        {
            return 34;
        }
        const u16 sr = read16(st_.a[7]);
        st_.a[7] = st_.a[7] + 2;
        const u32 pc = read32(st_.a[7]);
        st_.a[7] = st_.a[7] + 4;
        setSr(sr);
        refillPrefetch(pc);
        return 20;
    }

    if (op == 0x4E77u)  // RTR: CCR と PC を戻す
    {
        const u16 ccr = read16(st_.a[7]);
        st_.a[7] = st_.a[7] + 2;
        const u32 pc = read32(st_.a[7]);
        st_.a[7] = st_.a[7] + 4;
        st_.sr = static_cast<u16>((st_.sr & ~sr_bit::kCcrMask) | (ccr & sr_bit::kCcrMask));
        refillPrefetch(pc);
        return 20;
    }

    if (op == 0x4E70u)  // RESET
    {
        if (!requirePrivilege())
        {
            return 34;
        }
        // 68000 としては RESET 信号を外へ出すだけで CPU 自身は何もしない。
        // ただし X68000 ではメモリコントローラがこれを受けて $000000 の
        // ROM 写像を解除する。IPL-ROM は起動直後 ($FF001A) にこれを実行し、
        // 通常のメモリ配置へ切り替える。
        // ここを何もしないままにすると、以降 RAM を読んでも ROM の値が返り、
        // スタックが読めずサブルーチンから戻れなくなる。
        if (resetCallback_ != nullptr)
        {
            resetCallback_(resetContext_);
        }
        return 132;
    }

    if (op == 0x4E72u)  // STOP #<data>
    {
        if (!requirePrivilege())
        {
            return 34;
        }
        // 即値は step() の fetch() で既に ir へ流れ込んでいる。これを SR に入れ、
        // プリフェッチを命令語の位置へ巻き戻して停止する。
        //
        // Why not ここで fetch() を呼ばないか: 実機の STOP は停止と同時に
        // プリフェッチも止めるので、PC・ir・irc は STOP の命令語を指したまま
        // 動かない (テストベクタの最終状態が初期状態と同一なのはこのため)。
        // fetch() を足すと PC がさらに 2 バイト進み、ir/irc も先の内容で潰れる。
        //
        // 巻き戻し先は「STOP の命令語」。step() の fetch() を通った直後の pc は
        // 命令語 + 6 (命令語 + 即値 + 先読み 2 ワード) なので、6 引くと戻る。
        const u16 immediate = st_.ir;
        refillPrefetch(st_.pc - 6);
        setSr(immediate);
        st_.stopped = true;
        return 4;
    }

    if (op == 0x4E76u)  // TRAPV
    {
        if ((st_.sr & sr_bit::kOverflow) != 0)
        {
            takeException(vector::kTrapv);
            return 34;
        }
        return 4;
    }

    if ((op & 0xFFF0u) == 0x4E40u)  // TRAP #n
    {
        takeException(vector::kTrapBase + (op & 0xFu));
        return 38;
    }

    if ((op & 0xFFF8u) == 0x4E50u)  // LINK An,#<disp>
    {
        const s16 disp = static_cast<s16>(fetch());
        // LINK A7 では An と A7 が同じレジスタになる。
        // 積むのは「デクリメントする前の A7」なので、先に控えを取る。
        // ここを st_.a[reg] のまま読むと、デクリメント後の値を積んでしまう。
        const u32 pushed = st_.a[reg];
        st_.a[7] = st_.a[7] - 4;
        write32(st_.a[7], pushed);
        st_.a[reg] = st_.a[7];
        st_.a[7] = st_.a[7] + static_cast<u32>(static_cast<s32>(disp));
        return 16;
    }

    if ((op & 0xFFF8u) == 0x4E58u)  // UNLK An
    {
        // 手順は「A7 ← An」「An ← (A7)」「A7 ← A7 + 4」。
        //
        // UNLK A7 では An と A7 が同じレジスタなので、最後の +4 が
        // 読み戻した値に対して行われるのではなく、読み戻した値がそのまま
        // 最終的な A7 になる (An への書き込みが後から A7 を上書きするため)。
        // 順番どおりに素直に書けば両方のケースが正しく処理される。
        st_.a[7] = st_.a[reg];
        const u32 restored = read32(st_.a[7]);
        st_.a[7] = st_.a[7] + 4;
        st_.a[reg] = restored;
        return 12;
    }

    if ((op & 0xFFF8u) == 0x4E60u)  // MOVE An,USP
    {
        if (!requirePrivilege())
        {
            return 34;
        }
        st_.usp = st_.a[reg];
        return 4;
    }

    if ((op & 0xFFF8u) == 0x4E68u)  // MOVE USP,An
    {
        if (!requirePrivilege())
        {
            return 34;
        }
        st_.a[reg] = st_.usp;
        return 4;
    }

    if ((op & 0xFFF8u) == 0x4840u)  // SWAP Dn
    {
        const u32 value = (st_.d[reg] >> 16) | (st_.d[reg] << 16);
        st_.d[reg] = value;
        setLogicFlags(value, kLong);
        return 4;
    }

    if ((op & 0xFFF8u) == 0x4880u)  // EXT.W Dn: バイト → ワード
    {
        const u32 value = static_cast<u32>(static_cast<s32>(static_cast<s8>(st_.d[reg] & 0xFFu)));
        st_.d[reg] = (st_.d[reg] & 0xFFFF0000u) | (value & 0xFFFFu);
        setLogicFlags(value, kWord);
        return 4;
    }

    if ((op & 0xFFF8u) == 0x48C0u)  // EXT.L Dn: ワード → ロング
    {
        const u32 value =
            static_cast<u32>(static_cast<s32>(static_cast<s16>(st_.d[reg] & 0xFFFFu)));
        st_.d[reg] = value;
        setLogicFlags(value, kLong);
        return 4;
    }

    // NBCD <ea> : 0100 1000 00 mmm rrr。バイト固定の 10 進符号反転 (0 - dst - X)。
    // EXT.W (0x4880) は上で処理済みなので、ここに来る 0x4800 系は NBCD だけ。
    if ((op & 0xFFC0u) == 0x4800u)
    {
        u32 addr = 0;
        const u32 dst = readEaForModify(mode, reg, kByte, addr);
        const bool extend = (st_.sr & sr_bit::kExtend) != 0;
        const alu::Result r = alu::bcdSub(0, dst, extend);

        u16 sr = static_cast<u16>(
            st_.sr & ~(sr_bit::kNegative | sr_bit::kOverflow | sr_bit::kCarry | sr_bit::kExtend));
        if (r.n)
        {
            sr |= sr_bit::kNegative;
        }
        if (r.v)
        {
            sr |= sr_bit::kOverflow;
        }
        if (r.c)
        {
            sr |= sr_bit::kCarry | sr_bit::kExtend;
        }
        // Z は累積。結果が非ゼロのときだけ落とし、ゼロなら前の値を保つ。
        if (r.value != 0)
        {
            sr = static_cast<u16>(sr & ~sr_bit::kZero);
        }
        st_.sr = sr;

        writeEaToAddr(mode, reg, kByte, addr, r.value);
        return 6;
    }

    // --- パターンで分ける命令 -----------------------------------------------

    // LEA <ea>,An : op = 0100 rrr 111 mmm rrr
    if ((op & 0x01C0u) == 0x01C0u)
    {
        const u32 dstReg = (op >> 9) & 7u;
        // LEA はアドレスそのものを取る。読み出しは発生しない。
        const u32 addr = effectiveAddress(mode, reg, kLong);
        if (st_.halted)
        {
            return 0;
        }
        st_.a[dstReg] = addr;
        return 4;
    }

    // CHK <ea>,Dn : op = 0100 rrr 110 mmm rrr
    if ((op & 0x01C0u) == 0x0180u)
    {
        const u32 dstReg = (op >> 9) & 7u;
        const s16 bound = static_cast<s16>(readEa(mode, reg, kWord));
        const s16 value = static_cast<s16>(st_.d[dstReg] & 0xFFFFu);

        // CHK は Z/V/C を必ずクリアし、N には「範囲外だった向き」を残す。
        // 下側 (負) で外れたら N=1、上側で外れたら N=0。範囲内なら N=0。
        //
        // Why not Z を「値がゼロか」で作るか: 適合性ベクタの範囲内ケース
        // (89/89) はすべて Z=0 で、値がゼロでも Z は立たない。
        const bool isBelowZero = value < 0;
        const bool isAboveBound = value > bound;
        u16 sr = static_cast<u16>(
            st_.sr & ~(sr_bit::kNegative | sr_bit::kZero | sr_bit::kOverflow | sr_bit::kCarry));
        if (isBelowZero)
        {
            sr |= sr_bit::kNegative;
        }
        st_.sr = sr;

        if (isBelowZero || isAboveBound)
        {
            takeException(vector::kChk);
            return 40;
        }
        return 10;
    }

    // JMP / JSR : 0100 1110 1 x mmm rrr
    if ((op & 0xFFC0u) == 0x4EC0u)  // JMP
    {
        const u32 addr = effectiveAddress(mode, reg, kLong);
        if (st_.halted)
        {
            return 0;
        }
        refillPrefetch(addr);
        return 8;
    }

    if ((op & 0xFFC0u) == 0x4E80u)  // JSR
    {
        const u32 addr = effectiveAddress(mode, reg, kLong);
        if (st_.halted)
        {
            return 0;
        }
        // 戻り先は「この命令の次のアドレス」。
        //
        // PC の位置関係: プリフェッチにより PC は常に「読み込み済みの最後のワードの
        // 次」を指す。effectiveAddress が拡張ワードを消費した後もこの関係は保たれ、
        // 命令の終端は pc - 4 になる (プリフェッチ 2 ワードぶん先を見ているため)。
        const u32 returnAddr = st_.pc - 4;
        st_.a[7] = st_.a[7] - 4;
        write32(st_.a[7], returnAddr);
        refillPrefetch(addr);
        return 16;
    }

    // PEA <ea> : 0100 1000 01 mmm rrr
    if ((op & 0xFFC0u) == 0x4840u)
    {
        const u32 addr = effectiveAddress(mode, reg, kLong);
        if (st_.halted)
        {
            return 0;
        }
        st_.a[7] = st_.a[7] - 4;
        write32(st_.a[7], addr);
        return 12;
    }

    // MOVEM : 0100 1d00 1s mmm rrr
movem_op:
    if ((op & 0xFB80u) == 0x4880u)
    {
        const bool memoryToRegister = (op & 0x0400u) != 0;
        const u32 size = (op & 0x0040u) != 0 ? kLong : kWord;
        const u16 mask = fetch();

        u32 count = 0;

        if (!memoryToRegister && mode == 4)
        {
            // -(An) 形式はレジスタ順が逆 (A7 から D0 へ) になる。
            // ここを間違えるとスタックフレームが壊れ、原因が非常に追いにくい。
            u32 addr = st_.a[reg];
            for (u32 i = 0; i < 16; ++i)
            {
                if ((mask & (1u << i)) == 0)
                {
                    continue;
                }
                // ビット i は「A7 から数えて i 番目」に対応する。
                const u32 regIndex = 15u - i;
                const u32 value = regIndex < 8 ? st_.d[regIndex] : st_.a[regIndex - 8];
                addr = addr - size;
                if (size == kWord)
                {
                    write16(addr, static_cast<u16>(value));
                }
                else
                {
                    write32(addr, value);
                }
                ++count;
            }
            st_.a[reg] = addr;
            return 8 + count * (size == kWord ? 4 : 8);
        }

        u32 addr = 0;
        if (memoryToRegister && mode == 3)
        {
            addr = st_.a[reg];
        }
        else
        {
            addr = effectiveAddress(mode, reg, size);
            if (st_.halted)
            {
                return 0;
            }
        }

        for (u32 i = 0; i < 16; ++i)
        {
            if ((mask & (1u << i)) == 0)
            {
                continue;
            }
            if (memoryToRegister)
            {
                const u32 value =
                    size == kWord
                        ? static_cast<u32>(static_cast<s32>(static_cast<s16>(read16(addr))))
                        : read32(addr);
                if (i < 8)
                {
                    st_.d[i] = value;
                }
                else
                {
                    st_.a[i - 8] = value;
                }
            }
            else
            {
                const u32 value = i < 8 ? st_.d[i] : st_.a[i - 8];
                if (size == kWord)
                {
                    write16(addr, static_cast<u16>(value));
                }
                else
                {
                    write32(addr, value);
                }
            }
            addr = addr + size;
            ++count;
        }

        if (memoryToRegister && mode == 3)
        {
            // (An)+ 形式は読み終わった位置までポインタを進める。
            st_.a[reg] = addr;
        }
        return 12 + count * (size == kWord ? 4 : 8);
    }

    // MOVE from SR : 0100 0000 11 mmm rrr
    if ((op & 0xFFC0u) == 0x40C0u)
    {
        u32 addr = 0;
        readEaForModify(mode, reg, kWord, addr);
        writeEaToAddr(mode, reg, kWord, addr, st_.sr);
        return 6;
    }

    // MOVE to CCR : 0100 0100 11 mmm rrr
    if ((op & 0xFFC0u) == 0x44C0u)
    {
        const u32 value = readEa(mode, reg, kWord);
        st_.sr =
            static_cast<u16>((st_.sr & clearMask(sr_bit::kCcrMask)) | (value & sr_bit::kCcrMask));
        return 12;
    }

    // MOVE to SR : 0100 0110 11 mmm rrr (特権)
    if ((op & 0xFFC0u) == 0x46C0u)
    {
        if (!requirePrivilege())
        {
            return 34;
        }
        const u32 value = readEa(mode, reg, kWord);
        setSr(static_cast<u16>(value));
        return 12;
    }

    // TAS: 0100 1010 11 mmm rrr
    //
    // バイトを読んで N/Z を立て、bit7 を立てて書き戻す。サイズ欄が 3 の
    // 位置に居るので、下の単項演算 (サイズ欄 0-2) とは別に先に捌く。
    //
    // Why not 単項演算の switch に混ぜないか: あちらは sizeField から
    // 演算サイズを決める形になっている。TAS は sizeField=3 を「サイズ」
    // ではなく命令の識別に使うので、同じ枠に入れると sizeFromField(3) が
    // 意味を持たない値を返す経路ができる。
    //
    // Why not 実機の 5 サイクル read-modify-write を再現しないか:
    // バスを占有したまま読んで書く点が実機との違いだが、本エミュレータは
    // バスマスタが CPU と DMAC しかなく、DMAC の転送は命令境界でしか
    // 進まない。分割不可能性が問題になる場面が無い。
    // (upstream のテストベクタもこのタイミングは未実装と明記している)
    // $4AFC (mode 7 / reg 4) だけは TAS ではなく ILLEGAL 命令。
    // 実効アドレスとして意味を持たない組み合わせをその 1 つに割り当てて
    // あるので、ここで先に弾かないと ILLEGAL を TAS として実行してしまう。
    if (op == 0x4AFCu)
    {
        // ILLEGAL。実機は不当命令例外 (ベクタ 4) を起こす。
        //
        // Why not 下の unary_ops へ流さないか: $4AFC は sizeField=3 なので
        // 単項演算の条件に入らず、末尾の unimplemented() まで落ちる。
        // あれは「まだ実装していない命令に当たった」ことを開発者へ知らせる
        // ための停止で、実機には無い状態。ゲストが意図して ILLEGAL を
        // 置いた場合 (デバッガのブレークポイント等) にエミュレータごと
        // 止まってしまう。
        //
        // 積む PC は命令そのもの (faulting = true)。RTE で戻ると同じ
        // 命令を再実行するのが実機の振る舞い。
        takeException(vector::kIllegalInstruction, true);
        return 34;
    }
    {
        const bool isTas = (op & 0xFFC0u) == 0x4AC0u;
        if (!isTas)
        {
            goto unary_ops;
        }
    }
    {
        u32 addr = 0;
        const u32 value = readEaForModify(mode, reg, kByte, addr);
        setLogicFlags(value, kByte);
        writeEaToAddr(mode, reg, kByte, addr, value | 0x80u);
        // データレジスタ直接なら 4、メモリなら 14 サイクル。
        return mode == 0 ? 4 : 14;
    }

    // 単項演算: NEGX/CLR/NEG/NOT/TST : 0100 ooo 0 ss mmm rrr
unary_ops:
    if (((op >> 6) & 3u) != 3)
    {
        const u32 size = sizeFromField((op >> 6) & 3u);

        switch ((op >> 8) & 0xFu)
        {
            case 0x0:  // NEGX
            {
                u32 addr = 0;
                const u32 dst = readEaForModify(mode, reg, size, addr);
                const u32 x = (st_.sr & sr_bit::kExtend) != 0 ? 1u : 0u;
                const alu::Result first = alu::sub(0, dst, size);
                const alu::Result second = alu::sub(first.value, x, size);
                u16 sr = static_cast<u16>(st_.sr & ~(sr_bit::kNegative | sr_bit::kOverflow |
                                                     sr_bit::kCarry | sr_bit::kExtend));
                if (alu::isNegative(second.value, size))
                {
                    sr |= sr_bit::kNegative;
                }
                if (first.v || second.v)
                {
                    sr |= sr_bit::kOverflow;
                }
                if (first.c || second.c)
                {
                    sr |= sr_bit::kCarry | sr_bit::kExtend;
                }
                // Z は累積 (ADDX/SUBX と同じ規則)。
                if (second.value != 0)
                {
                    sr = static_cast<u16>(sr & ~sr_bit::kZero);
                }
                st_.sr = sr;
                writeEaToAddr(mode, reg, size, addr, second.value);
                return 8;
            }

            case 0x2:  // CLR: 0 を書く。読み出しは行われる (RMW)
            {
                u32 addr = 0;
                readEaForModify(mode, reg, size, addr);
                writeEaToAddr(mode, reg, size, addr, 0);
                st_.sr = static_cast<u16>(
                    (st_.sr & ~(sr_bit::kNegative | sr_bit::kOverflow | sr_bit::kCarry)) |
                    sr_bit::kZero);
                return 6;
            }

            case 0x4:  // NEG
            {
                u32 addr = 0;
                const u32 dst = readEaForModify(mode, reg, size, addr);
                const alu::Result r = alu::sub(0, dst, size);
                u16 sr = static_cast<u16>(st_.sr &
                                          ~(sr_bit::kNegative | sr_bit::kZero | sr_bit::kOverflow |
                                            sr_bit::kCarry | sr_bit::kExtend));
                if (r.n)
                {
                    sr |= sr_bit::kNegative;
                }
                if (r.z)
                {
                    sr |= sr_bit::kZero;
                }
                if (r.v)
                {
                    sr |= sr_bit::kOverflow;
                }
                if (r.c)
                {
                    sr |= sr_bit::kCarry | sr_bit::kExtend;
                }
                st_.sr = sr;
                writeEaToAddr(mode, reg, size, addr, r.value);
                return 6;
            }

            case 0x6:  // NOT
            {
                u32 addr = 0;
                const u32 dst = readEaForModify(mode, reg, size, addr);
                const u32 value = alu::truncate(~dst, size);
                setLogicFlags(value, size);
                writeEaToAddr(mode, reg, size, addr, value);
                return 6;
            }

            case 0xA:  // TST
            {
                const u32 value = readEa(mode, reg, size);
                setLogicFlags(value, size);
                return 4;
            }

            default:
                break;
        }
    }

    return unimplemented(op);
}

}  // namespace x68k
