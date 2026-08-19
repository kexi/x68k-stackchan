// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 直線区間 1 本ぶんの実行計画。POD だけを置き、実装は持たない。
//
// core/ の役割は「この番地から何をすればよいか」を**データ**にするところまでで、
// それを解釈実行するのも Xtensa の機械語へ落とすのも外側の仕事にする。
// 計画がデータなら、翻訳器はホストで全数テストでき、ESP32 に触らずに済む。
//
// ## なぜ命令語とデコード済みの意味を両方持つのか
//
// 意味 (レジスタ番号 / サイズ / 演算種別) だけにするとエミッタは楽になるが、
// 命令語そのものを捨てると **バイト照合** ができなくなる。ページ世代は
// 1KB 粒度なので、コードと同居するデータを書いただけで世代が上がる
// (偽共有)。そのとき命令語を実メモリと突き合わせて「同じなら世代を控え直す
// だけ」で済ませられるかどうかが、再翻訳の回数を決める。
// 逆に命令語だけにすると、実行のたびに再デコードが要る。
// **どちらかを捨てると、捨てた側は後から復元できない。**

#ifndef X68K_CORE_CPU_BLOCK_PLAN_H
#define X68K_CORE_CPU_BLOCK_PLAN_H

#include "m68k_types.h"

namespace x68k
{

// ブロックへ入れてよい命令の種別。
//
// Why not 禁止リストにしないか: 禁止リストだと、新しい命令を足した人の変更が
// 黙って通ってしまい、「例外が起きない」「メモリを触らない」という前提を
// 静かに壊す。許可リストなら、足したい人が必ずここへ来る。
enum class PlanKind : u8
{
    kMoveRegToReg = 0,  // MOVE.b/w/l Dn,Dm (src/dst とも mode 0)
    kMoveq,             // MOVEQ #imm8,Dn (bit8 == 0)
    kAluRegToReg,       // ADD/SUB/AND/OR/EOR/CMP <Dn>,Dm (両側 mode 0)
    kBranch,            // Bcc / BRA。**必ずブロック末尾**。BSR は含まない

    // --- Tier A: メモリに触れず例外も起きない形 ---
    //
    // **§5.3 の前提を 1mm も動かさない。** どれも実効アドレスを持たないか
    // (LEA は持つが「アドレスを求めるだけで読まない」)、副作用がレジスタに
    // 閉じている。実行時ガードが要らないのが Tier A の定義。
    kMoveAregToDreg,   // MOVE.w/l An,Dn。srcReg = An 番号
    kMoveImmToDreg,    // MOVE.b/w/l #imm,Dn。imm = size でマスク済み
    kMoveaDregToAreg,  // MOVEA.w/l Dn,An。**フラグ不変**
    kMoveaAregToAreg,  // MOVEA.w/l An,An。**フラグ不変**
    kMoveaImmToAreg,   // MOVEA.w/l #imm,An。imm = 符号拡張済み
    kTstDreg,          // TST.b/w/l Dn。srcReg = 対象 (読むだけ)
    kClrDreg,          // CLR.b/w/l Dn。dstReg = 対象
    kLeaDisp,          // LEA (An),An / (d16,An),An。imm = 符号拡張済み変位
    kLeaAbs,           // LEA (xxx).W/L,An。imm = アドレス

    // --- Tier B: メモリを**読む**形 ---
    //
    // Tier A との違いは、実効アドレスが窓の中に入っているかを
    // **実行時に確かめないと分からない**こと。翻訳器は読める前提で
    // 焼き、生成コードがガードで確かめて、外れたら step() へ降りる。
    //
    // どれも eaMode に読み出し側の EA を持つ (kEaNone 以外)。
    // 書く方向は入れない (世代更新とアドレスエラーを背負う)。
    kMoveMemToDreg,  // MOVE.b/w/l <mem>,Dn。srcReg = An 番号 / eaMode / imm
    kTstMem,         // TST.b/w/l <mem>
    kAluMemToDreg,   // ADD/SUB/AND/OR/CMP.b/w/l <mem>,Dn。kEor は入れない

