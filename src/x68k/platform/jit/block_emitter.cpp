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

// ガード不成立で降りる出口 1 つぶんの控え。
//
// 分岐 (bnez) は出口の島より**前**に発行されるので、変位はその時点では
// 決まらない。書き先だけ控えておき、島を吐き終えてからパッチする
// (emitBranchExit が既に使っている「slot を控えて後から書く」方式と同じ)。
struct PendingGuard
{
    std::uint8_t* slot = nullptr;  // 分岐を書き込む場所
    size_t insnPc = 0;             // その分岐の位置 (codeBase 相対)
    std::uint32_t opIndex = 0;     // 何番目の命令のガードか
    // 分岐が「0 のとき飛ぶ」か (beqz)。false なら bnez。
    //
    // 自ページ判定は「page(a) - plan.page == 0 なら脱出」なので beqz、
    // 範囲・整列の判定は「失敗ビットが立ったら脱出」なので bnez。
    bool branchOnZero = false;
    // 分岐に使うレジスタ。範囲ガードは kTmpD、自ページガードは kTmpE を使う。
    XReg reg = 0;
    // 島の戻り値へ足す追加のビット (kSelfPageExitFlag)。
    //
    // **島は (opIndex, extraRetFlags) の組で分かれる。** 同じ命令でも
    // 通常の脱出と自ページ脱出では戻り値が違うので、同じ島へは飛ばせない。
    std::uint32_t extraRetFlags = 0;
};

// ガード付き命令は 1 つにつき分岐 1 本 (読み) か最大 3 本 (書きの `.l`:
// 範囲 + 自ページ page(a) + 自ページ page(a+3))。kMaxOps = 4 なので
// 4 x 3 = 12 が上限。溢れたら failed で諦める。
constexpr size_t kMaxPendingGuards = 12;

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

    // 翻訳時の窓。読み形のガードと、焼き込むホストアドレスの出どころ。
    EmitEnv env{};

    // ガード脱出の控え。発行順に積み、出口の島を吐いてからパッチする。
    PendingGuard guards[kMaxPendingGuards] = {};
    size_t guardCount = 0;

    bool failed = false;

    // ガードの分岐を 1 本控える。書き先は呼び出し側が slot() で取る。
    void addGuard(std::uint8_t* slot, size_t insnPc, std::uint32_t opIndex, bool branchOnZero,
                  XReg reg, std::uint32_t extraRetFlags)
    {
        if (guardCount >= kMaxPendingGuards)
        {
            failed = true;
            return;
        }
        guards[guardCount] = PendingGuard{slot, insnPc, opIndex, branchOnZero, reg, extraRetFlags};
        ++guardCount;
    }

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

