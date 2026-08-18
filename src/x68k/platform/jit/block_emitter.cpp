// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// BlockPlan → Xtensa の直線コード。方針と契約はヘッダの冒頭に書いた。
//
// ## 2 パスにしてある理由
//
// リテラルプールはコードより**前**に置かないと l32r の変位が負にならない。
// だがどのリテラルが要るかは、コードを吐いてみないと分からない。そこで
// 「リテラルだけを集める空回し」と「本番」の 2 回、**同じ発行経路**を通す。
//
// Why not リテラル用に別の関数を書かないか: 2 本に分けると、片方に命令を
// 足してもう片方に足し忘れたときに、リテラルが 1 語足りない状態で本番が走る。
// 変位が 4 バイトずれた l32r は「別の正当な定数」を読むので、失敗は
// 演算結果が静かに間違うという形でしか出てこない。同じ関数を 2 回通せば、
// ずれようがない。

#include "block_emitter.h"

#include "xtensa_encoder.h"

namespace x68k::jit
{
namespace
{

// 生成コードのレジスタ割り当て。
//
// call0 ABI で a0 (戻りアドレス) と a1 (SP) は触らない。生成コードは**葉**
// (関数を一切呼ばない) なので、a2-a11 を自由に使える。a12-a15 は callee-saved
// だが、葉なら保存の義務が無いのと同じことなので使わない — 将来ヘルパ呼び出しを
// 足す人が「a12 は生きているはず」と考えられる余地を残しておく。
constexpr XReg kState = 3;      // M68kState*。入口で a2 から写す
constexpr XReg kTmpA = 4;       // src / d
constexpr XReg kTmpB = 5;       // dst / s
constexpr XReg kTmpC = 6;       // 結果
constexpr XReg kTmpD = 7;       // 作業
constexpr XReg kTmpE = 8;       // 作業
constexpr XReg kTmpCcr = 9;     // 組み立て中の CCR
constexpr XReg kTmpSr = 10;     // 読み出した sr
constexpr XReg kTmpConst = 11;  // 定数の置き場
constexpr XReg kRet = 2;        // 引数 (M68kState*) と戻り値 (サイクル数)

// CCR のビット位置。m68k_types.h の sr_bit と同じ値。
//
// Why not sr_bit を include しないか: できるが、生成コードが埋め込む定数は
// **翻訳時に確定した数値**であって、core/ の型ではない。ここで数値として
// 書き直しておくと、テストが「core/ の定義と一致すること」を static_assert で
// 別に問える。片方を書き換えたらもう片方が落ちる形になる。
constexpr std::uint32_t kCcrC = 0x01;
constexpr std::uint32_t kCcrV = 0x02;
constexpr std::uint32_t kCcrZ = 0x04;
constexpr std::uint32_t kCcrN = 0x08;
constexpr std::uint32_t kCcrX = 0x10;

// 論理演算が壊す 4 ビット (X は保存)。setLogicFlags と同じ集合。
constexpr std::uint32_t kLogicClear = kCcrN | kCcrZ | kCcrV | kCcrC;
// ADD / SUB が壊す 5 ビット (X も書く)。
constexpr std::uint32_t kArithClear = kLogicClear | kCcrX;

// 発行の状態。1 パスぶん。
//
// リテラルは「集めるだけのパス」と「書くパス」で同じ順・同じ内容になる。
// emit 側が値を問い合わせるたびに index を返し、書くパスではその index から
// アドレスを引く。
struct Emitter
{
    std::uint8_t* out = nullptr;  // 本番のバッファ先頭。集めるパスでは nullptr
    size_t capacity = 0;
    // リテラル領域の直後 = コード先頭。集めるパスでは 0 のまま進めて
    // 「何語要るか」を数え、本番では確定値を使う。
    size_t codeBase = 0;
    size_t cursor = 0;  // codeBase からのバイト数

    std::uint32_t literals[kMaxLiterals] = {};
    size_t literalCount = 0;

