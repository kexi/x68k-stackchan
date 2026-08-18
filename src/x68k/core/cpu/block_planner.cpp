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

// MOVE.b/w/l Dn,Dm。src / dst とも mode 0 のときだけ受ける。
bool planMove(u16 op, PlannedOp& out)
{
    const u32 group = static_cast<u32>(op >> 12);
    const u32 srcMode = static_cast<u32>((op >> 3) & 7u);
    const u32 srcReg = static_cast<u32>(op & 7u);
    const u32 dstMode = static_cast<u32>((op >> 6) & 7u);
    const u32 dstReg = static_cast<u32>((op >> 9) & 7u);

    const u32 size = moveSizeFromGroup(group);

    // 転送先は Dn か An だけ。メモリへ書く形は codeGen_ の世代更新と
    // アドレスエラーを背負うので Tier A には入れない。
    if (dstMode > 1)
    {
        return false;
    }

    // 転送元は Dn / An / 即値だけ。mode 7 の 0-3 (絶対・PC 相対) は
    // 読み出しがメモリに触る。
    const bool srcIsImmediate = srcMode == 7 && srcReg == 4;
    if (srcMode > 1 && !srcIsImmediate)
    {
        return false;
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

// $4 から、メモリに触れず例外も起きない 3 系統だけを拾う。
//
// Why not NEGX/NEG/NOT Dn も入れないか: X と累積 Z が絡む。planAlu が
// ADDX/SUBX を除いているのと同じ理由で、まず確実な形だけにする。
bool planMisc(u16 op, PlannedOp& out)
{
    const u32 mode = static_cast<u32>((op >> 3) & 7u);
    const u32 reg = static_cast<u32>(op & 7u);

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

    // 単項 TST / CLR : 0100 oooo ss mmm rrr。
    // **判別ビットは (op >> 8) & 0xF。** 実装の unary_ops が switch している
    // 値と揃える (ここを (op >> 9) & 7 で書いて TST を丸ごと取りこぼした
    // 前例がある)。
    const u32 opcodeBits = static_cast<u32>((op >> 8) & 0xFu);
    const u32 sizeField = static_cast<u32>((op >> 6) & 3u);
    if (sizeField == 3 || mode != 0)
    {
        return false;
    }
    const u32 size = sizeField == 0 ? kByte : (sizeField == 1 ? kWord : kLong);

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

    // mode 0 (Dn) だけを受ける。メモリを触らない = バスエラーも
    // アドレスエラーも起きえない、という前提がここで閉じる。
    if (mode != 0)
    {
        return false;
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

// Bcc / BRA。BSR は入れない (I8)。
bool planBranch(u16 op, u32 pc, PlannedOp& out)
{
    const u32 cond = static_cast<u32>((op >> 8) & 0xFu);
    const u32 disp8 = static_cast<u32>(op & 0xFFu);

    // I8: BSR は a[7] を減らして write32 する。メモリを触るので
    // codeGen_.touch とアドレスエラーを同時に背負うことになる。
    if (cond == 1)
    {
        return false;
    }

    // 32bit 変位 ($FF) は 68020 以降。instructionLength も
    // kUnknownLength を返すので I1 が先に弾くが、単体でも落とす。
    if (disp8 == 0xFFu)
    {
        return false;
    }

    // 分岐先の基準は「命令語の次のワードのアドレス」。
    // groupBranch は st_.pc - 4 で同じ値を作る (命令語を fetch した直後の
    // pc は 命令語 + 6 なので、pc - 4 が 命令語 + 2)。
    const u32 base = pc + 2;

    if (disp8 == 0)
    {
        // 16bit 変位。拡張ワードの中身は plan() が読んで埋める。
        out.kind = PlanKind::kBranch;
        out.cond = static_cast<u8>(cond);
        // groupBranch の不成立側は disp8 == 0 で 12。
        out.cycles = 12;
        out.imm = base;
        return true;
    }

    out.kind = PlanKind::kBranch;
    out.cond = static_cast<u8>(cond);
    // 8bit 変位の不成立側は 8。
    out.cycles = 8;
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

bool BlockPlanner::plan(const PlanSource& src, const PlanGenSource& gen, u32 entryPc,
                        BlockPlan& out)
{
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
        bool extensionsReadable = true;
        u16 lastExtension = 0;
        for (u32 offset = 2; offset < length; offset += 2)
        {
            if (!src.read16(src.ctx, pc + offset, lastExtension))
            {
                extensionsReadable = false;
                break;
            }
        }
        if (!extensionsReadable)
        {
            out.end = BlockEnd::kWindowExit;
            break;
        }

        const bool isBranch = planned.kind == PlanKind::kBranch;
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
            // 成立側は形によらず 10 (groupBranch)。BRA も testCondition(0) が
            // 常に真なのでここを通り、cyclesNotTaken は参照されない。
            out.cyclesTaken += 10;
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