// a[reg] のバイトオフセット。最大 60 で s32i の範囲に収まり、
// usp (64) / ssp (68) とは重ならない。
constexpr std::uint32_t aOffset(std::uint32_t reg)
{
    return kStateAOffset + reg * 4u;
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
// 下位 16bit を符号拡張する。dst == src でもよい。**kTmpD を潰す。**
//
// Why not srai をエンコーダに足さないか: 足すと xtensa_encoder.h と
// テストのミニ解釈器とエンコーダのテストの 3 箇所に検証面が増える。
// extui / slli / sub は全部既存なので、追加の検証がゼロで済む。
// 命令数の差は 2 で、MOVEA.w でしか通らない。
//
// 仕組み: w = 下位 16bit、sign = bit15 として w - (sign << 16)。
// w=0x8000 なら 0x8000 - 0x10000 = 0xFFFF8000。w=0x7FFF ならそのまま。
void emitSext16(Emitter& e, XReg dst, XReg src)
{
    extui(e.slot(kWideLen), dst, src, 0u, 16u);
    extui(e.slot(kWideLen), kTmpD, src, 15u, 1u);
    slli(e.slot(kWideLen), kTmpD, kTmpD, 16u);
    sub(e.slot(kWideLen), dst, dst, kTmpD);
}

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

// MOVE.w/l An,Dn。転送元が a[] になるだけで、あとは MOVE と同じ。
//
// interpreter は readEa mode 1 で符号拡張した値を writeEa / setLogicFlags へ
// 渡すが、どちらも内部で size にマスクするので、切り詰めた値で等価。
void emitMoveAregToDreg(Emitter& e, const PlannedOp& op)
{
    l32i(e.slot(kWideLen), kTmpA, kState, aOffset(op.srcReg));
    emitTruncate(e, kTmpC, kTmpA, op.size);
    emitWriteDataRegister(e, op.dstReg, kTmpC, op.size);
    emitLogicFlags(e, kTmpC, op.size);
}

// MOVE.b/w/l #imm,Dn。**CCR は翻訳時に決まる。**
//
// 即値は planner が size でマスク済みなので、N と Z をここで畳める。
void emitMoveImmToDreg(Emitter& e, const PlannedOp& op)
{
    emitConst(e, kTmpA, op.imm);
    emitWriteDataRegister(e, op.dstReg, kTmpA, op.size);

    const std::uint32_t signBit = 1u << (sizeBits(op.size) - 1u);
    std::uint32_t ccr = 0;
    if (op.imm == 0)
    {
        ccr |= kCcrZ;
    }
    if ((op.imm & signBit) != 0)
    {
        ccr |= kCcrN;
    }
    emitConst(e, kTmpCcr, ccr);
    emitStoreCcr(e, kTmpCcr, kLogicClear);
}

// MOVEA.w/l。**フラグを 1 つも変えない。**
//
// emitStoreCcr を呼ばないことがそのまま「フラグ不変」の実装になっている。
// MOVE と同じだろうと思って emitLogicFlags を足すと壊れる。
//
// .w は符号拡張して 32bit 全体を書く (m68k_ops_move.cpp の MOVEA 経路)。
void emitMovea(Emitter& e, const PlannedOp& op, bool srcIsAddressRegister)
{
    const std::uint32_t src = srcIsAddressRegister ? aOffset(op.srcReg) : dOffset(op.srcReg);
    l32i(e.slot(kWideLen), kTmpA, kState, src);
    if (op.size == 2)
    {
        emitSext16(e, kTmpA, kTmpA);
    }
    s32i(e.slot(kWideLen), kTmpA, kState, aOffset(op.dstReg));
}

// MOVEA #imm,An と LEA (xxx).W/L,An。どちらも「定数を a[] へ入れる」だけ。
// 符号拡張は翻訳時に済んでいる。フラグは変えない。
void emitLoadAregConst(Emitter& e, const PlannedOp& op)
{
    emitConst(e, kTmpA, op.imm);
    s32i(e.slot(kWideLen), kTmpA, kState, aOffset(op.dstReg));
}

// LEA (An),An / (d16,An),An。**アドレスを求めるだけで読まない。**
// フラグは変えない。
void emitLeaDisp(Emitter& e, const PlannedOp& op)
{
    l32i(e.slot(kWideLen), kTmpA, kState, aOffset(op.srcReg));
    if (op.imm != 0)
    {
        // 変位が 0 かどうかは翻訳時に決まるので、生成コードは決定的。
        emitConst(e, kTmpB, op.imm);
        addN(e.slot(kNarrowLen), kTmpA, kTmpA, kTmpB);
    }
    s32i(e.slot(kWideLen), kTmpA, kState, aOffset(op.dstReg));
}

// TST.b/w/l Dn。**読むだけで d[] には書かない。**
// N/Z を立て V/C をクリア。X は保存 (kLogicClear に X が無い)。
void emitTstDreg(Emitter& e, const PlannedOp& op)
{
    l32i(e.slot(kWideLen), kTmpA, kState, dOffset(op.srcReg));
    emitTruncate(e, kTmpC, kTmpA, op.size);
    emitLogicFlags(e, kTmpC, op.size);
}

// CLR.b/w/l Dn。**CCR は翻訳時に決まる** (結果が必ず 0 なので Z=1)。
//
// 68000 の CLR は読んでから書く RMW だが、mode 0 の読みは
// readEaForModify が d[] を返すだけで副作用が無い。だから読まずに書ける。
void emitClrDreg(Emitter& e, const PlannedOp& op)
{
    emitConst(e, kTmpC, 0);
    emitWriteDataRegister(e, op.dstReg, kTmpC, op.size);
    emitConst(e, kTmpCcr, kCcrZ);
    emitStoreCcr(e, kTmpCcr, kLogicClear);
}

// AND / OR。**src は kTmpA に載っている前提。**
//
// レジスタ間形とメモリ読み形で、フラグの式も切り詰めも同じでなければ
// ならない。src の出どころだけが違うので、載せるところを呼び出し側に
// 出して本体を 1 本にする。
void emitLogicAluCore(Emitter& e, const PlannedOp& op)
{
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

// AND / OR のレジスタ間形。
void emitLogicAlu(Emitter& e, const PlannedOp& op)
{
    l32i(e.slot(kWideLen), kTmpA, kState, dOffset(op.srcReg));
    emitLogicAluCore(e, op);
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
// **src は kTmpA に載っている前提。**
void emitArithAluCore(Emitter& e, const PlannedOp& op)
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

// ADD / SUB / CMP のレジスタ間形。
void emitArithAlu(Emitter& e, const PlannedOp& op)
{
    l32i(e.slot(kWideLen), kTmpA, kState, dOffset(op.srcReg));
    emitArithAluCore(e, op);
}

// 演算種別で本体を選ぶ。**レジスタ間形とメモリ読み形で同じ関数を通す。**
// 片方だけに命令を足したときに食い違わないよう、選び方を 1 箇所にする。
void emitAluBody(Emitter& e, const PlannedOp& op)
{
    const bool isLogic = op.aluOp == PlanAluOp::kAnd || op.aluOp == PlanAluOp::kOr;
    if (isLogic)
    {
        emitLogicAluCore(e, op);
        return;
    }
    const bool isArith =
        op.aluOp == PlanAluOp::kAdd || op.aluOp == PlanAluOp::kSub || op.aluOp == PlanAluOp::kCmp;
    if (isArith)
    {
        emitArithAluCore(e, op);
        return;
    }
    // kEor は翻訳器が積まない。積まれたなら対応範囲がずれているので諦める。
    e.failed = true;
}

// --- Tier B: 読みガード ----------------------------------------------------
//
// ここから下の 4 つが「メモリを読む形」の全部。守る不変条件は 3 つ:
//
//   G1 読みの同値   ガードが通ったとき読むバイト列は、インタプリタの
//                   read8/16/32 が fast path を通ったときと同一
//   G3 commit の順  アーキテクチャ状態への書き込みは、最後のガード分岐より
//                   後にしか現れない ((An)+ / -(An) の An 更新を含む)
//   G7 出口の状態   降りる先は「その命令の直前の命令境界」

// ゲストアドレスの 24bit マスク。read8/16/32 が addr & kAddrMask で
// 始めるのと同じ (m68k.cpp:256 / 271 / 301)。
constexpr std::uint32_t kGuestAddrMask = 0x00FFFFFFu;

// (An)+ / -(An) の増減幅。**A7 をバイトで触るときだけ 2。**
// m68k.h:440-458 の step と同じ式。スタックポインタが奇数になると
// アドレスエラーになるための特例。
// **An 番号は eaRegOf() から取る。** 読み形は srcReg、書き形は dstReg。
// 直書きに戻すと、書き形で「関係ないアドレスレジスタが 2 進む」形になる。
constexpr std::uint32_t eaStep(const PlannedOp& op)
{
    const bool isStackPointerByte = eaRegOf(op) == 7 && op.size == 1;
    return isStackPointerByte ? 2u : op.size;
}

// 読み形かどうか。
constexpr bool isMemoryRead(const PlannedOp& op)
{
    return op.eaMode != kEaNone;
}

// 絶対アドレス形 (7.0 / 7.1) は実効アドレスが翻訳時定数。
constexpr bool isAbsoluteEa(std::uint8_t eaMode)
{
    return eaMode == kEaAbsShort || eaMode == kEaAbsLong;
}

// G1 の条件を**翻訳時に**評価する (絶対アドレス形と、範囲の事前判定に使う)。
//
// byte : a < limit
// word : (a & 1) == 0 かつ a + 1 < limit
// long : (a & 1) == 0 かつ a + 3 < limit
//
// **奇数の判定を範囲より先に見る。** read16 / read32 が
// takeAddressError を範囲判定より前に置いているのと同じ順序 (m68k.cpp:272/302)。
bool guestReadFits(const EmitEnv& env, std::uint32_t addr, std::uint32_t size)
{
    const std::uint32_t a = addr & kGuestAddrMask;
    const bool misaligned = size != 1 && (a & 1u) != 0;
    if (misaligned)
    {
        return false;
    }
    // a + size - 1 < limit。**桁あふれさせない**ので引き算の形で見る。
    if (env.ramLimit < size)
    {
        return false;
    }
    return a <= env.ramLimit - size;
}

// 実効アドレスを kTmpA へ作る。**状態は 1 bit も変えない** (G3)。
//
// mode 3 / 4 の An 更新はここではやらない。ガードが通ってから
// emitCommitAddressRegister が行う (G4)。
//
// 無マスクの値 (commit に使う) を kTmpB に、マスク済みの
// アクセス用アドレスを kTmpA に置く。
void emitEffectiveAddress(Emitter& e, const PlannedOp& op)
{
    const std::uint32_t step = eaStep(op);

    l32i(e.slot(kWideLen), kTmpB, kState, aOffset(eaRegOf(op)));
    if (op.eaMode == kEaPreDec)
    {
        // -(An) は**引いてから**アクセスする (m68k.h:454-457)。
        // 32bit 無マスクの環算なので、0 からのラップもそのまま再現される。
        if (!canAddi(-static_cast<std::int32_t>(step)))
        {
            e.failed = true;
            return;
        }
        addi(e.slot(kWideLen), kTmpB, kTmpB, -static_cast<std::int32_t>(step));
    }
    else if (op.eaMode == kEaDisp16)
    {
        // (d16,An) = An + sext16(d16)。d16 は翻訳時定数 (m68k.cpp:514-518)。
        if (op.imm != 0)
        {
            emitConst(e, kTmpConst, op.imm);
            addN(e.slot(kNarrowLen), kTmpB, kTmpB, kTmpConst);
        }
    }

    // アクセスに使うのはマスク済みの値だけ。**commit する値はマスクしない**
    // (インタプリタも a[] には無マスクの値を書く)。
    emitConst(e, kTmpConst, kGuestAddrMask);
    and_(e.slot(kWideLen), kTmpA, kTmpB, kTmpConst);
}

// ガード列。失敗を 1 本のビットへ畳んで分岐 1 つで降りる。
//
// **ここを通るまで状態を書かない** (G3)。分岐の変位は出口の島を吐いてから
// パッチするので、ここでは場所だけ確保する。
void emitReadGuard(Emitter& e, const PlannedOp& op, std::uint32_t opIndex)
{
    const std::uint32_t size = op.size;

    // 失敗ビットを kTmpD へ集める。
    const bool needsAlignment = size != 1;
    if (needsAlignment)
    {
        // 奇数なら bit0 が立つ。word / long だけが見る (read16:272 / read32:302)。
        extui(e.slot(kWideLen), kTmpD, kTmpA, 0u, 1u);
    }

    // 範囲: (limit - size) - a の符号ビットが立てば範囲外。
    //
    // **limit - size は翻訳時定数**にできる。limit < size なら窓が
    // その読みを絶対に許さないので、翻訳ごと諦める (下の呼び出し側で弾く)。
    const std::uint32_t bound = e.env.ramLimit - size;
    emitConst(e, kTmpConst, bound);
    sub(e.slot(kWideLen), kTmpE, kTmpConst, kTmpA);
    extui(e.slot(kWideLen), kTmpE, kTmpE, 31u, 1u);

    if (needsAlignment)
    {
        or_(e.slot(kWideLen), kTmpD, kTmpD, kTmpE);
    }
    else
    {
        movN(e.slot(kNarrowLen), kTmpD, kTmpE);
    }

    // 失敗なら出口の島へ。**変位は後でパッチする。**
    const size_t insnPc = e.codeBase + e.cursor;
    std::uint8_t* slot = e.slot(kWideLen);
    e.addGuard(slot, insnPc, opIndex, /*branchOnZero=*/false, kTmpD, /*extraRetFlags=*/0u);
}

// (An)+ / -(An) の An 更新を確定させる (G4)。**ガードが通ってから呼ぶ。**
//
// 値は 32bit 無マスクの環算。アクセス用に作ったマスク済みアドレスではなく、
// kTmpB に残した無マスクの値を使う。
void emitCommitAddressRegister(Emitter& e, const PlannedOp& op)
{
    if (op.eaMode == kEaPostInc)
    {
        // (An)+ は**読んだ後のアドレス**を書く。kTmpB は増やす前の値。
        const std::uint32_t step = eaStep(op);
        if (!canAddi(static_cast<std::int32_t>(step)))
        {
            e.failed = true;
            return;
        }
        addi(e.slot(kWideLen), kTmpC, kTmpB, static_cast<std::int32_t>(step));
        s32i(e.slot(kWideLen), kTmpC, kState, aOffset(eaRegOf(op)));
        return;
    }
    if (op.eaMode == kEaPreDec)
    {
        // -(An) は既に引いた値がアクセス先そのもの。
        s32i(e.slot(kWideLen), kTmpB, kState, aOffset(eaRegOf(op)));
    }
}

// ホストのバイト列から値を組む。**ビッグエンディアンで組む** (G1)。
//
// hostBase が翻訳時定数のときは l32r 1 本で済ませ、そうでなければ
// 窓の先頭 + マスク済みアドレスを足す。読み先は kTmpA (マスク済み) 側。
//
// Why not l16ui / l32i で一気に読まないか: ゲストはビッグエンディアンで、
// ホスト (Xtensa) はリトルエンディアン。まとめて読むとバイトが逆になり、
// さらにホスト側の整列も前提に入る。バイトずつなら
// m68k.cpp:282 / 311-312 と同じ式をそのまま写せる。
void emitBigEndianLoad(Emitter& e, std::uint32_t size, bool addressIsConst,
                       std::uint32_t constHostAddr)
{
    if (addressIsConst)
    {
        emitConst(e, kTmpE, constHostAddr);
    }
    else
    {
        emitConst(e, kTmpConst, e.env.ramBaseAddr);
        addN(e.slot(kNarrowLen), kTmpE, kTmpConst, kTmpA);
    }

    if (size == 1)
    {
        l8ui(e.slot(kWideLen), kTmpC, kTmpE, 0u);
        return;
    }

    // 上位バイトから順に読み、8 ビットずつ寄せて or する。
    l8ui(e.slot(kWideLen), kTmpC, kTmpE, 0u);
    for (std::uint32_t i = 1; i < size; ++i)
    {
        l8ui(e.slot(kWideLen), kTmpD, kTmpE, i);
        slli(e.slot(kWideLen), kTmpC, kTmpC, 8u);
        or_(e.slot(kWideLen), kTmpC, kTmpC, kTmpD);
    }
}

// ALU の本体を「src が既にレジスタに載っている」形で出す。
//
// emitLogicAlu / emitArithAlu との違いは src の出どころだけ。フラグの式は
// 同じでなければならないので、共通の本体 (emitAluBody) を両方から呼ぶ。
void emitAluWithSrcInReg(Emitter& e, const PlannedOp& op, XReg srcReg)
{
    if (srcReg != kTmpA)
    {
        movN(e.slot(kNarrowLen), kTmpA, srcReg);
    }
    emitAluBody(e, op);
}

// 読み形 1 命令。ガード → commit → 読み → 本体、の順に出す。
//
// **順序が契約そのもの。** commit (An 更新) と本体 (d/sr への書き込み) は
// どちらもアーキテクチャ状態を変えるので、ガードの分岐より後にしか
// 現れてはいけない (G3)。
void emitMemoryRead(Emitter& e, const PlannedOp& op, std::uint32_t opIndex)
{
    const std::uint32_t size = op.size;
    const bool absolute = isAbsoluteEa(op.eaMode);

    if (absolute)
    {
        // G6: 絶対アドレスは実効アドレスが翻訳時定数。**ガードを翻訳時に
        // 評価する。** 成立ならガード無しで焼き、ホストアドレスまで
        // 定数に畳む。不成立の命令はここへ来ない (呼び出し側が積まない)。
        const std::uint32_t addr = op.imm & kGuestAddrMask;
        if (!guestReadFits(e.env, addr, size))
        {
            e.failed = true;
            return;
        }
        emitBigEndianLoad(e, size, /*addressIsConst=*/true, e.env.ramBaseAddr + addr);
    }
    else
    {
        emitEffectiveAddress(e, op);
        if (e.failed)
        {
            return;
        }
        emitReadGuard(e, op, opIndex);
        // --- ここから下はガードが通ったときだけ走る ---
        emitCommitAddressRegister(e, op);
        if (e.failed)
        {
            return;
        }
        emitBigEndianLoad(e, size, /*addressIsConst=*/false, 0u);
    }

    // 読めた値は kTmpC に、size で切り出し済みの形で入っている
    // (バイトから組んだので上位は必ず 0)。
    switch (op.kind)
    {
        case PlanKind::kMoveMemToDreg:
            emitWriteDataRegister(e, op.dstReg, kTmpC, size);
            emitLogicFlags(e, kTmpC, size);
            return;
        case PlanKind::kTstMem:
            // **読むだけで書かない。**
            emitLogicFlags(e, kTmpC, size);
            return;
        case PlanKind::kAluMemToDreg:
            emitAluWithSrcInReg(e, op, kTmpC);
            return;
        default:
            e.failed = true;
            return;
    }
}

// --- Tier C: 書きガード ----------------------------------------------------
//
// 読み形 (Tier B) に対して増える不変条件は 4 つ:
//
//   G13 ページ凍結  生成コードは plan.page に属するバイトを 1 つも書かない。
//                   書き先が自ページなら、その命令の直前の命令境界で脱出する
//   G14 書きの同値  書くバイト値・位置と、**touch の回数と引数**が
//                   write8/16/32 の fast path と同一。`.l` は同一ページでも
//                   touch(a) / touch(a+3) の 2 回。畳まない
//   G16 touch の位置 最後のガードの後・ゲスト RAM store の前
//   G17 絶対形の二重防御 翻訳時に判定して積まないのに加え、**実行時ガードも吐く**

// ページ番号のビット幅。ゲストアドレスは 24bit、ページは 1KB なので 14bit。
// **extui の maskimm は 1..16 なので 14 は入る。**
constexpr std::uint32_t kPageShift = 10;
constexpr std::uint32_t kPageBits = 24u - kPageShift;

// 世代の飽和値。CodeGenMap::kAlwaysStale と同じ値。
//
// Why not code_gen_map.h を include しないか: block_emitter.h の
// kStateDOffset と同じ流儀。生成コードが埋め込むのは**翻訳時に確定した
// 数値**であって core/ の型ではない。ここで数値として書き直しておくと、
// テストが「core/ の定義と一致すること」を static_assert で別に問える。
constexpr std::uint32_t kAlwaysStaleGen = 0xFFFFu;

// 書き形かどうか。
constexpr bool isMemoryWrite(const PlannedOp& op)
{
    return isMemoryWriteKind(op.kind);
}

// G15 の条件を**翻訳時に**評価する (絶対形の事前判定に使う)。
//
// **読みガードと同式・同一実装を共用する。** write8/16/32 の fastRamHas*
// (m68k.h:346-358) は read 側とまったく同じ `a + size - 1 < limit` で、
// 整列判定の位置 (範囲より前) も同じ。違うのは `fastRamReadable_` を
// 見ないことだけで、それは guestReadFits の外にある。
bool guestWriteFits(const EmitEnv& env, std::uint32_t addr, std::uint32_t size)
{
    return guestReadFits(env, addr, size);
}

// 書き先が自ブロックのページに掛かるか (G13 を翻訳時に評価する)。
//
// **`.l` は両端を見る。** write32 は page(a) と page(a+3) の 2 ページに
// touch する (m68k.cpp:377-378)。片方だけだと、ページ境界を跨いだ長語書きで
// 自ページの端 (先頭 3 バイトか末尾 1 バイト) を黙って書く。
bool writeHitsPage(std::uint32_t addr, std::uint32_t size, std::uint32_t page)
{
    const std::uint32_t a = addr & kGuestAddrMask;
    const std::uint32_t last = (a + size - 1u) & kGuestAddrMask;
    return (a >> kPageShift) == page || (last >> kPageShift) == page;
}

// 自ページ判定を 1 本吐く (G13)。**kTmpA (マスク済みアドレス) を読むだけ。**
//
// byteOffset は 0 (先頭) か size-1 (末尾)。`.l` は 0 と 3 の 2 本吐く。
//
// Why not 「page(a) == page かつ page(a+3) == page」を 1 本に畳まないか:
// 畳めるのは「両方一致」であって、要るのは「どちらか一致」。or をとって
// 1 本にすることは**できる** (2 つの差の積が 0 かを見る等) が、
// ページ境界を跨ぐ長語では 2 つの差が別の値になるので、掛け算か
// もう 1 つの比較が要る。分岐 1 本の差でしかないので、
// **1 判定 = 1 分岐**の形をそろえて読めるようにする。
void emitSelfPageGuard(Emitter& e, std::uint32_t opIndex, std::uint32_t byteOffset,
                       std::uint32_t page)
{
    // page(a + byteOffset)。byteOffset は 0 か 3 なので、
    // マスク済みアドレスに足してから 24bit を保つ。
    if (byteOffset == 0)
    {
        extui(e.slot(kWideLen), kTmpE, kTmpA, kPageShift, kPageBits);
    }
    else
    {
        if (!canAddi(static_cast<std::int32_t>(byteOffset)))
        {
            e.failed = true;
            return;
        }
        addi(e.slot(kWideLen), kTmpE, kTmpA, static_cast<std::int32_t>(byteOffset));
        // **足してから 24bit へ丸め直す。** a は 24bit に収まっているが、
        // 0x00FFFFFF + 3 は 25bit になる。インタプリタの touch(a + 3) は
        // 丸めずに `(a+3) >> 10` を使う (m68k.cpp:378 が a+3 をそのまま
        // 渡し、CodeGenMap::touch は範囲外を数えない) ので、ここでも
        // マスクしないほうが同値。**マスクしない。**
        //
        // ただし範囲ガードが a <= limit - 4 を通しているので、
        // a + 3 < limit <= 0x00FFFFFF + 1 で 24bit を超えない。
        extui(e.slot(kWideLen), kTmpE, kTmpE, kPageShift, kPageBits);
    }
    emitConst(e, kTmpConst, page);
    sub(e.slot(kWideLen), kTmpE, kTmpE, kTmpConst);

    // 差が 0 (= 自ページ) なら島へ。**beqz** で、範囲ガードの bnez とは逆。
    const size_t insnPc = e.codeBase + e.cursor;
    std::uint8_t* slot = e.slot(kWideLen);
    e.addGuard(slot, insnPc, opIndex, /*branchOnZero=*/true, kTmpE, kSelfPageExitFlag);
}

// touch を 1 回吐く (G14/G16)。飽和つき、分岐なし。
//
// **kTmpA (マスク済みアドレス) から毎回ページを作り直す。** ガードで作った
// ページ番号を使い回せば数命令減るが、`.l` は a と a+3 の 2 つを別々に
// 数えるので、どのみち片方は作り直す。1 つの形にそろえておく。
//
// 生成コードから `page < pageCount_` の判定を消してよい根拠は翻訳時に作る:
// canEmitWritesIn が `(genPageCount << 10) >= ramLimit` を要求しているので、
// 範囲ガード `a <= limit - size` の成立から `page(a), page(a+3) < pageCount`
// が導ける (G19)。条件を満たさない env では書き形を焼かない。
//
// **kTmpConst / kTmpD / kTmpE を潰す。** ゲスト RAM の store より前に呼ぶこと。
void emitTouch(Emitter& e, std::uint32_t byteOffset, const PlannedOp& op)
{
    // page = (a + byteOffset) >> 10
    if (byteOffset == 0)
    {
        extui(e.slot(kWideLen), kTmpD, kTmpA, kPageShift, kPageBits);
    }
    else
    {
        if (!canAddi(static_cast<std::int32_t>(byteOffset)))
        {
            e.failed = true;
            return;
        }
        addi(e.slot(kWideLen), kTmpD, kTmpA, static_cast<std::int32_t>(byteOffset));
        extui(e.slot(kWideLen), kTmpD, kTmpD, kPageShift, kPageBits);
    }
    // &gen_[page] = genBase + page * 2 (u16 の配列)
    slli(e.slot(kWideLen), kTmpD, kTmpD, 1u);
    emitConst(e, kTmpConst, e.env.genBaseAddr);
    addN(e.slot(kNarrowLen), kTmpD, kTmpConst, kTmpD);

    // cur = gen_[page]
    l16ui(e.slot(kWideLen), kTmpE, kTmpD, 0u);
    // 飽和判定: cur - kAlwaysStale が 0 なら据え置き。
    emitConst(e, kTmpConst, kAlwaysStaleGen);
    sub(e.slot(kWideLen), kTmpConst, kTmpE, kTmpConst);
    // cur + 1 を作ってから、飽和なら cur へ戻す。
    //
    // **飽和させる理由はインタプリタと同じ** (CodeGenMap::touch)。
    // 単純な ++ だと 65,536 回の書き込みで元の値へ戻り、書き換えられた
    // ページが「変わっていない」と判定される。ここで畳むと JIT ON/OFF で
    // 世代が割れる。
    addi(e.slot(kWideLen), kTmpB, kTmpE, 1);
    moveqz(e.slot(kWideLen), kTmpB, kTmpE, kTmpConst);
    // **s16i なので上位 16bit は落ちる。** cur + 1 が 0x10000 になるのは
    // cur == 0xFFFF のときだけで、そのときは moveqz が cur へ戻している。
    s16i(e.slot(kWideLen), kTmpB, kTmpD, 0u);

    // op は使わないが、シグネチャを touch 対象の命令と結びつけておく
    // (将来サイズごとに形を変えたくなったときに、呼び出し側を直さずに済む)。
    (void)op;
}

// ゲスト RAM へビッグエンディアンで書く (G14)。
//
// **write8/16/32 の代入文をそのまま写す** (m68k.cpp:342 / 359-360 / 379-382)。
// バイトずつ書けばホストのバイト順にも整列にも依存しない。
//
// 値は valueReg の下位 size バイト。**valueReg は切り出し済みでなくてよい**
// (s8i が下位 8bit しか書かないので、上位は落ちる)。
// hostAddrReg にホストアドレス (窓の先頭 + マスク済みアドレス) が要る。
void emitBigEndianStore(Emitter& e, XReg hostAddrReg, XReg valueReg, std::uint32_t size)
{
    if (size == 1)
    {
        s8i(e.slot(kWideLen), valueReg, hostAddrReg, 0u);
        return;
    }
    // 上位バイトから順に置く。write16 / write32 の代入の並びと同じ。
    for (std::uint32_t i = 0; i < size; ++i)
    {
        const std::uint32_t shift = (size - 1u - i) * 8u;
        if (shift == 0)
        {
            // 最下位バイトはシフト不要。s8i が下位 8bit だけを書く。
            s8i(e.slot(kWideLen), valueReg, hostAddrReg, i);
            continue;
        }
        extui(e.slot(kWideLen), kTmpD, valueReg, shift, 8u);
        s8i(e.slot(kWideLen), kTmpD, hostAddrReg, i);
    }
}

// 書きガード列 (G15)。**読みガードと同一実装を共用する。**
//
// 式が同じなら実装も同じにする。別々に書くと、片方だけ直したときに
// 「読みでは弾くが書きでは通す」形ができ、失敗はホストメモリを踏むという
// 最も遠い形で出る。
void emitWriteGuard(Emitter& e, const PlannedOp& op, std::uint32_t opIndex)
{
    emitReadGuard(e, op, opIndex);
}

// 書き形 1 命令。
//
// 順序が契約そのもの (G3/G16):
//
//   [EA 計算]      レジスタを読むだけ。状態は 1 bit も変えない
//   [ガード 1]     整列 + 範囲 (bnez → 通常の島)
//   [ガード 2,3]   自ページ (beqz → 自ページの島)。`.l` は 2 本
//   --- ここから下が commit ---
//   [An 更新]      (An)+ / -(An)
//   [touch]        `.l` は 2 回。**畳まない**
//   [RAM store]    ビッグエンディアン、バイト順まで同じ
//   [CCR]          MOVE は setLogicFlags 相当 / CLR は Z=1,N=V=C=0
//
// **CLR の読み脚は吐かない (G20)。** インタプリタは readEaForModify で
// 一度読むが、戻り値を捨てる (m68k_ops_group4.cpp:606-607)。ガードが
// 成立する範囲では read8/16/32 の fast path に副作用が無い (touch しない /
// faulted も立たない) ので、状態に対する同値は読まなくても保たれる。
// 窓が読めない写像との両立は翻訳器が受け持つ (kClrMem は readsAllowed も要る)。
void emitMemoryWrite(Emitter& e, const PlannedOp& op, std::uint32_t opIndex, std::uint32_t page)
{
    const std::uint32_t size = op.size;
    const bool absolute = isAbsoluteEa(op.eaMode);

    if (absolute)
    {
        // G17 (a): 翻訳時の性能フィルタ。不成立の命令はここへ来ない
        // (翻訳器が積まない / canEmitWrites が断る)。
        const std::uint32_t addr = op.imm & kGuestAddrMask;
        if (!guestWriteFits(e.env, addr, size) || writeHitsPage(addr, size, page))
        {
            e.failed = true;
            return;
        }
        // G17 (b): **積んだ絶対形にも mode 2-5 と同一のガード列を吐く。**
        //
        // 読み側 (G6) は畳んで消しているが、書きで畳み間違えると窓の外の
        // ホストメモリを**書く** = ヒープ破壊で、被害の半径が違う。
        // アドレスを kTmpA へ定数で置くだけにすれば、以降のガード・touch・
        // store は動的 EA とまったく同じ発行経路を通る。
        // **畳み専用の経路が存在しなくなる**ので、テストが到達できない分岐が
        // 生まれず、翻訳時判定の変異は「ガード脱出が増える」という
        // 観測可能な形でしか現れない。
        emitConst(e, kTmpA, addr);
    }
    else
    {
        emitEffectiveAddress(e, op);
        if (e.failed)
        {
            return;
        }
    }

    emitWriteGuard(e, op, opIndex);
    emitSelfPageGuard(e, opIndex, 0u, page);
    if (size == 4)
    {
        // `.l` は 2 ページに掛かりうる (write32 が touch(a) と touch(a+3) を
        // 別々に呼ぶのと同じ理由)。
        emitSelfPageGuard(e, opIndex, size - 1u, page);
    }
    if (e.failed)
    {
        return;
    }

    // --- ここから下はガードが全部通ったときだけ走る (G3) ---

    // (An)+ / -(An) の An 更新。**絶対形では kTmpB が未定義**なので呼ばない。
    if (!absolute)
    {
        emitCommitAddressRegister(e, op);
        if (e.failed)
        {
            return;
        }
    }

    // 書く値を kTmpC へ。
    //
    // MOVE は d[srcReg] をそのまま (s8i が下位バイトだけ書くので切り出し不要)。
    // ただし **CCR は切り出した値から作る**ので、そこは別に切り出す。
    // CLR は 0。
    const bool isClear = op.kind == PlanKind::kClrMem;
    if (isClear)
    {
        movi(e.slot(kWideLen), kTmpC, 0);
    }
    else
    {
        l32i(e.slot(kWideLen), kTmpC, kState, dOffset(op.srcReg));
    }

    // touch (G14/G16)。**ゲスト RAM の store より前。**
    //
    // **`.l` は同一ページでも 2 回。畳まない。** 飽和つきなので回数が
    // 意味を持つ: gen が 0xFFFE のとき、2 回なら 0xFFFF (飽和) で止まるが
    // 1 回なら 0xFFFF。次の書きで JIT ON は据え置き、JIT OFF は…と
    // 見えるが、実際に割れるのは 0xFFFD からの `.l` で、
    // インタプリタは 0xFFFF (飽和) / 畳んだ変異体は 0xFFFE になる。
    emitTouch(e, 0u, op);
    if (e.failed)
    {
        return;
    }
    if (size == 4)
    {
        emitTouch(e, size - 1u, op);
        if (e.failed)
        {
            return;
        }
    }

    // ホストアドレス = 窓の先頭 + マスク済みアドレス。
    //
    // **絶対形でも定数に畳まない。** 畳む経路を作らないのが G17 (b) の要点。
    emitConst(e, kTmpConst, e.env.ramBaseAddr);
    addN(e.slot(kNarrowLen), kTmpE, kTmpConst, kTmpA);
    emitBigEndianStore(e, kTmpE, kTmpC, size);

    // CCR。
    if (isClear)
    {
        // CLR は結果が必ず 0 なので**翻訳時に決まる**
        // (m68k_ops_group4.cpp:608-610: N/V/C をクリアして Z を立てる)。
        emitConst(e, kTmpCcr, kCcrZ);
        emitStoreCcr(e, kTmpCcr, kLogicClear);
        return;
    }
    // MOVE は書いた値から N/Z (setLogicFlags、m68k_ops_move.cpp:67)。
    // **size で切り出してから渡す** (切り出していないと Z が立たない)。
    emitTruncate(e, kTmpC, kTmpC, size);
    emitLogicFlags(e, kTmpC, size);
}

// ガード脱出の出口で irc に入る語 = mem16(opPc + 2) を求める (I11)。
//
// **導出はここ 1 箇所だけに置く。** 出どころが 3 つに分かれるので、
// 散らばると片方だけ直したときに気づけない。テストが全ケースで
// 「実メモリの mem16(opPc + 2)」と突き合わせる。
//
//   長さ 2 (mode 2/3/4)  : opPc + 2 は**次の命令語**。
//                          最後の命令なら fallThroughIr、そうでなければ
//                          ops[k + 1].op (I4 が同一ページを保証している)
//   長さ 4 (mode 5, 7.0) : opPc + 2 は第 1 拡張ワード。
//                          imm は sext16 した値なので下位 16bit が原文
//   長さ 6 (mode 7.1)    : opPc + 2 は (xxx).L の**上位語**。
//                          imm は 2 語の連結なので上位 16bit が原文
//
// **長さ 6 は Tier C で来るようになった。** 読み側 (G6) は絶対形のガードを
// 翻訳時に畳んで消しているので島を作らないが、書き側は G17 (b) で
// **積んだ絶対形にも実行時ガードを吐く**。畳み間違いの帰結が
// 「窓の外のホストメモリを書く」= ヒープ破壊なので、読みと違って網を残す。
// その結果、(xxx).L の書きが脱出しうる = 長さ 6 の島が要る。
//
// 戻り値 false は「導出できない」。呼び出し側はその命令を積まない。
bool guardExitIrc(const BlockPlan& plan, std::uint32_t k, std::uint16_t fallThroughIr,
                  std::uint16_t& out)
{
    const PlannedOp& op = plan.ops[k];
    if (op.length == 2)
    {
        const bool isLast = k + 1 >= plan.count;
        out = isLast ? fallThroughIr : plan.ops[k + 1].op;
        return true;
    }
    if (op.length == 4)
    {
        // sext16 は下位 16bit を変えないので、原文がそのまま取り出せる。
        out = static_cast<std::uint16_t>(op.imm & 0xFFFFu);
        return true;
    }
    if (op.length == 6)
    {
        // (xxx).L は longValue = (ext0 << 16) | ext1 なので、
        // 第 1 拡張ワード (= mem16(opPc + 2)) は上位 16bit。
        //
        // **6 バイトになるのは (xxx).L だけ。** 段 1-3 の許可リストで
        // 他に 6 バイトの形が無いことは instructionLength が保証する。
        out = static_cast<std::uint16_t>((op.imm >> 16) & 0xFFFFu);
        return true;
    }
    return false;
}

// 命令境界の出口。pc / ir / irc を定数で書き、戻り値を返す。
//
// **nextInsnPc は「次に実行する命令語のアドレス」**で、出口の pc は
// そこ + 4 になる (プリフェッチの契約: pc == 命令語 + 4、
// ir == mem16(命令語)、irc == mem16(命令語 + 2))。
//
// 非分岐終端・分岐不成立・ガード脱出の 3 つが同じ形をしているので、
// 1 つにまとめて retConst だけを変える。
void emitBoundaryExit(Emitter& e, std::uint32_t nextInsnPc, std::uint16_t ir, std::uint16_t irc,
                      std::uint32_t retConst)
{
    emitConst(e, kTmpA, nextInsnPc + 4u);
    s32i(e.slot(kWideLen), kTmpA, kState, kStatePcOffset);
    emitConst(e, kTmpA, ir);
    s16i(e.slot(kWideLen), kTmpA, kState, kStateIrOffset);
    emitConst(e, kTmpA, irc);
    s16i(e.slot(kWideLen), kTmpA, kState, kStateIrcOffset);
    emitConst(e, kRet, retConst);
    retN(e.slot(kNarrowLen));
}

// 非分岐終端 / 分岐不成立の出口。
void emitFallThroughExit(Emitter& e, const BlockPlan& plan, std::uint16_t ir, std::uint16_t irc,
                         std::uint32_t cycles)
{
    emitBoundaryExit(e, plan.fallThroughPc, ir, irc, cycles);
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

// ガード脱出の出口を、終端出口の**後ろ**にまとめて並べる (§4 の「出口の島」)。
//
// **島は終端出口の後ろにしか置けない。** 途中に置くと、ガードを通った
// 直線の実行がそのまま島へ落ちてしまう。後ろに置けば、届くのは
// bnez で飛んできたときだけになる。
//
// 出口の状態は「その命令の直前の命令境界」(G7):
//   pc     = opPc + 4      (次に実行する命令語 = opPc なので +4)
//   ir     = ops[k].op     (その命令語そのもの)
//   irc    = mem16(opPc+2) (guardExitIrc が導く)
//   cycles = Σ ops[0..k-1] (その命令はまだ実行していない)
//
// ガード無しのブロックでは 1 バイトも吐かないので、既存の決定性テストと
// バイト列が変わらない。
void emitGuardExitIslands(Emitter& e, const BlockPlan& plan, std::uint16_t fallThroughIr)
{
    // 既に吐いた島の位置。**(opIndex, extraRetFlags) の組で共有する。**
    //
    // `.l` の書きは自ページ判定を 2 本持つが、どちらも同じ出口の状態
    // (同じ命令の手前の境界、同じ戻り値) へ降りる。島を 2 つ吐いても
    // 正しいが、コードが無駄に伸びて codeFull_ の崖が近くなる。
    // 通常の脱出と自ページ脱出は戻り値が違うので、**同じ島にはできない**。
    struct Island
    {
        std::uint32_t opIndex = 0;
        std::uint32_t extraRetFlags = 0;
        size_t stubPc = 0;
        bool valid = false;
    };
    Island islands[kMaxPendingGuards] = {};
    size_t islandCount = 0;

    for (size_t g = 0; g < e.guardCount; ++g)
    {
        const PendingGuard& pending = e.guards[g];
        const std::uint32_t k = pending.opIndex;
        const PlannedOp& op = plan.ops[k];

        // 同じ (opIndex, extraRetFlags) の島が既にあれば、そこへ飛ばす。
        size_t stubPc = 0;
        bool found = false;
        for (size_t i = 0; i < islandCount; ++i)
        {
            const bool sameExit = islands[i].valid && islands[i].opIndex == k &&
                                  islands[i].extraRetFlags == pending.extraRetFlags;
            if (sameExit)
            {
                stubPc = islands[i].stubPc;
                found = true;
                break;
            }
        }

        if (!found)
        {
            std::uint16_t irc = 0;
            if (!guardExitIrc(plan, k, fallThroughIr, irc))
            {
                e.failed = true;
                return;
            }

            // その命令の手前までのサイクル。**その命令ぶんは足さない。**
            std::uint32_t cycles = 0;
            for (std::uint32_t i = 0; i < k; ++i)
            {
                cycles += plan.ops[i].cycles;
            }

            // 島の先頭 = この分岐の飛び先。
            stubPc = e.codeBase + e.cursor;
            const std::uint32_t ret = kGuardExitFlag | pending.extraRetFlags |
                                      (k << kGuardCountShift) | (cycles & kCycleMask);
            emitBoundaryExit(e, op.pc, op.op, irc, ret);

            if (islandCount >= kMaxPendingGuards)
            {
                e.failed = true;
                return;
            }
            islands[islandCount] = Island{k, pending.extraRetFlags, stubPc, true};
            ++islandCount;
        }

        // 前方参照だった分岐をここでパッチする。
        //
        // **変位は codeBase 相対の差**なので、codeBase = 0 の集めるパスでも
        // 本番でも同じ値になる (l32r と違って検査を遅らせなくてよい)。
        const std::int32_t disp = static_cast<std::int32_t>(stubPc) -
                                  (static_cast<std::int32_t>(pending.insnPc) + kBranchOrigin);
        if (!canBranch12(disp))
        {
            e.failed = true;
            return;
        }
        // 集めるパスでは slot が scratch を指すので、書いても無害。
        //
        // **beqz と bnez を取り違えると条件が反転する。** 自ページ判定は
        // 「差が 0 なら脱出」(beqz)、範囲・整列は「失敗ビットが立ったら脱出」
        // (bnez)。控えた側で選び分ける。
        if (pending.branchOnZero)
        {
            beqz(pending.slot, pending.reg, disp);
        }
        else
        {
            bnez(pending.slot, pending.reg, disp);
        }
    }
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

            // --- Tier A: メモリに触れず例外も起きない形 ---
            case PlanKind::kMoveAregToDreg:
                emitMoveAregToDreg(e, op);
                break;
            case PlanKind::kMoveImmToDreg:
                emitMoveImmToDreg(e, op);
                break;
            case PlanKind::kMoveaDregToAreg:
                emitMovea(e, op, /*srcIsAddressRegister=*/false);
                break;
            case PlanKind::kMoveaAregToAreg:
                emitMovea(e, op, /*srcIsAddressRegister=*/true);
                break;
            case PlanKind::kMoveaImmToAreg:
            case PlanKind::kLeaAbs:
                // どちらも「翻訳時に決まった定数を a[] へ入れる」だけ。
                emitLoadAregConst(e, op);
                break;
            case PlanKind::kLeaDisp:
                emitLeaDisp(e, op);
                break;
            case PlanKind::kTstDreg:
                emitTstDreg(e, op);
                break;
            case PlanKind::kClrDreg:
                emitClrDreg(e, op);
                break;

            // --- Tier B: メモリを読む形 ---
            case PlanKind::kMoveMemToDreg:
            case PlanKind::kTstMem:
            case PlanKind::kAluMemToDreg:
                emitMemoryRead(e, op, i);
                break;

            // --- Tier C: メモリへ書く形 ---
            case PlanKind::kMoveDregToMem:
            case PlanKind::kClrMem:
                // 自ページ判定に plan.page が要る (G13)。
                emitMemoryWrite(e, op, i, plan.page);
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

    emitGuardExitIslands(e, plan, ir);
}

// 書き形を含む計画を、この窓で発行してよいか (G17 (a) / G19)。
//
// **翻訳時に決められることは翻訳時に決める。** 実行時ガードは別に吐く
// (G17 (b)) が、翻訳時に「必ず脱出する」と分かる形を積むと、
// 毎周ガードを踏むだけのブロックができる。
bool canEmitWritesFor(const BlockPlan& plan, const EmitEnv& env)
{
    for (std::uint32_t i = 0; i < plan.count; ++i)
    {
        const PlannedOp& op = plan.ops[i];
        if (!isMemoryWrite(op))
        {
            continue;
        }

        // G19: 窓と世代配列がそろっていないと、書き形は焼けない。
        if (!canEmitWritesIn(env))
        {
            return false;
        }

        // 窓がこのサイズの書きを一度も許さないなら、ガードは常に不成立。
        if (env.ramLimit < op.size)
        {
            return false;
        }

        // CLR <mem> は読み脚を省いている (G20)。省略の前提は
        // 「ガードが成立する範囲では read8/16/32 の fast path に副作用が
        // 無い」ことで、それは fastRamReadable_ が立っているときだけ成り立つ。
        //
        // 翻訳器 (canEmitWrites / readsAllowed) が先に弾いているが、
        // **エミッタ単体でも正しく断れるようにする** (テストが env を
        // 直接渡して requiredSize を問う形を取る)。
        const bool clearNeedsReadableWindow = op.kind == PlanKind::kClrMem && !env.ramReadable;
        if (clearNeedsReadableWindow)
        {
            return false;
        }

        // G17 (a): 絶対アドレスは実効アドレスが翻訳時定数なので、
        // **範囲・整列・自ページを翻訳時に判定して不成立なら積まない。**
        if (isAbsoluteEa(op.eaMode))
        {
            const std::uint32_t addr = op.imm & kGuestAddrMask;
            if (!guestWriteFits(env, addr, op.size))
            {
                return false;
            }
            if (writeHitsPage(addr, op.size, plan.page))
            {
                return false;
            }
        }
    }
    return true;
}

// 読み形を含む計画を、この窓で発行してよいか (G6 / G12)。
//
// **翻訳時に決められることは翻訳時に決める。** 走らせてから諦める形にすると、
// 「絶対に成立しないガード」を毎回踏むブロックができる。
bool canEmitReads(const BlockPlan& plan, const EmitEnv& env)
{
    for (std::uint32_t i = 0; i < plan.count; ++i)
    {
        const PlannedOp& op = plan.ops[i];
        // **書き形はここでは見ない。** eaMode だけで判定すると書き形も
        // 読み形として扱われ、窓が読めない写像 (ROM 写像中) で書き形まで
        // 断ってしまう。書き経路は fastRamReadable_ を見ない
        // (m68k.cpp:331-334) ので、そこで断るのは保守的すぎる。
        if (!isMemoryRead(op) || isMemoryWrite(op))
        {
            continue;
        }

        // G12: 窓が無い / 読めない写像では、読み形を焼かない。
        const bool windowUnusable = !env.ramReadable || env.ramBaseAddr == 0 || env.ramLimit == 0;
        if (windowUnusable)
        {
            return false;
        }

        // 窓がこのサイズの読みを一度も許さないなら、ガードは常に不成立。
        if (env.ramLimit < op.size)
        {
            return false;
        }

        // G6: 絶対アドレスは翻訳時にガードを評価する。**不成立なら積まない。**
        if (isAbsoluteEa(op.eaMode) && !guestReadFits(env, op.imm, op.size))
        {
            return false;
        }
    }
    return true;
}

// リテラルを集めるパスを回して、必要な語数とコード長を得る。
bool measure(const BlockPlan& plan, std::uint16_t ir, std::uint16_t irc, const EmitEnv& env,
             size_t& literalCount, size_t& codeLen)
{
    if (plan.count == 0 || plan.count > kMaxOps)
    {
        return false;
    }
    if (!canEmitReads(plan, env))
    {
        return false;
    }
    if (!canEmitWritesFor(plan, env))
    {
        return false;
    }
    Emitter e{};
    e.env = env;
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

bool canEmitWritesIn(const EmitEnv& env)
{
    const bool windowUnusable = env.ramBaseAddr == 0 || env.ramLimit == 0;
    if (windowUnusable)
    {
        return false;
    }
    // 世代配列が無いと touch を再現できない。**諦める** (G12 と同種)。
    const bool genUnusable = env.genBaseAddr == 0 || env.genPageCount == 0;
    if (genUnusable)
    {
        return false;
    }
    // `.l` の生成コードは a + 3 を**マスクせずに** page へ落とす
    // (インタプリタの touch(a + 3) が m68k.cpp:378 でそうしているのに合わせる)。
    // 範囲ガードが a <= limit - 4 を通しているので、limit がここに収まれば
    // a + 3 は 24bit を超えない。
    //
    // **超えると extui(.., 10, 14) が上位を落として別のページを触る。**
    // インタプリタ側は範囲外として数えないので、そこで世代が割れる。
    // 現実の窓 (メイン RAM 2MB) では起きないが、「起きない」に頼らない。
    if (env.ramLimit > 0x00FFFFFDu)
    {
        return false;
    }
    // G19: 範囲ガード成立 ⇒ touch 対象ページが配列の中、を導けること。
    //
    // これがあるから生成コードから `page < pageCount_` の判定を消せる。
    // 満たさない env で焼くと、窓の端の書きが**世代配列の外へ s16i する**。
    // 桁あふれさせないよう割り算の形で見る (pageCount << 10 は 32bit を
    // 超えうる)。
    const std::uint32_t coveredPages = (env.ramLimit + (1u << kPageShift) - 1u) >> kPageShift;
    return coveredPages <= env.genPageCount;
}

size_t requiredSize(const BlockPlan& plan, std::uint16_t fallThroughIr,
                    std::uint16_t fallThroughIrc, const EmitEnv& env)
{
    size_t literalCount = 0;
    size_t codeLen = 0;
    if (!measure(plan, fallThroughIr, fallThroughIrc, env, literalCount, codeLen))
    {
        return 0;
    }
    // 1 パス目の codeBase は 0 なので、l32r の変位が 2 パス目とは違う。
    // 変位が違っても命令長は 3 バイト固定なので**コード長は変わらない**。
    // リテラル領域だけが前に積まれる。
    return literalCount * 4u + codeLen;
}

bool emitBlock(const BlockPlan& plan, std::uint16_t fallThroughIr, std::uint16_t fallThroughIrc,
               const EmitEnv& env, std::uint8_t* out, size_t capacity, EmittedBlock& result)
{
    size_t literalCount = 0;
    size_t codeLen = 0;
    if (!measure(plan, fallThroughIr, fallThroughIrc, env, literalCount, codeLen))
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
    e.env = env;
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