    // --- Tier C: メモリへ**書く**形 ---
    //
    // Tier B に対して背負うものが 2 つ増える。
    //
    //   ページ世代の更新 (CodeGenMap::touch) — 回数まで含めて同値にする。
    //     .l は同一ページでも 2 回 (m68k.cpp:377-378)。畳むと飽和の
    //     タイミングが JIT ON/OFF で割れる (G14)
    //   自ページ書き換え — 書き先が自ブロックのページなら、焼いた定数
    //     (ops / 出口の ir/irc) が実行中に古くなる。**書く前に脱出**する
    //     (G13)
    //
    // **EA レジスタは dstReg に置く。** 読み形は srcReg に置いているので、
    // 「データフローの向きの欄」という規約で分ける。参照は必ず
    // eaRegOf() を通すこと (直書きすると読み形と書き形で静かに割れる)。
    kMoveDregToMem,  // MOVE.b/w/l Dn,<mem>。srcReg = Dn / dstReg = An 番号
    kClrMem,         // CLR.b/w/l <mem>。dstReg = An 番号
};

// PlannedOp::eaMode の値。
//
// 68000 の mode 番号をそのまま使い、mode 7 だけは reg を足して区別する
// (7.0 と 7.1 は拡張ワード数も意味も違うので、1 つの値にまとめられない)。
//
// **mode 6 と 7.2/7.3 は入れない。** mode 6 は拡張ワードの解釈 (インデックス
// レジスタとスケール) が要り、7.2/7.3 は PC 相対で「翻訳時の PC」に依存する。
// どちらも Tier B の読みガードとは別の検証面になる。
inline constexpr u8 kEaNone = 0;         // 読み形ではない (Tier A の kind)
inline constexpr u8 kEaIndirect = 2;     // (An)
inline constexpr u8 kEaPostInc = 3;      // (An)+
inline constexpr u8 kEaPreDec = 4;       // -(An)
inline constexpr u8 kEaDisp16 = 5;       // (d16,An)。imm = sext16(d16)
inline constexpr u8 kEaAbsShort = 0x70;  // (xxx).W。imm = 符号拡張済みアドレス
inline constexpr u8 kEaAbsLong = 0x71;   // (xxx).L。imm = アドレス

enum class PlanAluOp : u8
{
    kAdd = 0,
    kSub,
    kAnd,
    kOr,
    kEor,
    kCmp,
};

// ブロックが切れた理由。
//
// 段 0 の実測で得た区間の分布 (分岐 43.3% / 呼出・復帰 30.3% / 書込 26.5%) を
// 段 1 以降も同じ軸で追えるように enum で持つ。対応範囲を広げるかどうかの
// 判断は、速度ではなくこの内訳を見て決める。
enum class BlockEnd : u8
{
    kBranch = 0,     // 分岐で終端。branchTarget / fallThroughPc が有効
    kUnsupported,    // 未対応命令の**手前**で終端
    kUnknownLength,  // instructionLength が長さを返さない命令の手前で終端
    kCapacity,       // kMaxOps に達した
    kWindowExit,     // 命令語かプリフェッチ先が窓の外
    kPageBoundary,   // 1KB ページを跨ぐ手前で終端
};

// ブロック内の 1 命令。命令語とデコード済みの意味を両方持つ (冒頭の理由)。
struct PlannedOp
{
    u32 pc;     // この命令語のゲストアドレス
    u16 op;     // 命令語そのもの。バイト照合と統計に使う
    u8 length;  // 命令全体のバイト数 (2 または 4)
    PlanKind kind;
    u8 srcReg;  // 0-7
    u8 dstReg;  // 0-7
    // 1/2/4 バイト。m68k_alu.h と同じ値にそろえる。
    // **MOVE の符号化にある 0/1/2 ではない。**翻訳時に一度だけ変換しておく。
    u8 size;
    PlanAluOp aluOp;  // kAluRegToReg のときだけ有効
    u32 imm;          // kMoveq の符号拡張済み即値
    u8 cycles;        // この命令のサイクル。分岐は「不成立側」の値
    u8 cond;          // kBranch の条件コード (0 = BRA、2..15 = Bcc)
    // 読み形の実効アドレス (kEaNone / kEaIndirect / ... / kEaAbsLong)。
    // **Tier A の kind では必ず kEaNone。** エミッタはこの欄で
    // 「ガード列を吐くかどうか」を決めるので、埋め忘れると
    // ガード無しでメモリを読む形になる。
    u8 eaMode;
    u8 pad;
};

// メモリへ書く形か (Tier C)。
//
// **kind で判定する。** eaMode だけを見ると読み形と区別が付かない。
constexpr bool isMemoryWriteKind(PlanKind kind)
{
    return kind == PlanKind::kMoveDregToMem || kind == PlanKind::kClrMem;
}

// 実効アドレスの An 番号がどちらの欄に入っているか。
//
// **PlannedOp を 20 バイトに保つために欄を流用している。** 読み形は
// srcReg (メモリが転送元)、書き形は dstReg (メモリが転送先)。
// 「データフローの向きの欄」という規約なので読めば分かるが、
// 参照が散らばると片方だけ直したときに気づけない。
//
// ここを直書き (op.srcReg) に戻すと、書き形は (An)+ の An を srcReg
// (= 転送元の Dn 番号) から取ることになり、**関係ないアドレスレジスタが
// 進む**。MOVE.b D0,-(A7) の「A7 だけ 2 減る」特例も同時に外れる。
constexpr u8 eaRegOf(const PlannedOp& op)
{
    return isMemoryWriteKind(op.kind) ? op.dstReg : op.srcReg;
}

// ブロックへ入れる命令数の上限。
//
// **4 にしてある。** 段 0 実測の平均区間長は 1.81 命令 (direct chaining を
// 仮定しても 2.08) なので、8 でも 16 でも実効的な差が出ない。一方でこの上限は
// **割り込み受理の遅れの上限**でもある。ブロック実行中はデバイスに時間を
// 渡さないので、新たに立った割り込みは最大でブロック 1 本ぶん遅れる。
// 得が無い側 (長さ) と失う側 (応答性) が釣り合わないので、小さい方へ倒す。
//
// sizeof(BlockPlan) は 112 バイト (PlannedOp 20 バイト × 4 + 鍵と出口 32)。
// 256 ブロックで 28,672 バイト、世代配列 4,096 バイトと合わせて 32,768 バイトで、
// 実測した内部 SRAM の空き 48,351 バイトに収まる。
// 伸ばす変更は、割り込み受理点のずれを検査するテストを必ず通すこと。
inline constexpr u32 kMaxOps = 4;

struct BlockPlan
{
    // --- 鍵 ---
    //
    // 実行前にこの順で照合し、1 つでも合わなければ計画を捨てて翻訳し直す。
    // **pageGen != kAlwaysStale を pageGen の一致より先に見る。** 逆順だと
    // CodeGenMap が未配線・または世代が飽和したときに 0xFFFF == 0xFFFF が
    // 成立し、「常に古い」が「常に有効」へ化けて自己書き換えを一切
    // 検出しないブロックキャッシュができあがる。
    u32 entryPc;       // 先頭命令語のゲストアドレス。**0 は空きスロットの番兵**
    u32 mappingEpoch;  // CodeGenMap::mappingEpoch() の控え
    u32 page;          // 先頭バイトの属するページ番号
    u16 pageGen;       // そのページの世代
    u8 count;          // ops の有効数。**照合時に kMaxOps 以下を検査する**
    BlockEnd end;