    bool failed = false;

    // 同じ値のリテラルは共有する。
    //
    // 共有しなくても出るコードは正しい (読む定数は同じ) が、kMaxLiterals を
    // 超えて**発行そのものを諦める**ブロックが増える。CCR のマスクは 1 命令に
    // つき 1 語要るので、共有しないと ALU 4 個で 24 語を使い切る。
    // 正しさではなく被覆率の問題なので、テストは「kMaxOps いっぱいの
    // ブロックが発行できること」で守る。
    size_t literalIndex(std::uint32_t value)
    {
        for (size_t i = 0; i < literalCount; ++i)
        {
            if (literals[i] == value)
            {
                return i;
            }
        }
        if (literalCount >= kMaxLiterals)
        {
            failed = true;
            return 0;
        }
        literals[literalCount] = value;
        return literalCount++;
    }

    // 3 バイト命令 1 つぶんの書き先。集めるパスでは捨てる。
    std::uint8_t* slot(size_t len)
    {
        const size_t at = codeBase + cursor;
        cursor += len;
        if (out == nullptr)
        {
            return scratch;
        }
        if (at + len > capacity)
        {
            failed = true;
            return scratch;
        }
        return out + at;
    }

    // 集めるパスで書き先が要らないときの捨て場。
    std::uint8_t scratch[4] = {};
};

// 定数をレジスタへ置く。範囲に収まれば movi、収まらなければ l32r。
//
// Why not 常に l32r にしないか: movi は 3 バイトで済むのは同じだが、
// l32r はリテラルを 4 バイト消費する。kMaxLiterals は 24 しかないので、
// -2048..2047 に収まる定数 (CCR のマスク、MOVEQ の即値、小さい pc) を
// movi へ逃がさないとすぐ溢れる。
void emitConst(Emitter& e, XReg reg, std::uint32_t value)
{
    const std::int32_t s = static_cast<std::int32_t>(value);
    if (canMovi(s))
    {
        movi(e.slot(kWideLen), reg, s);
        return;
    }
    const size_t index = e.literalIndex(value);
    const size_t insnPc = e.codeBase + e.cursor;
    std::uint8_t* at = e.slot(kWideLen);
    if (e.out == nullptr)
    {
        // 集めるパス。codeBase がまだ 0 なので変位は正になり、l32r としては
        // 不正な値になる。**ここで検査してはいけない。** 長さを測ることだけが
        // 目的で、l32r は常に 3 バイトなので測る値は変わらない。
        return;
    }
    const std::int32_t off =
        l32rOffset(static_cast<std::uint32_t>(insnPc), static_cast<std::uint32_t>(index * 4));
    if (!canL32r(off))
    {
        e.failed = true;
        return;
    }
    l32r(at, reg, off);
}

// d[reg] / a[reg] のバイトオフセット。
constexpr std::uint32_t dOffset(std::uint32_t reg)
{
    return kStateDOffset + reg * 4u;
}

// サイズ (1/2/4 バイト) からビット数。
constexpr std::uint32_t sizeBits(std::uint32_t size)
{
    return size * 8u;
}

// sr の CCR を差し替える。ccrReg に新しい CCR (下位 5bit) が入っている前提。
//
// clearMask は「壊してよいビット」。X を保存するかどうかがここで決まる。
// 上位バイト (割り込みマスク / S / T) は l16ui で読んだ値をそのまま残す。
void emitStoreCcr(Emitter& e, XReg ccrReg, std::uint32_t clearMask)
{
    l16ui(e.slot(kWideLen), kTmpSr, kState, kStateSrOffset);
    // ~clearMask を 32bit の負値として作る。l16ui の結果は上位 16bit が 0 なので、
    // 上位まで 1 が立った定数と and をとっても 16bit の外は 0 のまま。
    emitConst(e, kTmpConst, static_cast<std::uint32_t>(~clearMask));
    and_(e.slot(kWideLen), kTmpSr, kTmpSr, kTmpConst);
    or_(e.slot(kWideLen), kTmpSr, kTmpSr, ccrReg);
    s16i(e.slot(kWideLen), kTmpSr, kState, kStateSrOffset);
}

// N と Z を value から作って CCR を書く (setLogicFlags 相当)。V/C はクリア、X は保存。
//
// valueReg は **size で切り出し済み**でなければならない。byte なら上位 24bit が 0、
// word なら上位 16bit が 0。切り出していない値を渡すと Z が立たなくなる。
void emitLogicFlags(Emitter& e, XReg valueReg, std::uint32_t size)
{
    // Z: 分岐を使わず moveqz で作る。
    //
    // Why not beqz で飛ばさないか: 条件分岐を挟むと、以降の命令の
    // 変位が発行順に依存する。直線のままなら、命令を足しても
    // 既に吐いたバイト列が変わらない。
    movi(e.slot(kWideLen), kTmpCcr, 0);
    movi(e.slot(kWideLen), kTmpConst, static_cast<std::int32_t>(kCcrZ));
    moveqz(e.slot(kWideLen), kTmpCcr, kTmpConst, valueReg);

    // N: 符号ビットを取り出して kCcrN の位置へ寄せる。
    extui(e.slot(kWideLen), kTmpD, valueReg, sizeBits(size) - 1u, 1u);
    slli(e.slot(kWideLen), kTmpD, kTmpD, 3u);  // kCcrN == 0x08 == 1 << 3
    or_(e.slot(kWideLen), kTmpCcr, kTmpCcr, kTmpD);

    emitStoreCcr(e, kTmpCcr, kLogicClear);
}

// d[reg] の下位 size バイトだけを valueReg で差し替える (writeEa(0, reg, size, v) 相当)。
//
// long なら丸ごと上書き。byte / word は上位を保存する必要があるので、
// 一度読んでから下位を落として or する。
//
// Why not extui で上位を切り出して slli で戻さないか: **extui の maskimm は
// 1..16 しかない。** byte なら残したい上位は 24 ビットで、範囲外の maskimm を
// 渡すと (maskimm - 1) & 0xF が折り返して**別の正当な extui** になる。
// 実際、最初はその形で書いてアセンブラに拒否された (extui a7, a7, 31, 6 は
// shiftimm + maskimm > 32 で不正)。定数マスクなら折り返す余地が無い。
void emitWriteDataRegister(Emitter& e, std::uint32_t reg, XReg valueReg, std::uint32_t size)
{
    if (size == 4)
    {
        s32i(e.slot(kWideLen), valueReg, kState, dOffset(reg));
        return;
    }
    const std::uint32_t keepMask = size == 1 ? 0xFFFFFF00u : 0xFFFF0000u;
    l32i(e.slot(kWideLen), kTmpE, kState, dOffset(reg));
    emitConst(e, kTmpConst, keepMask);
    and_(e.slot(kWideLen), kTmpE, kTmpE, kTmpConst);
    or_(e.slot(kWideLen), kTmpE, kTmpE, valueReg);
    s32i(e.slot(kWideLen), kTmpE, kState, dOffset(reg));
}

// レジスタの下位 size バイトを取り出す (上位をゼロにする)。
void emitTruncate(Emitter& e, XReg dstReg, XReg srcReg, std::uint32_t size)
{
    if (size == 4)
    {
        if (dstReg != srcReg)
        {
            movN(e.slot(kNarrowLen), dstReg, srcReg);
        }
        return;
    }
    extui(e.slot(kWideLen), dstReg, srcReg, 0u, sizeBits(size));
}

// MOVEQ #imm,Dn。
//
// 即値も、そこから決まる N / Z も **翻訳時に定数**なので、CCR は 1 つの
// 定数として置ける。実行時に何も計算しない。
void emitMoveq(Emitter& e, const PlannedOp& op)
{
    emitConst(e, kTmpA, op.imm);
    s32i(e.slot(kWideLen), kTmpA, kState, dOffset(op.dstReg));

    std::uint32_t ccr = 0;
    if (op.imm == 0)
    {
        ccr |= kCcrZ;
    }
    if ((op.imm & 0x80000000u) != 0)
    {
        ccr |= kCcrN;
    }
    movi(e.slot(kWideLen), kTmpCcr, static_cast<std::int32_t>(ccr));
    emitStoreCcr(e, kTmpCcr, kLogicClear);
}

// MOVE.b/w/l Dn,Dm。src / dst とも mode 0。
void emitMove(Emitter& e, const PlannedOp& op)
{
    l32i(e.slot(kWideLen), kTmpA, kState, dOffset(op.srcReg));
    emitTruncate(e, kTmpC, kTmpA, op.size);
    emitWriteDataRegister(e, op.dstReg, kTmpC, op.size);
    emitLogicFlags(e, kTmpC, op.size);
}

// AND / OR のレジスタ間形。フラグは setLogicFlags と同じ。
void emitLogicAlu(Emitter& e, const PlannedOp& op)
{
    l32i(e.slot(kWideLen), kTmpA, kState, dOffset(op.srcReg));
    l32i(e.slot(kWideLen), kTmpB, kState, dOffset(op.dstReg));
    if (op.aluOp == PlanAluOp::kAnd)
    {
        and_(e.slot(kWideLen), kTmpC, kTmpB, kTmpA);
    }
    else
    {
        or_(e.slot(kWideLen), kTmpC, kTmpB, kTmpA);
    }
    emitTruncate(e, kTmpC, kTmpC, op.size);
    emitWriteDataRegister(e, op.dstReg, kTmpC, op.size);
    emitLogicFlags(e, kTmpC, op.size);
}

// ADD / SUB / CMP のレジスタ間形。V と C を分岐なしのビット演算で作る。
//
// 記号: d = 切り出した被演算子 (dst)、s = 同 (src)、r = 結果、sb = 符号ビット。
//   ADD: r = (d + s) & mask
//        C = ((d & s) | ((d | s) & ~r)) & sb
//        V = (~(d ^ s) & (r ^ d)) & sb
//   SUB: r = (d - s) & mask
//        C = ((~d & s) | (~(d ^ s) & r)) & sb
//        V = ((d ^ s) & (r ^ d)) & sb
//
// この式が alu::add / alu::sub と一致することは、ホストのテストが
// byte を全数 (65,536 通り)、word / long を境界値で突き合わせて確かめる。
// **式を読んで納得しても、それだけでは根拠にしない。**
//
// CMP は値を書かず X も触らない (applyFlags(sr, r, false))。
// ADD / SUB は X に C と同じ値を書く。
void emitArithAlu(Emitter& e, const PlannedOp& op)
{
    const std::uint32_t size = op.size;
    const std::uint32_t bits = sizeBits(size);
    const bool isAdd = op.aluOp == PlanAluOp::kAdd;
    const bool isCmp = op.aluOp == PlanAluOp::kCmp;

    // d と s を切り出す。
    //
    // Why not 「結果だけ切り詰めれば同じ」で省かないか: 実際、下の V / C の式は
    // **結果 r さえ切り詰めてあれば被演算子の上位ビットに依存しない**
    // (32bit 全域の乱択 400,000 通りで一致を確認済み)。省いても現状のテストは
    // 全部通る。それでも切り出すのは、この等価性が「r を切り詰めている」ことに
    // 依存しているから。段 3 でレジスタに値を寝かせる最適化を入れて r の
    // 切り詰めを出口へ移した瞬間、V と C が上位の桁を拾い始める。
    // **等価性が別の場所の実装に依存している形は、崩れたことに気づけない。**
    l32i(e.slot(kWideLen), kTmpA, kState, dOffset(op.srcReg));
    l32i(e.slot(kWideLen), kTmpB, kState, dOffset(op.dstReg));
    emitTruncate(e, kTmpA, kTmpA, size);  // s
    emitTruncate(e, kTmpB, kTmpB, size);  // d

    // r
    if (isAdd)
    {
        addN(e.slot(kNarrowLen), kTmpC, kTmpB, kTmpA);
    }
    else
    {
        sub(e.slot(kWideLen), kTmpC, kTmpB, kTmpA);
    }
    emitTruncate(e, kTmpC, kTmpC, size);

    // V を先に作る。d ^ s と r ^ d の 2 つは C でも使い回したいが、
    // 使い回すとレジスタが足りなくなるので作り直す (命令 1 つの差)。
    xor_(e.slot(kWideLen), kTmpD, kTmpB, kTmpA);  // d ^ s
    xor_(e.slot(kWideLen), kTmpE, kTmpC, kTmpB);  // r ^ d
    if (isAdd)
    {
        // ~(d ^ s) & (r ^ d)
        neg(e.slot(kWideLen), kTmpD, kTmpD);
        addi(e.slot(kWideLen), kTmpD, kTmpD, -1);  // -x - 1 == ~x
    }
    and_(e.slot(kWideLen), kTmpD, kTmpD, kTmpE);
    // 符号ビットだけを残して kCcrV の位置 (bit1) へ寄せる。
    extui(e.slot(kWideLen), kTmpD, kTmpD, bits - 1u, 1u);
    slli(e.slot(kWideLen), kTmpD, kTmpD, 1u);
    movN(e.slot(kNarrowLen), kTmpCcr, kTmpD);

    // C。
    if (isAdd)
    {
        // (d & s) | ((d | s) & ~r)
        and_(e.slot(kWideLen), kTmpD, kTmpB, kTmpA);
        or_(e.slot(kWideLen), kTmpE, kTmpB, kTmpA);
        neg(e.slot(kWideLen), kTmpConst, kTmpC);
        addi(e.slot(kWideLen), kTmpConst, kTmpConst, -1);  // ~r
        and_(e.slot(kWideLen), kTmpE, kTmpE, kTmpConst);
        or_(e.slot(kWideLen), kTmpD, kTmpD, kTmpE);
    }
    else
    {
        // (~d & s) | (~(d ^ s) & r)
        neg(e.slot(kWideLen), kTmpConst, kTmpB);
        addi(e.slot(kWideLen), kTmpConst, kTmpConst, -1);  // ~d
        and_(e.slot(kWideLen), kTmpD, kTmpConst, kTmpA);
        xor_(e.slot(kWideLen), kTmpE, kTmpB, kTmpA);
        neg(e.slot(kWideLen), kTmpConst, kTmpE);
        addi(e.slot(kWideLen), kTmpConst, kTmpConst, -1);  // ~(d ^ s)
        and_(e.slot(kWideLen), kTmpE, kTmpConst, kTmpC);
        or_(e.slot(kWideLen), kTmpD, kTmpD, kTmpE);
    }
    extui(e.slot(kWideLen), kTmpD, kTmpD, bits - 1u, 1u);  // C は bit0 なので寄せ直し不要
    or_(e.slot(kWideLen), kTmpCcr, kTmpCcr, kTmpD);

    // X は C と同じ値。CMP は書かない。
    if (!isCmp)
    {
        slli(e.slot(kWideLen), kTmpE, kTmpD, 4u);  // kCcrX == 0x10 == 1 << 4
        or_(e.slot(kWideLen), kTmpCcr, kTmpCcr, kTmpE);
    }

    // N
    extui(e.slot(kWideLen), kTmpD, kTmpC, bits - 1u, 1u);
    slli(e.slot(kWideLen), kTmpD, kTmpD, 3u);
    or_(e.slot(kWideLen), kTmpCcr, kTmpCcr, kTmpD);

    // Z
    movi(e.slot(kWideLen), kTmpConst, static_cast<std::int32_t>(kCcrZ));
    or_(e.slot(kWideLen), kTmpD, kTmpCcr, kTmpConst);
    moveqz(e.slot(kWideLen), kTmpCcr, kTmpD, kTmpC);

    emitStoreCcr(e, kTmpCcr, isCmp ? kLogicClear : kArithClear);

    // CMP は結果を書かない。
    if (!isCmp)
    {
        emitWriteDataRegister(e, op.dstReg, kTmpC, size);
    }
}

// 非分岐終端 / 分岐不成立の出口。pc / ir / irc を定数で書き、サイクル数を返す。
void emitFallThroughExit(Emitter& e, const BlockPlan& plan, std::uint16_t ir, std::uint16_t irc,
                         std::uint32_t cycles)
{
    emitConst(e, kTmpA, plan.fallThroughPc + 4u);
    s32i(e.slot(kWideLen), kTmpA, kState, kStatePcOffset);
    emitConst(e, kTmpA, ir);
    s16i(e.slot(kWideLen), kTmpA, kState, kStateIrOffset);
    emitConst(e, kTmpA, irc);
    s16i(e.slot(kWideLen), kTmpA, kState, kStateIrcOffset);
    emitConst(e, kRet, cycles);
    retN(e.slot(kNarrowLen));
}

// 分岐の条件コードを評価して CCR から真偽を作る。
//
// 結果は resultReg に 0 (不成立) か 非0 (成立) として入る。
// **CCR のビットを直接組み合わせる。** testCondition の switch を
// そのまま写すと 16 通りの分岐が要るが、条件はすべて C/V/Z/N の
// 論理式なので、ビット演算だけで出せる。
//
// 実装は「各フラグを 0/1 に落としてから式を組む」。ビット位置のまま
// 組もうとすると、位置の違う 2 つを比べる場面 (GE の n == v) で
// 必ずシフトが要り、かえって長くなる。
void emitCondition(Emitter& e, std::uint32_t cond, XReg resultReg)
{
    // 常に真 (BRA)。段 1 は cond == 1 (BSR) を積まないので、ここには来ない。
    if (cond == 0)
    {
        movi(e.slot(kWideLen), resultReg, 1);
        return;
    }

    l16ui(e.slot(kWideLen), kTmpSr, kState, kStateSrOffset);
    extui(e.slot(kWideLen), kTmpA, kTmpSr, 0u, 1u);  // C
    extui(e.slot(kWideLen), kTmpB, kTmpSr, 1u, 1u);  // V
    extui(e.slot(kWideLen), kTmpC, kTmpSr, 2u, 1u);  // Z
    extui(e.slot(kWideLen), kTmpD, kTmpSr, 3u, 1u);  // N

    // 反転は「1 との xor」で作る。~ は 32bit 全体を反転するので、
    // 0/1 に落とした値に使うと 0xFFFFFFFE になって or の結果が壊れる。
    const auto notBit = [&e](XReg dst, XReg src)
    {
        movi(e.slot(kWideLen), kTmpConst, 1);
        xor_(e.slot(kWideLen), dst, src, kTmpConst);
    };

    switch (cond)
    {
        case 0x2:  // HI: !c && !z
            or_(e.slot(kWideLen), resultReg, kTmpA, kTmpC);
            notBit(resultReg, resultReg);
            break;
        case 0x3:  // LS: c || z
            or_(e.slot(kWideLen), resultReg, kTmpA, kTmpC);
            break;
        case 0x4:  // CC: !c
            notBit(resultReg, kTmpA);
            break;
        case 0x5:  // CS: c
            movN(e.slot(kNarrowLen), resultReg, kTmpA);
            break;
        case 0x6:  // NE: !z
            notBit(resultReg, kTmpC);
            break;
        case 0x7:  // EQ: z
            movN(e.slot(kNarrowLen), resultReg, kTmpC);
            break;
        case 0x8:  // VC: !v
            notBit(resultReg, kTmpB);
            break;
        case 0x9:  // VS: v
            movN(e.slot(kNarrowLen), resultReg, kTmpB);
            break;
        case 0xA:  // PL: !n
            notBit(resultReg, kTmpD);
            break;
        case 0xB:  // MI: n
            movN(e.slot(kNarrowLen), resultReg, kTmpD);
            break;
        case 0xC:  // GE: n == v
            xor_(e.slot(kWideLen), resultReg, kTmpD, kTmpB);
            notBit(resultReg, resultReg);
            break;
        case 0xD:  // LT: n != v
            xor_(e.slot(kWideLen), resultReg, kTmpD, kTmpB);
            break;
        case 0xE:  // GT: !z && (n == v)
            xor_(e.slot(kWideLen), kTmpE, kTmpD, kTmpB);
            or_(e.slot(kWideLen), resultReg, kTmpE, kTmpC);
            notBit(resultReg, resultReg);
            break;
        default:  // 0xF LE: z || (n != v)
            xor_(e.slot(kWideLen), kTmpE, kTmpD, kTmpB);
            or_(e.slot(kWideLen), resultReg, kTmpE, kTmpC);
            break;
    }
}

// 分岐終端。条件を評価し、成立なら bit31 を立てた成立側サイクルを返す。
//
// **成立側では pc / ir / irc を書かない。** branchTarget のプリフェッチは
// ページの外を読みうるので、呼び出し側が M68k::branchTo で詰め直す (§5.2)。
void emitBranchExit(Emitter& e, const BlockPlan& plan, std::uint16_t ir, std::uint16_t irc)
{
    emitCondition(e, plan.ops[plan.count - 1].cond, kTmpC);

    // 不成立なら前方へ飛ばす。飛び先は「成立側の出口の直後」。
    // 変位は 2 パス目で確定するので、先に不成立側の長さを測る。
    //
    // Why not 成立側を後ろに置いて beqz で飛び越えないか: 置き方は
    // どちらでもよいが、こちらだと**不成立側 (頻度の高い側) が
    // 分岐 1 本ぶん遠くなる**。ここでは正しさを優先して、
    // 「成立側を先に置き、不成立なら跨ぐ」形にそろえる。
    const size_t branchInsnPc = e.codeBase + e.cursor;
    std::uint8_t* branchSlot = e.slot(kWideLen);

    // --- 成立側 ---
    emitConst(e, kRet, plan.cyclesTaken | kBranchTakenFlag);
    retN(e.slot(kNarrowLen));

    // --- 不成立側 ---
    const size_t notTakenPc = e.codeBase + e.cursor;
    const std::int32_t disp = static_cast<std::int32_t>(notTakenPc) -
                              (static_cast<std::int32_t>(branchInsnPc) + kBranchOrigin);
    if (!canBranch12(disp))
    {
        e.failed = true;
    }
    else
    {
        // 条件が 0 (不成立) なら不成立側へ。
        beqz(branchSlot, kTmpC, disp);
    }
    emitFallThroughExit(e, plan, ir, irc, plan.cyclesNotTaken);
}

// ブロック 1 本を吐く。集めるパスと本番で同じ経路を通る。
void emitAll(Emitter& e, const BlockPlan& plan, std::uint16_t ir, std::uint16_t irc)
{
    // プロローグ: 引数 a2 を kState へ写す。以降 a2 は戻り値の置き場。
    movN(e.slot(kNarrowLen), kState, kRet);

    const bool endsWithBranch = plan.end == BlockEnd::kBranch;
    // 分岐は必ず末尾 (I6) なので、本体として吐くのはその手前まで。
    const std::uint32_t bodyCount = endsWithBranch ? plan.count - 1u : plan.count;

    for (std::uint32_t i = 0; i < bodyCount; ++i)
    {
        const PlannedOp& op = plan.ops[i];
        switch (op.kind)
        {
            case PlanKind::kMoveRegToReg:
                emitMove(e, op);
                break;
            case PlanKind::kMoveq:
                emitMoveq(e, op);
                break;
            case PlanKind::kAluRegToReg:
                if (op.aluOp == PlanAluOp::kAnd || op.aluOp == PlanAluOp::kOr)
                {
                    emitLogicAlu(e, op);
                }
                else if (op.aluOp == PlanAluOp::kAdd || op.aluOp == PlanAluOp::kSub ||
                         op.aluOp == PlanAluOp::kCmp)
                {
                    emitArithAlu(e, op);
                }
                else
                {
                    // kEor は段 1 の翻訳器が積まない (命令長デコーダが $B の
                    // 特殊形を判別できないため)。積まれたなら翻訳器と
                    // エミッタの対応範囲がずれているので、諦める。
                    e.failed = true;
                }
                break;
            case PlanKind::kBranch:
                // I6 が「分岐は末尾のみ」を保証しているので、本体には来ない。
                e.failed = true;
                break;
        }
        if (e.failed)
        {
            return;
        }
    }

    if (endsWithBranch)
    {
        emitBranchExit(e, plan, ir, irc);
    }
    else
    {
        emitFallThroughExit(e, plan, ir, irc, plan.cyclesNotTaken);
    }
}

// リテラルを集めるパスを回して、必要な語数とコード長を得る。
bool measure(const BlockPlan& plan, std::uint16_t ir, std::uint16_t irc, size_t& literalCount,
             size_t& codeLen)
{
    if (plan.count == 0 || plan.count > kMaxOps)
    {
        return false;
    }
    Emitter e{};
    emitAll(e, plan, ir, irc);
    if (e.failed)
    {
        return false;
    }
    literalCount = e.literalCount;
    codeLen = e.cursor;
    return true;
}

}  // namespace

size_t requiredSize(const BlockPlan& plan, std::uint16_t fallThroughIr,
                    std::uint16_t fallThroughIrc)
{
    size_t literalCount = 0;
    size_t codeLen = 0;
    if (!measure(plan, fallThroughIr, fallThroughIrc, literalCount, codeLen))
    {
        return 0;
    }
    // 1 パス目の codeBase は 0 なので、l32r の変位が 2 パス目とは違う。
    // 変位が違っても命令長は 3 バイト固定なので**コード長は変わらない**。
    // リテラル領域だけが前に積まれる。
    return literalCount * 4u + codeLen;
}

bool emitBlock(const BlockPlan& plan, std::uint16_t fallThroughIr, std::uint16_t fallThroughIrc,
               std::uint8_t* out, size_t capacity, EmittedBlock& result)
{
    size_t literalCount = 0;
    size_t codeLen = 0;
    if (!measure(plan, fallThroughIr, fallThroughIrc, literalCount, codeLen))
    {
        return false;
    }

    const size_t codeBase = literalCount * 4u;
    const size_t total = codeBase + codeLen;
    if (out == nullptr || total > capacity)
    {
        return false;
    }

    Emitter e{};
    e.out = out;
    e.capacity = capacity;
    e.codeBase = codeBase;
    emitAll(e, plan, fallThroughIr, fallThroughIrc);
    if (e.failed || e.cursor != codeLen || e.literalCount != literalCount)
    {
        // 2 パスで結果が食い違ったら、l32r の変位がどこかで狂っている。
        // **黙って走らせない。** 段 1 の翻訳器と同じく、怪しければ諦める。
        return false;
    }

    // リテラルを先頭へ書く。**メモリ順のリトルエンディアン**で置く
    // (l32r が読むのは 32bit 語なので、CPU のバイト順に従う)。
    for (size_t i = 0; i < literalCount; ++i)
    {
        const std::uint32_t v = e.literals[i];
        out[i * 4 + 0] = static_cast<std::uint8_t>(v & 0xFFu);
        out[i * 4 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        out[i * 4 + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        out[i * 4 + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    }

    result.entryOffset = codeBase;
    result.totalSize = total;
    result.branchTarget = plan.branchTarget;
    result.endsWithBranch = plan.end == BlockEnd::kBranch;
    return true;
}

}  // namespace x68k::jit
