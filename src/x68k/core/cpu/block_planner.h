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

// 読み形をブロックへ入れてよいか。
//
// **翻訳器がエミッタの都合を知る唯一の口。** 読み形はガードで窓を検査する
// ので、窓が使えない状態 (ROM 写像中で RAM が読めない、窓が未設定) では
// 発行できない。
//
// Why 翻訳器に教えるのか: 教えないと、翻訳器が読み形を積んだブロックを
// エミッタが**丸ごと**拒否する。読み形の手前で終端していれば短くても
// 翻訳できたのに、1 つ入っただけで全部失うので、**入れる前より悪くなる**。
// 実際に踏んだ (翻訳失敗 1,944 → 2,198,539、クロック 6493 → 6316)。
//
// 既定 (nullptr) は「入れてよい」。テストが窓を持たない場合に段 1 以前と
// 同じ挙動になる。
struct PlanCapabilities
{
    bool (*canEmitReads)(void* ctx);
    // 書き形をブロックへ入れてよいか (Tier C)。
    //
    // 読みと条件が違う。**書き経路は fastRamReadable_ を見ない**ので
    // (m68k.cpp:331-334 — ROM 写像中も RAM へは書ける)、窓が読めない
    // 写像でも書き形は焼ける。代わりに世代配列が要る (touch を再現するため)。
    //
    // 既定 (nullptr) は canEmitReads と同じく「入れてよい」。
    bool (*canEmitWrites)(void* ctx);
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
    static bool plan(const PlanSource& src, const PlanGenSource& gen, u32 entryPc, BlockPlan& out,
                     const PlanCapabilities& caps = PlanCapabilities{});

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
