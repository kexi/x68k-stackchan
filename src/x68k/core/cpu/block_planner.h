// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 命令語の並びを BlockPlan へ翻訳する。core/ が持つのはここまでで、
// 計画をどう走らせるかは外側 (platform/jit/) の仕事にする。
//
// ## なぜ M68k も Bus も受け取らないのか
//
// 翻訳器が M68k を握ると、テストが Machine を丸ごと立てないと書けなくなる。
// 命令長デコーダ (m68k_length.h) が 65,536 通りの全数検証をできているのは
// **入力が u16 だけで閉じている**からで、同じ性質をここでも保ちたい。
// 命令語の読み出しとページ世代の参照を関数ポインタで抽象しておけば、
// テストは平坦な u16 配列と偽の世代を渡すだけで全数を回せる。
//
// ## 保守的に振る舞う
//
// 少しでも怪しければ、その命令を**積まずに**終端する。ブロックが短く
// なるだけで正しさは損なわれない。逆に「たぶん大丈夫」で積むと、失敗は
// 古い状態で走り続ける形で現れ、原因から最も遠いところで表面化する。

#ifndef X68K_CORE_CPU_BLOCK_PLANNER_H
#define X68K_CORE_CPU_BLOCK_PLANNER_H

#include "block_plan.h"
#include "m68k_types.h"

namespace x68k
{

// 命令語を読む口。
//
// **副作用を持ってはいけない。** ここでバスを叩くと、翻訳しただけで
// MFP の割り込み要因レジスタなどが動く。呼び出し側は必ず「窓 (fastRam)
// の中だけを読み、外なら false を返す」実装を渡すこと。
struct PlanSource
{
    // addr の命令語を読む。窓の外なら false を返し out は触らない。
    bool (*read16)(void* ctx, u32 addr, u16& out);
    void* ctx;
};

// ページ世代を引く口。テストから偽物を渡せるように抽象する。
struct PlanGenSource
{
    u16 (*generation)(void* ctx, u32 addr);
    u32 (*mappingEpoch)(void* ctx);
    void* ctx;
};

class BlockPlanner
{
public:
    // entryPc (命令語のアドレス) からブロックを 1 本組む。
    //
    // 戻り値: 1 命令でも積めたら true。0 命令なら false
    //         (呼び出し側は kDeferToStep を返して step() へ落とす)。
    //
    // false のとき out の中身は未規定。true のときだけ読むこと。
    static bool plan(const PlanSource& src, const PlanGenSource& gen, u32 entryPc, BlockPlan& out);

    // 1 命令ぶんを解析する。
    //
    // **この関数がブロックへ入れてよい命令の唯一の定義。** 段 2 で命令を
    // 足すときは、例外に入りうる形をここで手で弾いていること
    // (MOVE.b の An / BSR) を読み直してから足すこと。
    //
    // pc は命令語のアドレス。分岐先の計算に要る。
    // 分岐は cyclesNotTaken 相当 (不成立側) を out.cycles に入れる。
    // 成立側は plan() が cond と length から組み立てる。
    static bool planOne(u16 op, u32 pc, PlannedOp& out);
};

}  // namespace x68k

#endif  // X68K_CORE_CPU_BLOCK_PLANNER_H