    // --- 本体 ---
    PlannedOp ops[kMaxOps];

    // --- 出口 ---

    // 分岐不成立 / 非分岐終端のときの「次に実行する命令語アドレス」。
    u32 fallThroughPc;

    // 分岐成立時の飛び先 (命令語アドレス)。end == kBranch のときだけ有効。
    // **翻訳時に偶数であることを検査済み。** 奇数ならアドレス例外に入るので、
    // その分岐は積まずに手前で終端する。
    u32 branchTarget;

    // 総サイクル。成立側と不成立側を別に持つ。
    //
    // Why not 「成立なら +2」で片方から導かないか: Bcc.b は不成立 8 / 成立 10、
    // Bcc.w は不成立 12 / 成立 10 で **符号が逆になる**。片方から導くと Bcc.w で
    // 4 サイクルずれ、rasterNumber() の 317 サイクル粒度が数万命令後に必ず割れる。
    // 2 本持てば足し算のときに選ぶだけで済み、ずれようがない。
    //
    // BRA は条件が常に真なので必ず cyclesTaken (10) が使われ、
    // cyclesNotTaken は参照されない。
    u32 cyclesNotTaken;
    u32 cyclesTaken;
};

}  // namespace x68k

#endif  // X68K_CORE_CPU_BLOCK_PLAN_H
