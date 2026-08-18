// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// BlockPlan を Xtensa LX7 の直線コードへ落とす。段 1 が作ったデータを、段 2 が機械語にする。
//
// ## 出口の契約 (段 1 設計 §5) をどう守るか
//
// 生成コードが抜けるとき、M68kState は「そのブロックの最後の命令をインタプリタで
// 実行し終えた直後」とビット単位で同一でなければならない。守り方は 3 つに分かれる。
//
//   d[0..7] / a[0..7]  — 各命令が s32i でその場で書く。**レジスタに寝かせない**
//   sr                 — l16ui で読み、CCR の 5 ビットだけを作り直して s16i で書く
//   pc / ir / irc      — 出口で 3 語まとめて書く。値は**翻訳時に確定している**
//
// Why not d/a をレジスタに寝かせて出口でまとめて書き戻さないか: 寝かせると
// 「どの Dn がどの ax に居るか」の割り当てが要り、出口の書き戻し漏れが
// **静かに** 起きる。漏れたレジスタは次のブロックが読むまで誰にも見えず、
// 失敗は原因から遠いところで表面化する。メモリを直接更新すれば、漏れは
// 原理的に起こりえない。kMaxOps = 4 に対し平均区間長 1.81 命令なので、
// 寝かせて省ける再ロードの回数もそもそも少ない。速度が要るなら段 3 で、
// **出口の書き戻しを機械で検査するテストを先に置いてから**やる。
//
// ## pc / ir / irc を翻訳時に確定できる根拠
//
// 出口の状態は pc == X + 4 / ir == mem16(X) / irc == mem16(X + 2) (X は次に実行する
// 命令語のアドレス)。非分岐終端と分岐不成立では X == fallThroughPc で、
// **段 1 の不変条件 I4 が「entryPc から fallThroughPc + 2 までが同一 1KB ページ」を
// 保証している**ので、この 2 ワードは翻訳時に PlanSource から読める。だから
// 生成コードはメモリを読まずに定数を置くだけでよい。
//
// 分岐成立側は X == branchTarget で、そこはページの外にありうる。読めるとは
// 限らないので、**生成コードでは ir / irc を書かない**。代わりに戻り値の
// bit31 を立てて「分岐が成立した」ことだけを伝え、呼び出し側が
// M68k::branchTo(branchTarget) を呼ぶ。
//
// Why not 生成コードから refillPrefetch を呼ばないか (§5.2): あれは窓を外すと
// bus_.read16 へ落ち、I/O 空間なら読み出しの副作用が起き、窓外なら
// **インタプリタでは一度も起きなかったバスエラー**が発生する。呼んでよいのは
// 分岐が成立したときだけで、その判定は生成コードの外に置くのが安全。
//
// ## 呼び出し規約
//
// call0 ABI (windowed の entry/retw 9.3 サイクルに対し 6.7 サイクル、実測)。
// 引数 a2 = M68kState*、戻り値 a2 = サイクル数 | 分岐成立フラグ。
// 生成コードは a0 / a1 / PS / WINDOWBASE を触らず、ret.n で戻る**葉**。
//
// Why not 途中でヘルパ関数を callx0 で呼ばないか: ADD/SUB のフラグ計算を
// core/ の alu::add へ委ねられれば同値性は自明になるが、ESP-IDF は既定で
// windowed ABI なので、生成した call0 コードから windowed 関数を直接
// callx0 すると entry / retw が食い違って壊れる。call0 のスタブを .S で
// 挟めば繋がるものの、**その繋ぎ目の正しさはホストでは一切検査できず、
// 実機でしか分からない**。フラグ計算を直線で出せば生成コードは葉のままで、
// 検査できない繋ぎ目がゼロになる。V / C の式は host のテストが
// alu::add / alu::sub と全数で突き合わせる。
//
// ## エンコーディングは推測しない
//
// 機械語は xtensa_encoder.h だけが組み立てる。このファイルは 1 バイトも
// 直接書かない。エンコーダの定数はすべて xtensa-esp32s3-elf-as に組ませた値。

#ifndef X68K_PLATFORM_JIT_BLOCK_EMITTER_H
#define X68K_PLATFORM_JIT_BLOCK_EMITTER_H

#include <cstddef>
#include <cstdint>

#include "cpu/block_plan.h"

