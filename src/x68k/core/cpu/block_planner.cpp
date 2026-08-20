// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// BlockPlanner の本体。純関数だけを置く。

#include "block_planner.h"

#include "code_gen_map.h"
#include "m68k_alu.h"
#include "m68k_length.h"

namespace x68k
{
namespace
{

using alu::kByte;
using alu::kLong;
using alu::kWord;

// MOVE の符号化 ($1/$2/$3) からバイト数を得る。
//
// **$2 が long で $3 が word。** グループ番号の順とサイズの順が一致しない
// のは 68000 の符号化の都合で、ここを素直に並べると MOVE.l と MOVE.w が
// 入れ替わる。m68k_length.h の同じ変換とそろえてある。
constexpr u32 moveSizeFromGroup(u32 group)
{
    if (group == 0x1u)
    {
        return kByte;
    }
    return group == 0x2u ? kLong : kWord;
}

// ALU 群の opmode 下位 2bit からバイト数を得る。m68k_ops_alu.cpp の
// sizeFromOpmode と同じ値を返す。
constexpr u32 aluSizeFromOpmode(u32 opmode)
{
    const u32 bits = opmode & 3u;
    if (bits == 0)
    {
        return kByte;
    }
    return bits == 1 ? kWord : kLong;
}

// $8/$9/$B/$C/$D のグループ番号から演算種別を得る。
// 呼び出し側が「読み出し方向 (opmode < 4)」を確かめてから使うこと。
// EOR は $B のメモリ方向にしか存在しないので、ここでは扱わない。
bool aluOpFromGroupRead(u32 group, PlanAluOp& out)
{
    switch (group)
    {
        case 0x8u:
            out = PlanAluOp::kOr;
            return true;
        case 0x9u:
            out = PlanAluOp::kSub;
            return true;
        case 0xBu:
            out = PlanAluOp::kCmp;
            return true;
        case 0xCu:
            out = PlanAluOp::kAnd;
            return true;
        case 0xDu:
            out = PlanAluOp::kAdd;
            return true;
        default:
            return false;
    }
}

// I10: 読み形として受けてよい EA か。受けるなら eaMode を返す。
//
// **許可リスト。** mode 6 (d8,An,Xn) は拡張ワードの解釈が要り、7.2/7.3 は
// PC 相対なので「翻訳時の PC」に依存する。7.4 (即値) はメモリを読まないので
// Tier A の kMoveImmToDreg が既に持っている。どれもここでは受けない。
//
// mode 3/4 ((An)+ / -(An)) は An を動かす副作用があるが、**ガードが成立して
// から** commit する形をエミッタが守る (G3/G4) ので受けてよい。
bool readEaMode(u32 mode, u32 reg, u8& out)
{
    switch (mode)
    {
        case 2:
            out = kEaIndirect;
            return true;
        case 3:
            out = kEaPostInc;
            return true;
        case 4:
            out = kEaPreDec;
            return true;
        case 5:
            out = kEaDisp16;
            return true;
        case 7:
            if (reg == 0)
            {
                out = kEaAbsShort;
                return true;
            }
            if (reg == 1)
            {
                out = kEaAbsLong;
                return true;
            }
            return false;
        default:
            return false;
    }
}

// 書き形として受けてよい EA か (Tier C)。受けるなら eaMode を返す。
//
// **読み形と同じ集合。** 実効アドレスの計算はまったく同じ経路
// (effectiveAddress / effectiveAddressSlow) を通るので、受ける形を分ける
// 理由が無い。分けると「読みでは受けるが書きでは受けない」形ができ、
// 片方だけ壊れたときに気づけない。
bool writeEaMode(u32 mode, u32 reg, u8& out)
{
    return readEaMode(mode, reg, out);
}

// MOVE.b/w/l Dn,Dm。src / dst とも mode 0 のときだけ受ける。
bool planMove(u16 op, PlannedOp& out)
{
    const u32 group = static_cast<u32>(op >> 12);
    const u32 srcMode = static_cast<u32>((op >> 3) & 7u);
    const u32 srcReg = static_cast<u32>(op & 7u);
    const u32 dstMode = static_cast<u32>((op >> 6) & 7u);
    const u32 dstReg = static_cast<u32>((op >> 9) & 7u);

    const u32 size = moveSizeFromGroup(group);

    // 転送先がメモリ (Tier C)。**転送元は Dn だけ**を受ける。
    //
    // Why not <mem>,<mem> も入れないか: 読みガードと書きガードが 1 命令の
    // 中に 2 つ並び、脱出の島も 2 つ要る。しかも読みが成立して書きが
    // 不成立という組み合わせで「読みの副作用 ((An)+ の An 更新) だけ
    // 済んだ状態」を作らないよう、2 つのガードをまとめて先に評価する
    // 別の形が要る。まず片側だけで被覆を測る。
    //
    // Why not #imm,<mem> も入れないか: 拡張ワードが EA のものと即値の
    // ものに分かれ、foldImmediate が 1 つの imm 欄に両方を持てない。
    // PlannedOp を 20 バイトに保つ制約と正面から当たるので、別途設計が要る。
    if (dstMode > 1)
    {
        u8 dstEa = kEaNone;
        if (!writeEaMode(dstMode, dstReg, dstEa))
        {
            return false;
        }
        // 転送元は Dn だけ。An / 即値 / メモリは受けない。
        if (srcMode != 0)
        {
            return false;
        }
        out.kind = PlanKind::kMoveDregToMem;
        out.eaMode = dstEa;
        out.srcReg = static_cast<u8>(srcReg);
        // **EA の An 番号は dstReg**。eaRegOf() の規約 (block_plan.h)。
        out.dstReg = static_cast<u8>(dstReg);
        out.size = static_cast<u8>(size);
        // groupMove は EA の形にもサイズにもよらず 4 (m68k_ops_move.cpp:68)。
        out.cycles = 4;
        return true;
    }

    // 転送元は Dn / An / 即値 / **メモリ読み** (Tier B)。
    const bool srcIsImmediate = srcMode == 7 && srcReg == 4;
    u8 srcEa = kEaNone;
    const bool srcIsMemory = !srcIsImmediate && srcMode > 1 && readEaMode(srcMode, srcReg, srcEa);
    if (srcMode > 1 && !srcIsImmediate && !srcIsMemory)
    {
        return false;
    }

    // メモリ読み形は Dn 宛てだけ。**MOVEA (dstMode == 1) は入れない。**
    //
    // Why not 一緒に入れないか: MOVEA.w は符号拡張して 32bit を書くので、
    // ガードの後ろに繋ぐ本体が MOVE とは別物になる。入れるなら別の kind と
    // 別のテストが要る。読み形の被覆はまず Dn 宛てで測る。
    if (srcIsMemory)
    {
        if (dstMode != 0)
        {
            return false;
        }
        out.kind = PlanKind::kMoveMemToDreg;
        out.eaMode = srcEa;
        out.srcReg = static_cast<u8>(srcReg);
        out.dstReg = static_cast<u8>(dstReg);
        out.size = static_cast<u8>(size);
        // groupMove は EA の形によらず 4 を返す (m68k_ops_move.cpp:68)。
        out.cycles = 4;
        return true;
    }

    // **byte で An に触る形は不当命令。** 68000 は例外を出す
    // (m68k_ops_move.cpp が readEa より前に弾いている)。§5.3 の
    // 「例外が起きうる地点を入れない」に直結するので、ここで落とす。
    const bool touchesAddressRegisterAsByte = size == kByte && (srcMode == 1 || dstMode == 1);
    if (touchesAddressRegisterAsByte)
    {
        return false;
    }

    // 転送先が An なら MOVEA。**フラグを 1 つも変えない別の命令**なので、
    // 転送元ごとに種別を分けてエミッタが取り違えないようにする。
    if (dstMode == 1)
    {
        out.kind = srcIsImmediate ? PlanKind::kMoveaImmToAreg
                   : srcMode == 1 ? PlanKind::kMoveaAregToAreg
                                  : PlanKind::kMoveaDregToAreg;
    }
    else
    {
        out.kind = srcIsImmediate ? PlanKind::kMoveImmToDreg
                   : srcMode == 1 ? PlanKind::kMoveAregToDreg
                                  : PlanKind::kMoveRegToReg;
    }
    out.srcReg = static_cast<u8>(srcReg);
    out.dstReg = static_cast<u8>(dstReg);
    out.size = static_cast<u8>(size);
    // groupMove は MOVEA 経路も writeEa 経路も 4 を返す (m68k_ops_move.cpp)。
    out.cycles = 4;
    return true;
}

// JSR の実効アドレスとして受けてよい形か (Tier D)。
//
// **mode 3/4 は受けない。** 68000 の JSR は制御アドレッシングしか取らず、
// (An)+ / -(An) は符号としては通っても正当な命令ではない。インタプリタ側は
// effectiveAddress がそれらを黙って計算してしまうが、その形は
// 「実機の JSR」ではないので**エミュレータの現在の挙動をわざわざ写す価値が無い**。
// 受けなければブロックがそこで切れるだけで、正しさは step() が持つ。
//
// **mode 0/1 も受けない。** JSR Dn / JSR An は effectiveAddress が
// halted を立てる形 (m68k.cpp) で、実行器の「例外に入らない」契約と
// 相容れない。
bool jsrEaMode(u32 mode, u32 reg, u8& out)
{
    switch (mode)
    {
        case 2:
            out = kEaIndirect;
            return true;
        case 5:
            out = kEaDisp16;
            return true;
        case 7:
            if (reg == 0)
            {
                out = kEaAbsShort;
                return true;
            }
            if (reg == 1)
            {
                out = kEaAbsLong;
                return true;
            }
            return false;
        default:
            return false;
    }
}

// $4 から、メモリに触れず例外も起きない 3 系統だけを拾う。
//
// Why not NEGX/NEG/NOT Dn も入れないか: X と累積 Z が絡む。planAlu が
// ADDX/SUBX を除いているのと同じ理由で、まず確実な形だけにする。
bool planMisc(u16 op, PlannedOp& out)
{
    const u32 mode = static_cast<u32>((op >> 3) & 7u);
    const u32 reg = static_cast<u32>(op & 7u);

    // --- Tier D: RTS / JSR ---
    //
    // **判定を LEA / 単項演算より先に置く。** RTS ($4E75) は
    // LEA のマスク ($F1C0 == $41C0) にも単項のマスクにも当たらないので順序は
    // 問題にならないが、JSR ($4E80-$4EBF) は単項演算の判別ビット
    // ((op >> 8) & 0xF == 0xE) の範囲に入る。後に回すと取りこぼす。
    //
    // 判定の順序は m68k_ops_group4.cpp の実装に合わせてある。

    if (op == 0x4E75u)  // RTS
    {
        out.kind = PlanKind::kRts;
        // m68k_ops_group4.cpp:101 の RTS は 16。
        out.cycles = 16;
        // **eaMode を持たない。** A7 からの読みは EA の合成を通らない。
        out.eaMode = kEaNone;
        // **size を必ず埋める。** エミッタの読みガードは op.size から
        // 「窓の上限 - size」を作る。0 のままだと bound が limit そのものに
        // なり、**窓の末尾 3 バイトで範囲外を読む**。
        // 状態の意味としては「A7 から read32 する」なので 4。
        out.size = static_cast<u8>(kLong);
        return true;
    }

    const bool isJsr = (op & 0xFFC0u) == 0x4E80u;
    if (isJsr)
    {
        u8 ea = kEaNone;
        if (!jsrEaMode(mode, reg, ea))
        {
            return false;
        }
        out.kind = PlanKind::kJsr;
        out.eaMode = ea;
        // **EA の An 番号は dstReg。** 読み形は srcReg に置くという規約
        // (block_plan.h) に対し、JSR の EA は「読む元」ではないので
        // 書き形と同じ欄にそろえる。参照は eaRegOf() を通さない
        // (isMemoryWriteKind が kJsr を含まないので dstReg を直に読む)。
        out.dstReg = static_cast<u8>(reg);
        out.size = static_cast<u8>(kLong);
        // m68k_ops_group4.cpp:370 の JSR は EA の形によらず 16。
        out.cycles = 16;
        return true;
    }

    // LEA <ea>,An : 0100 rrr 111 mmm rrr
    //
    // **アドレスを求めるだけで読まない**ので、メモリに触らない。
    const bool isLea = (op & 0xF1C0u) == 0x41C0u;
    if (isLea)
    {
        // mode 3/4 は An を進める副作用、mode 6 と 7.2/7.3 は安全だが
        // 拡張ワードの解釈が要るので今回は入れない。
        const bool isDisp = mode == 2 || mode == 5;
        const bool isAbs = mode == 7 && reg <= 1;
        if (!isDisp && !isAbs)
        {
            return false;
        }
        out.kind = isDisp ? PlanKind::kLeaDisp : PlanKind::kLeaAbs;
        out.srcReg = static_cast<u8>(reg);
        out.dstReg = static_cast<u8>((op >> 9) & 7u);
        out.size = static_cast<u8>(kLong);
        // m68k_ops_group4.cpp の LEA は EA の形によらず 4 を返す。
        out.cycles = 4;
        return true;
    }

    // --- Tier E: MOVEM.L の 2 形 ---
    //
    // **受けるのは 2 つの符号だけ。**
    //   MOVEM.L (An)+,<regs> : 0100 1100 11 011 rrr = $4CD8 + rrr
    //   MOVEM.L <regs>,-(An) : 0100 1000 11 100 rrr = $48E0 + rrr
    //
    // Why not 他の形も受けないか: 実測 (400M サイクル、Human68k 起動) では
    // MOVEM の **100%** がこの 2 形だった (.W が 0 件、(An)+ と -(An) 以外の
    // EA が 0 件)。残りを受けても被覆は 1 命令も増えず、代わりに
    // 「レジスタ順が逆になる形」「拡張ワードを持つ EA」「符号拡張が要る .W」を
    // それぞれ別の検証面として背負う。
    //
    // **とくに方向と EA の組は入れ替えられない。** 読みは (An)+ だけ、
    // 書きは -(An) だけ。読みの mode 4 (MOVEM.L -(An),<regs>) と書きの
    // mode 3 (MOVEM.L <regs>,(An)+) は 68000 に存在しない符号なので、
    // マスクを $FFF8 にして d ビットと mode を同時に固定する。
    //
    // レジスタマスクは拡張ワードなので、ここでは埋まらない。plan() 側の
    // foldImmediate が imm へ入れ、本数の検査もそこで行う。
    const bool isMovemPostIncToRegs = (op & 0xFFF8u) == 0x4CD8u;
    if (isMovemPostIncToRegs)
    {
        out.kind = PlanKind::kMovemPostIncToRegs;
        out.eaMode = kEaPostInc;
        // **EA の An 番号は srcReg** (読み形の欄)。eaRegOf() の規約。
        out.srcReg = static_cast<u8>(reg);
        out.size = static_cast<u8>(kLong);
        // cycles は本数で決まるので **foldImmediate が入れる。**
        // ここで 0 のまま置くと「サイクル 0 の命令」に見えるが、
        // 本数はレジスタマスク (拡張ワード) を読むまで分からない。
        return true;
    }

    const bool isMovemRegsToPredec = (op & 0xFFF8u) == 0x48E0u;
    if (isMovemRegsToPredec)
    {
        out.kind = PlanKind::kMovemRegsToPredec;
        out.eaMode = kEaPreDec;
        // **EA の An 番号は dstReg** (書き形の欄)。eaRegOf() の規約。
        out.dstReg = static_cast<u8>(reg);
        out.size = static_cast<u8>(kLong);
        return true;
    }

    // 単項 TST / CLR : 0100 oooo ss mmm rrr。
    // **判別ビットは (op >> 8) & 0xF。** 実装の unary_ops が switch している
    // 値と揃える (ここを (op >> 9) & 7 で書いて TST を丸ごと取りこぼした
    // 前例がある)。
    const u32 opcodeBits = static_cast<u32>((op >> 8) & 0xFu);
    const u32 sizeField = static_cast<u32>((op >> 6) & 3u);
    if (sizeField == 3)
    {
        return false;
    }
    const u32 size = sizeField == 0 ? kByte : (sizeField == 1 ? kWord : kLong);

    // TST <mem> (Tier B) と CLR <mem> (Tier C)。
    //
    // TST は読むだけ。CLR は読んで書く RMW だが、**読み値は捨てられる**
    // (m68k_ops_group4.cpp:606-607 が readEaForModify の戻り値を使わない)。
    // ガードが成立する範囲では read8/16/32 の fast path に副作用が無いので、
    // 生成コードは読みを 1 つも吐かない (G20)。
    if (mode != 0)
    {
        u8 ea = kEaNone;
        if (opcodeBits == 0xAu && readEaMode(mode, reg, ea))
        {
            out.kind = PlanKind::kTstMem;
            out.eaMode = ea;
            out.srcReg = static_cast<u8>(reg);
            out.size = static_cast<u8>(size);
            // m68k_ops_group4.cpp:657 の TST は EA の形によらず 4。
            out.cycles = 4;
            return true;
        }
        if (opcodeBits == 0x2u && writeEaMode(mode, reg, ea))
        {
            out.kind = PlanKind::kClrMem;
            out.eaMode = ea;
            // **EA の An 番号は dstReg**。eaRegOf() の規約 (block_plan.h)。
            out.dstReg = static_cast<u8>(reg);
            out.size = static_cast<u8>(size);
            // m68k_ops_group4.cpp:611 の CLR は EA の形にもサイズにもよらず 6。
            out.cycles = 6;
            return true;
        }
        return false;
    }

    if (opcodeBits == 0xAu)  // TST Dn
    {
        out.kind = PlanKind::kTstDreg;
        out.srcReg = static_cast<u8>(reg);
        out.size = static_cast<u8>(size);
        out.cycles = 4;
        return true;
    }
    if (opcodeBits == 0x2u)  // CLR Dn
    {
        out.kind = PlanKind::kClrDreg;
        out.dstReg = static_cast<u8>(reg);
        out.size = static_cast<u8>(size);
        // **サイズによらず 6。** 実機の 68000 は .b/.w の Dn 形が 4 だが、
        // このエミュレータは一律 6 を返す (m68k_ops_group4.cpp)。
        // JIT の契約は「インタプリタとビット単位で同一」なので、
        // 実機の値へ「直す」と JIT ON/OFF でサイクルが割れる。
        out.cycles = 6;
        return true;
    }
    return false;
}

// MOVEQ #imm8,Dn。
bool planMoveq(u16 op, PlannedOp& out)
{
    // bit8 が立つ符号は MOVEQ ではなく不当命令。instructionLength も
    // kUnknownLength を返すので I1 が先に弾くが、planOne 単体でも
    // 正しく false を返せるようにここでも見る。
    const bool isIllegalEncoding = (op & 0x0100u) != 0;
    if (isIllegalEncoding)
    {
        return false;
    }

    out.kind = PlanKind::kMoveq;
    out.dstReg = static_cast<u8>((op >> 9) & 7u);
    out.size = static_cast<u8>(kLong);
    out.imm = static_cast<u32>(static_cast<s32>(static_cast<s8>(op & 0xFFu)));
    // groupMoveq は常に 4。
    out.cycles = 4;
    return true;
}

// ADD/SUB/AND/OR/EOR/CMP のレジスタ間形。
bool planAlu(u16 op, PlannedOp& out)
{
    const u32 opmode = static_cast<u32>((op >> 6) & 7u);
    const u32 mode = static_cast<u32>((op >> 3) & 7u);

    // opmode 3/7 は ADDA/SUBA/CMPA と MULU/MULS/DIVU/DIVS。
    // A レジスタを書く形と、除算でゼロ除算例外に入りうる形。どちらも入れない。
    const bool isAddressOrMulDiv = opmode == 3 || opmode == 7;
    if (isAddressOrMulDiv)
    {
        return false;
    }

    // mode 0 (Dn) と、Tier B の読み形メモリ EA を受ける。
    //
    // **読み方向 (opmode < 4) だけ。** メモリ方向はこの下で弾く。
    const u32 reg = static_cast<u32>(op & 7u);
    u8 ea = kEaNone;
    const bool isMemoryRead = mode != 0 && readEaMode(mode, reg, ea);
    if (mode != 0 && !isMemoryRead)
    {
        return false;
    }

    if (isMemoryRead)
    {
        // メモリ方向 (opmode 4/5/6) は書き込みなので入れない。
        // **読み方向より先に見る。** 後に回すと $B の EOR <ea>,Dn
        // (メモリ方向) を読み形として受けてしまう。
        if ((opmode & 4u) != 0)
        {
            return false;
        }
        PlanAluOp memAluOp = PlanAluOp::kAdd;
        const u32 memGroup = static_cast<u32>(op >> 12);
        if (!aluOpFromGroupRead(memGroup, memAluOp))
        {
            return false;
        }

        // **byte で An を読む形は不当命令。** 68000 は .b の mode 1 を
        // 持たないが、ここで受ける mode 2-5/7 は An の**中身**ではなく
        // アドレスとして使うので .b でも正当。mode 1 は readEaMode が
        // 既に弾いている。

        out.kind = PlanKind::kAluMemToDreg;
        out.eaMode = ea;
        out.aluOp = memAluOp;
        out.srcReg = static_cast<u8>(reg);
        out.dstReg = static_cast<u8>((op >> 9) & 7u);
        out.size = static_cast<u8>(aluSizeFromOpmode(opmode));
        // 読み方向は EA の形によらず 4 (m68k_ops_alu.cpp の各 return)。
        out.cycles = 4;
        return true;
    }

    // メモリ方向 (opmode 4/5/6) + mode 0 は、群ごとに意味の違う特殊形になる。
    //   $9/$D : ADDX/SUBX Dy,Dx (X を巻き込み、Z が累積する)
    //   $8/$C : SBCD/ABCD Dy,Dx (10 進補正)
    //   $B    : EOR Dn,Dn
    //
    // ADDX/SUBX/SBCD/ABCD は状態の持ち方が普通の ALU と違うので入れない。
    // **EOR Dn,Dn も入れない。**
    //
    // Why not EOR だけ通さないか: instructionLength が $B のこの形に
    // kUnknownLength を返す (m68k_length.h の isSpecialForm が「判別が
    // 群ごとに違う」ことを理由に、メモリ方向の mode 0/1 をまとめて
    // 諦めている)。plan() は I1 (長さ) を I2 (許可リスト) より先に見るので、
    // ここで受けても **一度も真にならない死んだコード**にしかならない。
    // しかもテストは「EOR を弾く」ことを緑で確認して通る。
    // 命令長デコーダが $B の特殊形を判別できるようになったとき、ここも
    // 一緒に開ける。片方だけ足すと、開けたつもりで閉じたままになる。
    const bool toMemory = (opmode & 4u) != 0;
    if (toMemory)
    {
        return false;
    }

    // <ea>,Dn 方向。mode 0 なので Dn,Dn。
    PlanAluOp aluOp = PlanAluOp::kAdd;
    const u32 group = static_cast<u32>(op >> 12);
    if (!aluOpFromGroupRead(group, aluOp))
    {
        return false;
    }

    out.kind = PlanKind::kAluRegToReg;
    out.aluOp = aluOp;
    out.srcReg = static_cast<u8>(op & 7u);
    out.dstReg = static_cast<u8>((op >> 9) & 7u);
    out.size = static_cast<u8>(aluSizeFromOpmode(opmode));
    // groupAdd / groupSub / groupOrDiv / groupAndMul / groupCmpEor の
    // 読み出し方向は、サイズによらず 4 を返す。
    out.cycles = 4;
    return true;
}

// Bcc / BRA / BSR。
bool planBranch(u16 op, u32 pc, PlannedOp& out)
{
    const u32 cond = static_cast<u32>((op >> 8) & 0xFu);
    const u32 disp8 = static_cast<u32>(op & 0xFFu);

    // 32bit 変位 ($FF) は 68020 以降。instructionLength も
    // kUnknownLength を返すので I1 が先に弾くが、単体でも落とす。
    //
    // **BSR.l ($61FF) もここで落ちる。** cond == 1 の判定より先に置いてある
    // ので、BSR を受けるようにしても $61FF は入らない。
    if (disp8 == 0xFFu)
    {
        return false;
    }

    // 分岐先の基準は「命令語の次のワードのアドレス」。
    // groupBranch は st_.pc - 4 で同じ値を作る (命令語を fetch した直後の
    // pc は 命令語 + 6 なので、pc - 4 が 命令語 + 2)。
    const u32 base = pc + 2;

    // Tier F: BSR (cond == 1)。**Bcc と飛び先の作り方は完全に同じ**なので、
    // kind と cycles だけを分ける。
    //
    // **cond 欄は埋めない。** BSR は条件を持たない (必ず飛ぶ) ので、
    // 0 のままにしておく。1 を入れると emitCondition が「cond == 1」を
    // 評価しようとする形が生まれる。エミッタは kBsr で条件を一切見ないが、
    // 意味の無い値を置かないでおけば、後から誰かが読んでも迷わない。
    const bool isBsr = cond == 1;

    if (disp8 == 0)
    {
        // 16bit 変位。拡張ワードの中身は plan() が読んで埋める。
        out.kind = isBsr ? PlanKind::kBsr : PlanKind::kBranch;
        out.cond = isBsr ? 0u : static_cast<u8>(cond);
        // groupBranch: BSR は形によらず 18、Bcc の不成立側は disp8 == 0 で 12。
        out.cycles = isBsr ? 18u : 12u;
        // **BSR は A7 から 4 バイトぶん read/write する。** エミッタの
        // 範囲ガードは op.size から「窓の上限 - size」を作るので、
        // 0 のままだと窓の末尾 3 バイトで範囲外を書く (RTS と同じ理由)。
        out.size = isBsr ? static_cast<u8>(kLong) : 0u;
        out.imm = base;
        return true;
    }

    out.kind = isBsr ? PlanKind::kBsr : PlanKind::kBranch;
    out.cond = isBsr ? 0u : static_cast<u8>(cond);
    // BSR は 18、Bcc の 8bit 変位の不成立側は 8。
    out.cycles = isBsr ? 18u : 8u;
    out.size = isBsr ? static_cast<u8>(kLong) : 0u;
    out.imm = base + static_cast<u32>(static_cast<s32>(static_cast<s8>(disp8)));
    return true;
}

// アドレスからページ番号。CodeGenMap と同じ 1KB 粒度にそろえる。
constexpr u32 pageOf(u32 addr)
{
    return addr >> CodeGenMap::kPageShift;
}

}  // namespace

bool BlockPlanner::planOne(u16 op, u32 pc, PlannedOp& out)
{
    // 呼び出し側が使い回した PlannedOp のゴミを引き継がないよう、
    // 受ける前に全部の欄をゼロにする。kind ごとに使う欄が違うので、
    // 埋め忘れた欄が前回の値のまま残ると段 2 のエミッタが誤読する。
    out = PlannedOp{};
    out.pc = pc;
    out.op = op;

    const u32 group = static_cast<u32>(op >> 12);
    switch (group)
    {
        case 0x1u:
        case 0x2u:
        case 0x3u:
            return planMove(op, out);
        case 0x4u:
            return planMisc(op, out);
        case 0x6u:
            return planBranch(op, pc, out);
        case 0x7u:
            return planMoveq(op, out);
        case 0x8u:
        case 0x9u:
        case 0xBu:
        case 0xCu:
        case 0xDu:
            return planAlu(op, out);
        default:
            // 許可リストに無い。$0 の即値演算も $4 (RTS/JSR/MOVEM/LEA) も
            // $5 (ADDQ/Scc/DBcc) も $E (シフト) も、まだ入れない。
            return false;
    }
}

// 拡張ワード由来の即値を PlannedOp へ畳む。
//
// **読み方は interpreter と揃える。** readEaSlow の 7.4 (即値) と
// effectiveAddressSlow の mode 5 / 7.0 / 7.1 が、それぞれどう合成して
// いるかに合わせる。ずれると値が静かに変わる。
void foldImmediate(PlannedOp& p, u16 ext0, u16 ext1, u32 length)
{
    const auto sext16 = [](u16 v)
    { return static_cast<u32>(static_cast<s32>(static_cast<s16>(v))); };
    const u32 longValue = (static_cast<u32>(ext0) << 16) | ext1;

    switch (p.kind)
    {
        case PlanKind::kMoveImmToDreg:
            // size でマスクする。byte は下位バイトだけ。
            p.imm = p.size == 4 ? longValue : (p.size == 1 ? (ext0 & 0xFFu) : ext0);
            break;
        case PlanKind::kMoveaImmToAreg:
            // MOVEA.w は符号拡張して 32bit 全体を書く。
            p.imm = p.size == 4 ? longValue : sext16(ext0);
            break;
        case PlanKind::kLeaDisp:
            // (An) 形 (length == 2) は変位を持たない。
            p.imm = length == 4 ? sext16(ext0) : 0;
            break;
        case PlanKind::kLeaAbs:
            // (xxx).W は符号拡張、(xxx).L は 2 語を連結。
            p.imm = length == 4 ? sext16(ext0) : longValue;
            break;

        // --- Tier B: 読み形の EA が持つ定数 ---
        //
        // **effectiveAddressSlow (m68k.cpp:509-545) と同じ合成にする。**
        //   mode 5    : (d16,An) の d16 を符号拡張 (514-518 行)
        //   mode 7.0  : (xxx).W は符号拡張される (534 行)
        //   mode 7.1  : (xxx).L は 2 語を連結 (537-541 行)
        // mode 2/3/4 は拡張ワードを持たないので 0 のまま。
        // --- Tier C: 書き形の EA が持つ定数 ---
        //
        // 実効アドレスの合成は読み形とまったく同じ (effectiveAddressSlow を
        // 通るのが読み書きどちらでも同じ関数なので、分ける理由が無い)。
        case PlanKind::kMoveMemToDreg:
        case PlanKind::kTstMem:
        case PlanKind::kAluMemToDreg:
        case PlanKind::kMoveDregToMem:
        case PlanKind::kClrMem:
        // --- Tier D: JSR の飛び先 EA が持つ定数 ---
        //
        // 合成は読み形・書き形とまったく同じ。effectiveAddressSlow を通るのが
        // どの向きでも同じ関数なので、分ける理由が無い。
        // **kRts はここへ来ない** (eaMode が kEaNone なので imm を持たない)。
        case PlanKind::kJsr:
            switch (p.eaMode)
            {
                case kEaDisp16:
                case kEaAbsShort:
                    p.imm = sext16(ext0);
                    break;
                case kEaAbsLong:
                    p.imm = longValue;
                    break;
                default:
                    p.imm = 0;
                    break;
            }
            break;

        // --- Tier E: MOVEM.L のレジスタマスクとサイクル ---
        //
        // 拡張ワードはレジスタマスクそのもの (EA の定数ではない)。
        // **生値をそのまま入れる。** ビットとレジスタの対応は方向で逆転する
        // (-(An) は A7 から D0 へ / m68k_ops_group4.cpp:397-405) が、
        // ここでは正規化しない。解釈をエミッタ 1 箇所に閉じておかないと、
        // planner とエミッタが別々に逆転させて二重に打ち消す。
        //
        // **サイクルは本数で決まるのでここで入れる。** planOne は命令語
        // だけを見るので本数を知らない。インタプリタの return 式と同じ形に
        // する (m68k_ops_group4.cpp:420 と 482):
        //   (An)+,<regs> : 12 + count * 8   (.L の per-reg は 8)
        //   <regs>,-(An) :  8 + count * 8
        // 基底が 4 違う。片方から導くと必ずずれる。
        case PlanKind::kMovemPostIncToRegs:
        case PlanKind::kMovemRegsToPredec:
        {
            p.imm = ext0;
            u32 count = 0;
            for (u32 bit = 0; bit < 16; ++bit)
            {
                if (((p.imm >> bit) & 1u) != 0)
                {
                    ++count;
                }
            }
            const u32 base = p.kind == PlanKind::kMovemPostIncToRegs ? 12u : 8u;
            // 本数の上限は plan() が別に検査する。ここは 4 本までなら
            // 12 + 4*8 = 44 で u8 に収まる。
            p.cycles = static_cast<u8>(base + count * 8u);
            break;
        }
        default:
            break;
    }
}

bool BlockPlanner::plan(const PlanSource& src, const PlanGenSource& gen, u32 entryPc,
                        BlockPlan& out, const PlanCapabilities& caps)
{
    // 読み形を積んでよいか。**教わっていなければ積んでよい** (段 1 以前と
    // 同じ挙動)。教わった場合、窓が使えないなら読み形の手前で終端する。
    const bool readsAllowed = caps.canEmitReads == nullptr || caps.canEmitReads(caps.ctx);
    // 書き形を積んでよいか (Tier C)。読みと条件が違う (block_planner.h)。
    const bool writesAllowed = caps.canEmitWrites == nullptr || caps.canEmitWrites(caps.ctx);
    // 動的分岐 (Tier D) を積んでよいか。**既定 (nullptr) は「入れてよい」。**
    const bool dynamicBranchAllowed =
        caps.canEmitDynamicBranch == nullptr || caps.canEmitDynamicBranch(caps.ctx);

    // entryPc == 0 は空きスロットの番兵なので、そこからは翻訳しない。
    //
    // Why 要るか: BlockPlan::entryPc の 0 は「このスロットは空」を意味する。
    // 番地 0 のブロックを作ると、**空きスロットと区別できない計画**が
    // できてしまう。検索側が空きと見て上書きするか、空きを有効な計画と
    // 読むかは実装次第で、どちらも静かに壊れる。
    //
    // 実害があるかは別問題で、X68000 では $000000-$0003FF が例外ベクタ
    // テーブルなので通常そこにコードは無い (リセットは番地 4 から PC を
    // 読む)。**ただし「通常無い」に頼らない。** ゲストは何でもできるし、
    // 番兵の意味は 68000 の都合とは無関係に守られるべき。
    if (entryPc == 0)
    {
        return false;
    }

    // I9: 世代が飽和 (または CodeGenMap が未配線) なら、そもそも翻訳しない。
    //
    // Why not 翻訳してから鍵の照合で落とすか: kAlwaysStale を控えた計画は
    // 「照合で必ず外れる計画」でしかない。作った瞬間に捨てると分かっている
    // ものを保持すると、スロットを 1 つ潰したまま毎回翻訳し直すので、
    // 遅くなるうえに統計の keyMissStale と translateFail が混ざる。
    const u16 entryGen = gen.generation(gen.ctx, entryPc);
    if (entryGen == CodeGenMap::kAlwaysStale)
    {
        return false;
    }

    const u32 entryPage = pageOf(entryPc);

    out = BlockPlan{};
    out.entryPc = entryPc;
    out.mappingEpoch = gen.mappingEpoch(gen.ctx);
    out.page = entryPage;
    out.pageGen = entryGen;
    out.count = 0;
    out.end = BlockEnd::kUnsupported;
    out.fallThroughPc = entryPc;
    out.branchTarget = 0;
    out.cyclesNotTaken = 0;
    out.cyclesTaken = 0;

    u32 pc = entryPc;
    for (;;)
    {
        // I5: 容量。
        if (out.count >= kMaxOps)
        {
            out.end = BlockEnd::kCapacity;
            break;
        }

        // I3: 命令語そのものが窓の中にあること。
        u16 op = 0;
        if (!src.read16(src.ctx, pc, op))
        {
            out.end = BlockEnd::kWindowExit;
            break;
        }

        // I1: 長さが静的に決まること。
        const u32 length = instructionLength(op);
        if (length == kUnknownLength)
        {
            out.end = BlockEnd::kUnknownLength;
            break;
        }

        // I2: 許可リストに入ること。
        PlannedOp planned{};
        if (!planOne(op, pc, planned))
        {
            out.end = BlockEnd::kUnsupported;
            break;
        }

        // **エミッタが発行できない読み形は、積まずに終端する。**
        //
        // 積んでしまうとエミッタがブロックを丸ごと拒否する。読み形の手前で
        // 終端すれば、短くても翻訳できるブロックが残る。1 つ入っただけで
        // 全部失うのは、入れる前より悪い。
        // **needsReadWindow / needsWriteWindow を通す。** eaMode を直に見ると
        // Tier D を取りこぼす: RTS は A7 から read32 するのに eaMode が
        // kEaNone で、JSR は -(A7) へ write32 するのに eaMode は飛び先を指す。
        const bool isWrite = needsWriteWindow(planned.kind);
        const bool isRead = needsReadWindow(planned.kind, planned.eaMode);
        if (isRead && !readsAllowed)
        {
            out.end = BlockEnd::kUnsupported;
            break;
        }
        if (isWrite && !writesAllowed)
        {
            out.end = BlockEnd::kUnsupported;
            break;
        }
        // 動的分岐 (Tier D) は飛び先の置き場 (メールボックス) が要る。
        //
        // **読み・書きの許可とは別の条件。** 窓が全部そろっていても
        // メールボックスが未配線なら焼けないので、そこで終端する。
        //
        // **静的な飛び先を持つ終端 (Bcc / BRA / BSR) は除く。** 飛び先が
        // BlockPlan::branchTarget に入るので、メールボックスが未配線でも
        // 焼ける。`kind != kBranch` と直に書くと、後から静的な終端を
        // 足した人 (= BSR) が黙ってこのゲートに掛かる。
        if (isBlockTerminator(planned.kind) && !hasStaticBranchTarget(planned.kind) &&
            !dynamicBranchAllowed)
        {
            out.end = BlockEnd::kUnsupported;
            break;
        }
        // **CLR <mem> は読みの許可も要る (G20)。** 生成コードは読みを
        // 吐かないが、それは「ガードが成立する範囲では read8/16/32 の
        // fast path に副作用が無い」ことに乗った省略。窓が読めない写像
        // (ROM 写像中) では、インタプリタの読みは ROM かバスへ落ちて
        // 別の値を返し、バスエラーにも入りうる。**省略の前提が消える。**
        if (planned.kind == PlanKind::kClrMem && !readsAllowed)
        {
            out.end = BlockEnd::kUnsupported;
            break;
        }
        planned.length = static_cast<u8>(length);

        const u32 nextPc = pc + length;

        // I4: entryPc から nextPc + 2 までが同一 1KB ページに収まること。
        //
        // **+2 が要る。** 出口の CPU 状態は irc == mem16(nextPc + 2) を
        // 含むので、nextPc までしか見ないと「最後の命令の次のワード」は
        // ページ内でも、その先の irc に入るワードを読み損ねる。
        //
        // **これは鍵を 1 ページに閉じるための条件であって、窓の中に
        // あることの保証ではない。** ページに収まることと窓に収まることは
        // 別の性質で、現在の窓 (メイン RAM 2MB / IPL-ROM 128KB) の境界が
        // たまたま 1KB 整列しているために一致して見えているだけ。
        // 窓の判定は I3 (PlanSource::read16 が false を返したら終端) が
        // 別に担っている。**I4 があるから窓を考えなくてよい、とは読まないこと。**
        // 窓の長さが 1KB 非整列になったら、ここではなく I3 が効く。
        if (pageOf(nextPc + 2) != entryPage)
        {
            out.end = BlockEnd::kPageBoundary;
            break;
        }

        // 拡張ワードも窓の中にあること (I3 の残り)。
        // 段 1 の許可リストで 4 バイトになるのは Bcc.w だけだが、
        // 命令ごとに書き分けると足したときに漏れるので長さから回す。
        // **2 語まで集める。** MOVE.l #imm と LEA (xxx).L は 6 バイトで
        // 拡張が 2 語ある。1 語しか持たないと即値の上位が落ちる。
        bool extensionsReadable = true;
        u16 lastExtension = 0;
        u16 extension[2] = {0, 0};
        u32 extensionCount = 0;
        for (u32 offset = 2; offset < length; offset += 2)
        {
            if (!src.read16(src.ctx, pc + offset, lastExtension))
            {
                extensionsReadable = false;
                break;
            }
            if (extensionCount < 2)
            {
                extension[extensionCount] = lastExtension;
                ++extensionCount;
            }
        }
        if (!extensionsReadable)
        {
            out.end = BlockEnd::kWindowExit;
            break;
        }

        // 拡張ワード由来の即値を確定させる。
        //
        // planOne は命令語だけから決まる欄を埋めるので、拡張ワードに
        // 入っている値はここで畳む (planBranch が飛び先の基準だけを置き、
        // 変位をここで足すのと同じ役割分担)。
        foldImmediate(planned, extension[0], extension[1], length);

        // G17 (a): 絶対アドレスの書きが**自ブロックのページ**を指すなら、
        // 積まずに終端する。
        //
        // 実効アドレスが翻訳時に決まっているので、積んでも
        // 「必ず自ページ脱出するブロック」にしかならない。しかも脱出は
        // runner に負のキャッシュを焼かせる (再翻訳の止血) ので、
        // 積むと**その番地の JIT を epoch が動くまで失う**。
        //
        // 動的 EA (mode 2-5) は実行時にしか分からないので、そちらは
        // 生成コードのガードが受け持つ (G13)。
        //
        // **`.l` は両端を見る。** write32 は page(a) と page(a+3) の 2 ページに
        // touch する (m68k.cpp:377-378)。片方だけだと、ページ境界を跨いだ
        // 長語書きで自ページの端を黙って書く。
        //
        // **kJsr はここへ来ない。** JSR の絶対形 EA は**飛び先**であって
        // 書き先ではない。書き先は常に -(A7) で、実行時にしか分からないので
        // 生成コードの自ページガードが受け持つ (G13)。ここで飛び先を
        // 自ページ判定にかけると、**自分のページへの再帰呼び出しを
        // 積めなくする**だけで何も守らない。
        //
        // **kMovemRegsToPredec もここへ来ない。** isMemoryWriteKind には
        // 入るが eaMode が kEaPreDec なので絶対形の条件が成立しない。
        // これは幸運ではなく必要な性質で、MOVEM の imm は**アドレスではなく
        // レジスタマスク**である。絶対形の条件を外すと、マスク $8000 を
        // 「番地 $8000 への書き込み」と読んで自ページ判定にかける。
        // 書き形を足すときは imm が何を指しているかを必ず確かめること。

        // Tier E: MOVEM の本数が上限を超える形は、積まずに終端する。
        //
        // **手前で終端するのが要点。** 積んでからエミッタに丸ごと拒否させると、
        // 「1 つ入っただけでブロック全部を失う」形になる。Tier B の
        // canEmitReads で一度踏んだ轍 (block_planner.h の冒頭)。
        //
        // 本数 0 も弾く。マスクが 0 の MOVEM は 1 本も転送しない正当な命令
        // だが、生成コードは「アクセス 0 回でポインタも動かない」特例になる。
        // 実測で 0 件なので、被覆を 1 命令も失わずに検証面を 1 つ減らせる。
        const bool isMovem = planned.kind == PlanKind::kMovemPostIncToRegs ||
                             planned.kind == PlanKind::kMovemRegsToPredec;
        if (isMovem)
        {
            u32 transfers = 0;
            for (u32 bit = 0; bit < 16; ++bit)
            {
                if (((planned.imm >> bit) & 1u) != 0)
                {
                    ++transfers;
                }
            }
            const bool tooManyOrEmpty = transfers == 0 || transfers > kMovemMaxTransfers;
            if (tooManyOrEmpty)
            {
                // **kUnsupported と別の理由にする。** 「MOVEM を知らない」と
                // 「MOVEM だが本数が多すぎる」で次の一手が違う (block_plan.h)。
                out.end = BlockEnd::kMovemTooManyRegs;
                break;
            }
        }

        const bool isAbsoluteWriteTarget =
            isMemoryWriteKind(planned.kind) &&
            (planned.eaMode == kEaAbsShort || planned.eaMode == kEaAbsLong);
        if (isAbsoluteWriteTarget)
        {
            const u32 addr = planned.imm & 0x00FFFFFFu;
            const u32 last = (addr + planned.size - 1u) & 0x00FFFFFFu;
            if (pageOf(addr) == entryPage || pageOf(last) == entryPage)
            {
                out.end = BlockEnd::kUnsupported;
                break;
            }
        }

        // I7 の Tier D 版: JSR の飛び先が**翻訳時に分かっていて奇数**なら
        // 積まずに終端する。refillPrefetch がアドレスエラーへ入るので、
        // 実行器の「例外に入らない」契約と相容れない。
        //
        // 動的な EA (mode 2 / 5) の飛び先は実行時にしか分からないので、
        // 生成コードのガードが受け持つ。
        const bool isAbsoluteJumpTarget =
            planned.kind == PlanKind::kJsr &&
            (planned.eaMode == kEaAbsShort || planned.eaMode == kEaAbsLong);
        if (isAbsoluteJumpTarget && (planned.imm & 1u) != 0)
        {
            out.end = BlockEnd::kUnsupported;
            break;
        }

        // --- Tier D: 動的分岐で終端する ---
        //
        // **kBranch と同じく必ずブロック末尾** (I6)。飛び先が翻訳済みか
        // どうかはここでは分からないので、必ず切る。
        //
        // 違うのは、飛び先が BlockPlan に入らないこと。生成コードが
        // 実行時に求めてメールボックスへ書く。**branchTarget は 0 のまま**に
        // しておく (使うと「翻訳時に確定した飛び先」と読み違えられる)。
        const bool isDynamicBranch =
            planned.kind == PlanKind::kRts || planned.kind == PlanKind::kJsr;
        if (isDynamicBranch)
        {
            out.ops[out.count] = planned;
            ++out.count;
            out.end = BlockEnd::kDynamicBranch;
            // **fallThroughPc は nextPc のまま。** 動的分岐が成立すれば
            // runner が branchTo で pc / ir / irc を作り直すので使われないが、
            // ガード不成立で降りたときの島は「その命令の手前」を書くので、
            // ここが未定義だと出口の状態が壊れる。
            out.fallThroughPc = nextPc;
            // **成立側と不成立側でサイクルが同じ。** 条件分岐と違って
            // RTS / JSR は必ず飛ぶ。ガードが不成立なら 1 命令も実行せずに
            // 降りるので、そのときは島が「手前まで」の値を返す。
            out.cyclesNotTaken += planned.cycles;
            out.cyclesTaken += planned.cycles;
            break;
        }

        // 静的分岐 (Bcc / BRA / BSR)。飛び先が翻訳時に決まるので
        // BlockPlan::branchTarget へ入れる。
        const bool isBranch = hasStaticBranchTarget(planned.kind);
        if (isBranch)
        {
            // 16bit 変位形は、planBranch が imm に基準アドレスだけを
            // 置いている。拡張ワードを足して飛び先を確定させる。
            const bool hasWordDisplacement = length == 4;
            const u32 target =
                hasWordDisplacement
                    ? planned.imm +
                          static_cast<u32>(static_cast<s32>(static_cast<s16>(lastExtension)))
                    : planned.imm;

            // I7: 飛び先が奇数ならアドレスエラーに入る。**積まずに終端する。**
            //
            // Why not 積んでから実行時に落とすか: 実行器は「例外に入らない」
            // 契約なので、例外に入りうる命令を持たせた時点で契約が破れる。
            // 静的に分かることは静的に弾く。
            if ((target & 1u) != 0)
            {
                out.end = BlockEnd::kUnsupported;
                break;
            }

            out.ops[out.count] = planned;
            ++out.count;
            out.end = BlockEnd::kBranch;
            out.fallThroughPc = nextPc;
            out.branchTarget = target;
            out.cyclesNotTaken += planned.cycles;
            // 成立側は Bcc / BRA なら形によらず 10 (groupBranch)。BRA も
            // testCondition(0) が常に真なのでここを通り、cyclesNotTaken は
            // 参照されない。
            //
            // **BSR は 18 で、しかも .s と .w で同じ。** groupBranch の
            // BSR 経路は disp8 を見ずに 18 を返す (m68k_ops_move.cpp:126)
            // ので、planned.cycles をそのまま使う。BSR は必ず飛ぶので
            // cyclesNotTaken も同じ値になるが、**そちらも埋めておく**:
            // ガード不成立で降りると島が「その命令の手前まで」を返すため、
            // 未定義のままだと出口のサイクルが壊れる (Tier D と同じ)。
            out.cyclesTaken += planned.kind == PlanKind::kBsr ? planned.cycles : 10u;
            // I6: 分岐はブロックの最後にしか置けない。飛んだ先が翻訳済みか
            // どうかはここでは分からないので、必ず切る。
            break;
        }

        out.ops[out.count] = planned;
        ++out.count;
        out.cyclesNotTaken += planned.cycles;
        out.cyclesTaken += planned.cycles;
        out.fallThroughPc = nextPc;
        pc = nextPc;
    }

    return out.count > 0;
}

}  // namespace x68k