namespace x68k::jit
{

using std::size_t;

// 生成コードが M68kState の中で使うオフセット。
//
// Why not offsetof を使わないか: 使ってもよいが、**この値が変わったら
// 生成コードが黙って別のメンバを壊す**ことを目立たせたい。ホストのテストが
// static_assert で M68kState の実レイアウトと突き合わせるので、
// core/ 側が動いたらビルドが落ちる。
inline constexpr std::uint32_t kStateDOffset = 0;     // d[0..7]
inline constexpr std::uint32_t kStateAOffset = 32;    // a[0..7]
inline constexpr std::uint32_t kStatePcOffset = 72;   // pc (u32)
inline constexpr std::uint32_t kStateSrOffset = 76;   // sr (u16)
inline constexpr std::uint32_t kStateIrOffset = 78;   // ir (u16)
inline constexpr std::uint32_t kStateIrcOffset = 80;  // irc (u16)

// 戻り値の bit31。**分岐が成立した**ことを呼び出し側へ伝える。
//
// Why not 出口を 2 つに分けて別々のアドレスへ戻らないか: call0 の戻り値は
// a2 の 1 語しかない。サイクル数は 1 ブロックで最大 58 (kMaxOps = 4 の
// 分岐 12 + 非分岐 4 x 3 が上限) なので bit31 は絶対に立たず、
// 1 語に畳んでも情報が失われない。
inline constexpr std::uint32_t kBranchTakenFlag = 0x80000000u;
inline constexpr std::uint32_t kCycleMask = 0x7FFFFFFFu;

// 生成コードのシグネチャ。call0 で呼ぶので、呼び出し側は callx0 のゲートウェイを通す。
//
// **この型で直接呼んではいけない。** ESP-IDF の既定は windowed ABI なので、
// C++ の関数ポインタ呼び出しは call8 になり、生成コードの ret.n と食い違う。
// 型は「何を受けて何を返すか」を書き留めるためだけに置く。
using BlockCodeFn = std::uint32_t (*)(void* state);

// 1 ブロックぶんの発行結果。
struct EmittedBlock
{
    // バッファ先頭からエントリポイントまでのバイト数。
    // リテラルプールがコードより前に置かれるので 0 にならない。
    size_t entryOffset = 0;
    // リテラルプールとコードを合わせた総バイト数。
    size_t totalSize = 0;
    // 呼び出し側が「分岐成立フラグが立ったら」使う飛び先。
    std::uint32_t branchTarget = 0;
    // 分岐で終端したか。false なら戻り値の bit31 は決して立たない。
    bool endsWithBranch = false;
};

// リテラルプールに入れられる語数の上限。
//
// 1 ブロックで要る 32bit 定数は、出口の pc / ir / irc (それぞれ 1 語ずつ、
// ただし movi の -2048..2047 に収まれば消える) と、命令ごとのマスク・符号ビット・
// MOVEQ の即値。kMaxOps = 4 の最悪ケース (ADD.l x 4) を数えて 24 に採ってある。
// 溢れたら emit が false を返し、そのブロックは翻訳しない (保守的に諦める)。
inline constexpr size_t kMaxLiterals = 24;

// BlockPlan を機械語へ落とす。
//
// out には少なくとも requiredSize(plan) バイトの空きが要る。足りなければ
// false を返し、out は未規定 (途中まで書かれている可能性がある)。
//
// fallThroughIr / fallThroughIrc は「非分岐終端 / 分岐不成立のときの出口で
// ir / irc に入るワード」。呼び出し側が plan.fallThroughPc と
// plan.fallThroughPc + 2 を PlanSource から読んで渡す。**I4 がこの 2 語を
// 同一ページ内に保証している**ので、翻訳した時点で必ず読める。
//
// 戻り値: 発行できたら true。out と result が有効。
[[nodiscard]] bool emitBlock(const BlockPlan& plan, std::uint16_t fallThroughIr,
                             std::uint16_t fallThroughIrc, std::uint8_t* out, size_t capacity,
                             EmittedBlock& result);

// plan を発行するのに要るバイト数を返す。**emitBlock と同じ経路で数える**ので、
// 見積もりではなく実際の値。0 を返したら発行できない (許可されていない kind が
// 混じっている、リテラルが溢れる)。
[[nodiscard]] size_t requiredSize(const BlockPlan& plan, std::uint16_t fallThroughIr,
                                  std::uint16_t fallThroughIrc);

}  // namespace x68k::jit

#endif  // X68K_PLATFORM_JIT_BLOCK_EMITTER_H
