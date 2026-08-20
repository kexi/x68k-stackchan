// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// BlockPlan から発行した Xtensa コードが、同じ計画をインタプリタで実行したときと
// **ビット単位で同じ状態**を作ること。
//
// ## ホストで実行できないものを、どうやってホストで検査するか
//
// 発行されるのは Xtensa の機械語なので、Mac の上では走らない。走らせずに
// 正しさを問う手段は 3 つあり、このファイルは 3 つとも使う。
//
//   1. **バイト列を読み解く** — 出口で s32i / s16i される先を数え、
//      d/a/pc/sr/ir/irc が 1 つも漏れていないことを機械的に確かめる。
//      漏れたレジスタは次のブロックが読むまで誰にも見えないので、
//      「動いているように見える」時間が長い。
//   2. **発行したコードを解釈実行する** — このファイルが持つ小さな
//      Xtensa インタプリタ (kEmitted... 以下) で発行結果を走らせ、
//      M68k::step() を回した結果と M68kState を丸ごと比べる。
//      **これが同値テストの本体。**
//   3. **決定性** — 同じ計画からは同じバイト列が出ること。
//
// ## なぜ 2 が要るか
//
// 1 だけだと「書き戻している」ことは分かっても「正しい値を」書き戻している
// ことは分からない。ADD の V と C を分岐なしのビット演算で出す部分は、
// 式を読んで納得しても根拠にならない。実際に走らせて alu::add と突き合わせる。
//
// ## Xtensa インタプリタの立ち位置
//
// **これはエミュレータではなく、テストの計算器**。発行器が使う命令だけを
// 解釈する。知らない命令に当たったら黙って進まず、その場で落とす
// (発行器が新しい命令を使い始めたら、テストが必ず気づく)。

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "cpu/block_plan.h"
#include "cpu/block_planner.h"
#include "cpu/code_gen_map.h"
#include "cpu/m68k_alu.h"
#include "cpu/m68k_length.h"
#include "cpu/m68k_types.h"
#include "jit/block_emitter.h"
#include "jit/block_runner.h"
#include "jit/exec_memory.h"
#include "jit/negative_cache.h"
#include "jit/xtensa_encoder.h"
#include "machine.h"
#include "doctest.h"

using x68k::BlockEnd;
using x68k::BlockPlan;
using x68k::BlockPlanner;
using x68k::M68kState;
using x68k::PlanAluOp;
using x68k::PlanGenSource;
using x68k::PlanKind;
using x68k::PlannedOp;
using x68k::PlanSource;
using x68k::u16;
using x68k::u32;
using x68k::u8;

namespace jit = x68k::jit;

namespace
{

// --- 発行器が M68kState のどこを触るかの前提 -------------------------------
//
// block_emitter.h はオフセットを数値で持っている。core/ 側のレイアウトが
// 動いたら、生成コードは黙って別のメンバを壊す。ここで縛る。
static_assert(offsetof(M68kState, d) == jit::kStateDOffset, "d[] の位置が動いた");
static_assert(offsetof(M68kState, a) == jit::kStateAOffset, "a[] の位置が動いた");
static_assert(offsetof(M68kState, pc) == jit::kStatePcOffset, "pc の位置が動いた");
static_assert(offsetof(M68kState, sr) == jit::kStateSrOffset, "sr の位置が動いた");
static_assert(offsetof(M68kState, ir) == jit::kStateIrOffset, "ir の位置が動いた");
static_assert(offsetof(M68kState, irc) == jit::kStateIrcOffset, "irc の位置が動いた");

// --- 発行したコードを走らせる小さな Xtensa 解釈器 ---------------------------

// 実行の結果。
struct XtensaRun
{
    bool ok = false;        // 未知の命令に当たらずに ret.n まで到達した
    std::uint32_t ret = 0;  // a2 の値
    const char* failure = nullptr;
};

// 発行器が使う命令だけを解釈する。
//
// **知らない命令は必ず失敗にする。** ここで「読み飛ばす」ようにすると、
// 発行器が命令を足したときにテストが緑のまま通ってしまう。
class XtensaCpu
{
public:
    XtensaCpu(const std::uint8_t* code, std::size_t size, std::uint8_t* mem, std::size_t memSize)
        : code_(code), size_(size), mem_(mem), memSize_(memSize)
    {
    }

    // ゲスト RAM の窓を教える。読みガード付きの命令はここを l8ui で読む。
    //
    // **状態領域とは別の空間**として持つ。生成コードから見えるゲスト RAM の
    // 先頭アドレスは kFakeWindow で、状態は 0 番地から sizeof(M68kState)。
    // 重なりようがない値を選んであるので、どちらの領域でもないアドレスは
    // その場で失敗にできる (踏んだら「テストが固まる」ではなく「落ちる」)。
    void setGuestRam(std::uint32_t base, std::uint8_t* ram, std::size_t size)
    {
        ramBase_ = base;
        ram_ = ram;
        ramSize_ = size;
    }

    // ページ世代の配列を教える。書き形の touch が l16ui / s16i で触る (Tier C)。
    //
    // **ゲスト RAM とも状態領域とも別の空間**として持つ。3 つとも重ならない
    // 値を選んであるので、どの領域でもないアドレスはその場で失敗にできる。
    // touch の書き先を間違える変異は「ゲスト RAM を潰す」ではなく
    // 「範囲外を触った」として落ちる。
    void setGenMap(std::uint32_t base, std::uint8_t* gen, std::size_t size)
    {
        genBase_ = base;
        gen_ = gen;
        genSize_ = size;
    }

    // 動的分岐 (Tier D) が飛び先を置く 1 語。
    //
    // **ゲスト RAM とも世代配列とも状態領域とも別の空間**として持つ。
    // 4 つとも重ならない値を選んであるので、飛び先の書き先を間違える変異は
    // 「別の領域を潰す」ではなく「範囲外を触った」として落ちる。
    void setMailbox(std::uint32_t base, std::uint8_t* box, std::size_t size)
    {
        mailboxBase_ = base;
        mailbox_ = box;
        mailboxSize_ = size;
    }

    XtensaRun run(std::size_t entry, std::uint32_t arg)
    {
        for (int i = 0; i < 16; ++i)
        {
            a_[i] = 0xDEADBEEFu;
        }
        a_[2] = arg;
        pc_ = entry;
        touchedForbidden_ = false;

        // 発行するのは直線コードで、後方分岐は無い。上限は「1 命令 2 バイトで
        // バッファを舐め尽くす回数」より少し多め。無限ループになったら落とす。
        for (int steps = 0; steps < 4096; ++steps)
        {
            if (pc_ + 2 > size_)
            {
                return {false, 0, "コードの外へ出た"};
            }
            const std::uint32_t w = word(pc_);
            const std::uint32_t op0 = w & 0xFu;

            // ret.n
            if ((w & 0xFFFFu) == 0xF00Du)
            {
                if (touchedForbidden_)
                {
                    // 生成コードが a12-a15 に触った。実機なら祖先フレームを
                    // 壊すが、ホストでは症状が出ない。ここで落とす。
                    return {false, 0, "a12 以降 (窓の外) に触った"};
                }
                return {true, a_[2], nullptr};
            }

            const char* err = execute(w, op0);
            if (err != nullptr)
            {
                return {false, 0, err};
            }
            if (outOfRange_)
            {
                return {false, 0, "M68kState の外を触った"};
            }
        }
        return {false, 0, "命令数の上限に達した (無限ループ?)"};
    }

private:
    static std::uint32_t nib(std::uint32_t v, unsigned i)
    {
        return (v >> (4u * i)) & 0xFu;
    }

    [[nodiscard]] std::uint32_t word(std::size_t at) const
    {
        std::uint32_t v = static_cast<std::uint32_t>(code_[at]);
        if (at + 1 < size_)
        {
            v |= static_cast<std::uint32_t>(code_[at + 1]) << 8;
        }
        if (at + 2 < size_)
        {
            v |= static_cast<std::uint32_t>(code_[at + 2]) << 16;
        }
        return v;
    }

    // 生成コードが触ってよいのは 2 つの領域だけ。
    //
    //   [0, sizeof(M68kState))         — a2 (= 状態の先頭) からの相対
    //   [ramBase_, ramBase_ + ramSize_) — 読みガードを通ったゲスト RAM
    //
    // **どちらでもないアドレスはその場で失敗にする。** ここを素通しにすると、
    // プロローグを落とすような変異や、ガードをすり抜けた読みが
    // 「ホストのメモリを踏んで落ちる」形で現れ、テストが緑にも赤にもならず固まる。
    //
    // 戻り値は「その領域の実体へのポインタ」。解決できなければ nullptr を返し、
    // outOfRange_ を立てる (呼び出し側は値を使わない)。
    [[nodiscard]] std::uint8_t* resolve(std::uint32_t addr, std::size_t len)
    {
        if (static_cast<std::size_t>(addr) + len <= memSize_)
        {
            return mem_ + addr;
        }
        if (ram_ != nullptr && addr >= ramBase_)
        {
            const std::size_t off = static_cast<std::size_t>(addr - ramBase_);
            if (off + len <= ramSize_)
            {
                return ram_ + off;
            }
        }
        if (gen_ != nullptr && addr >= genBase_)
        {
            const std::size_t off = static_cast<std::size_t>(addr - genBase_);
            if (off + len <= genSize_)
            {
                return gen_ + off;
            }
        }
        if (mailbox_ != nullptr && addr >= mailboxBase_)
        {
            const std::size_t off = static_cast<std::size_t>(addr - mailboxBase_);
            if (off + len <= mailboxSize_)
            {
                return mailbox_ + off;
            }
        }
        outOfRange_ = true;
        return nullptr;
    }

    [[nodiscard]] std::uint32_t load32(std::uint32_t addr)
    {
        const std::uint8_t* at = resolve(addr, 4);
        if (at == nullptr)
        {
            return 0;
        }
        std::uint32_t v = 0;
        std::memcpy(&v, at, 4);
        return v;
    }
    void store32(std::uint32_t addr, std::uint32_t v)
    {
        std::uint8_t* at = resolve(addr, 4);
        if (at == nullptr)
        {
            return;
        }
        std::memcpy(at, &v, 4);
    }
    [[nodiscard]] std::uint32_t load16(std::uint32_t addr)
    {
        const std::uint8_t* at = resolve(addr, 2);
        if (at == nullptr)
        {
            return 0;
        }
        std::uint16_t v = 0;
        std::memcpy(&v, at, 2);
        return v;
    }
    void store16(std::uint32_t addr, std::uint32_t v)
    {
        std::uint8_t* at = resolve(addr, 2);
        if (at == nullptr)
        {
            return;
        }
        const std::uint16_t h = static_cast<std::uint16_t>(v & 0xFFFFu);
        std::memcpy(at, &h, 2);
    }
    [[nodiscard]] std::uint32_t load8(std::uint32_t addr)
    {
        const std::uint8_t* at = resolve(addr, 1);
        return at == nullptr ? 0u : static_cast<std::uint32_t>(*at);
    }
    void store8(std::uint32_t addr, std::uint32_t v)
    {
        std::uint8_t* at = resolve(addr, 1);
        if (at == nullptr)
        {
            return;
        }
        *at = static_cast<std::uint8_t>(v & 0xFFu);
    }

    // 1 命令。認識できたら pc_ を進めて nullptr を返す。
    const char* execute(std::uint32_t w, std::uint32_t op0)
    {
        const std::uint32_t t = nib(w, 1);
        const std::uint32_t s = nib(w, 2);
        const std::uint32_t r = nib(w, 3);
        const std::uint32_t op1 = nib(w, 4);
        const std::uint32_t op2 = nib(w, 5);
        const std::uint32_t imm8 = (w >> 16) & 0xFFu;

        switch (op0)
        {
            case 0x1u:  // l32r at, imm16
            {
                const std::int32_t off =
                    static_cast<std::int16_t>(static_cast<std::uint16_t>((w >> 8) & 0xFFFFu));
                const std::int64_t lit =
                    static_cast<std::int64_t>((pc_ + 3) & ~static_cast<std::size_t>(3)) +
                    static_cast<std::int64_t>(off) * 4;
                if (lit < 0 || static_cast<std::size_t>(lit) + 4 > size_)
                {
                    return "l32r がバッファの外を指した";
                }
                std::uint32_t v = 0;
                std::memcpy(&v, code_ + lit, 4);
                reg(static_cast<int>(t)) = v;
                pc_ += 3;
                return nullptr;
            }
            case 0x2u:  // RRI8 群
                switch (r)
                {
                    case 0x0u:  // l8ui at, as, imm8
                        // **オフセットを割らない。** l16ui (2 で割る) /
                        // l32i (4 で割る) と粒度が違う。
                        reg(static_cast<int>(t)) = load8(reg(static_cast<int>(s)) + imm8);
                        pc_ += 3;
                        return nullptr;
                    case 0x1u:  // l16ui at, as, imm8*2
                        reg(static_cast<int>(t)) = load16(reg(static_cast<int>(s)) + imm8 * 2u);
                        pc_ += 3;
                        return nullptr;
                    case 0x2u:  // l32i at, as, imm8*4
                        reg(static_cast<int>(t)) = load32(reg(static_cast<int>(s)) + imm8 * 4u);
                        pc_ += 3;
                        return nullptr;
                    case 0x4u:  // s8i at, as, imm8
                        // **オフセットを割らない。** l8ui と同じ粒度で、
                        // s16i (2 で割る) / s32i (4 で割る) とは違う。
                        store8(reg(static_cast<int>(s)) + imm8, reg(static_cast<int>(t)));
                        pc_ += 3;
                        return nullptr;
                    case 0x5u:  // s16i
                        store16(reg(static_cast<int>(s)) + imm8 * 2u, reg(static_cast<int>(t)));
                        pc_ += 3;
                        return nullptr;
                    case 0x6u:  // s32i
                        store32(reg(static_cast<int>(s)) + imm8 * 4u, reg(static_cast<int>(t)));
                        pc_ += 3;
                        return nullptr;
                    case 0xAu:  // movi at, imm12 (s は imm[11:8])
                    {
                        const std::uint32_t raw = (s << 8) | imm8;
                        // 12bit 符号付き。
                        const std::int32_t v = (raw & 0x800u) != 0
                                                   ? static_cast<std::int32_t>(raw | 0xFFFFF000u)
                                                   : static_cast<std::int32_t>(raw);
                        reg(static_cast<int>(t)) = static_cast<std::uint32_t>(v);
                        pc_ += 3;
                        return nullptr;
                    }
                    case 0xCu:  // addi at, as, imm8 (符号付き)
                        reg(static_cast<int>(t)) =
                            reg(static_cast<int>(s)) +
                            static_cast<std::uint32_t>(static_cast<std::int32_t>(
                                static_cast<std::int8_t>(static_cast<std::uint8_t>(imm8))));
                        pc_ += 3;
                        return nullptr;
                    default:
                        return "未知の RRI8 命令";
                }
            case 0x0u:  // RRR 群
                if (op1 == 0x4u || op1 == 0x5u)
                {
                    // extui ar, at, shiftimm, maskimm
                    //
                    // **op1 が 0x4|shiftimm[4]、op2 が maskimm-1。**
                    // 素直に読むと逆に置きたくなるところで、実際ここを
                    // 取り違えて 1 度落とした (xtensa_encoder.h も同じ罠を
                    // 冒頭に記録している)。
                    const std::uint32_t sh = s | ((op1 & 1u) << 4);
                    const std::uint32_t mask = op2 + 1u;
                    const std::uint32_t m = mask >= 32u ? 0xFFFFFFFFu : ((1u << mask) - 1u);
                    reg(static_cast<int>(r)) = (reg(static_cast<int>(t)) >> sh) & m;
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x1u && op1 == 0x0u)
                {
                    and_op(r, s, t);
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x2u && op1 == 0x0u)
                {
                    reg(static_cast<int>(r)) = reg(static_cast<int>(s)) | reg(static_cast<int>(t));
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x3u && op1 == 0x0u)
                {
                    reg(static_cast<int>(r)) = reg(static_cast<int>(s)) ^ reg(static_cast<int>(t));
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0xCu && op1 == 0x0u)
                {
                    reg(static_cast<int>(r)) = reg(static_cast<int>(s)) - reg(static_cast<int>(t));
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x6u && op1 == 0x0u && s == 0u)
                {
                    reg(static_cast<int>(r)) = 0u - reg(static_cast<int>(t));
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x8u && op1 == 0x3u)
                {
                    if (reg(static_cast<int>(t)) == 0u)
                    {
                        reg(static_cast<int>(r)) = reg(static_cast<int>(s));
                    }
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x9u && op1 == 0x3u)
                {
                    if (reg(static_cast<int>(t)) != 0u)
                    {
                        reg(static_cast<int>(r)) = reg(static_cast<int>(s));
                    }
                    pc_ += 3;
                    return nullptr;
                }
                if (op1 == 0x1u && (op2 == 0x0u || op2 == 0x1u))
                {
                    // slli ar, as, n。符号化されているのは 32 - n。
                    const std::uint32_t sa = t | (op2 << 4);
                    const std::uint32_t n = 32u - sa;
                    reg(static_cast<int>(r)) = n >= 32u ? 0u : (reg(static_cast<int>(s)) << n);
                    pc_ += 3;
                    return nullptr;
                }
                return "未知の RRR 命令";
            case 0x6u:  // BRI12 / j
            {
                const std::uint32_t sub = nib(w, 1);
                if (sub == 0x1u || sub == 0x5u)
                {
                    const std::uint32_t raw = (w >> 12) & 0xFFFu;
                    const std::int32_t disp = (raw & 0x800u) != 0
                                                  ? static_cast<std::int32_t>(raw | 0xFFFFF000u)
                                                  : static_cast<std::int32_t>(raw);
                    const bool take = sub == 0x1u ? (reg(static_cast<int>(s)) == 0u)
                                                  : (reg(static_cast<int>(s)) != 0u);
                    pc_ += 3;
                    if (take)
                    {
                        // 変位の基準は「命令先頭 + 4」。3 バイト進めた後なので +1。
                        pc_ = static_cast<std::size_t>(static_cast<std::int64_t>(pc_) + 1 + disp);
                    }
                    return nullptr;
                }
                return "未知の BRI12 命令";
            }
            case 0xAu:  // add.n ar, as, at
                reg(static_cast<int>(r)) = reg(static_cast<int>(s)) + reg(static_cast<int>(t));
                pc_ += 2;
                return nullptr;
            case 0x8u:  // l32i.n at, as, r*4
                reg(static_cast<int>(t)) = load32(reg(static_cast<int>(s)) + r * 4u);
                pc_ += 2;
                return nullptr;
            case 0x9u:  // s32i.n
                store32(reg(static_cast<int>(s)) + r * 4u, reg(static_cast<int>(t)));
                pc_ += 2;
                return nullptr;
            case 0xCu:  // movi.n as, imm7
            {
                const std::uint32_t raw = (t << 4) | r;
                const std::int32_t v = (raw & 0x60u) == 0x60u
                                           ? static_cast<std::int32_t>(raw | 0xFFFFFF80u)
                                           : static_cast<std::int32_t>(raw);
                reg(static_cast<int>(s)) = static_cast<std::uint32_t>(v);
                pc_ += 2;
                return nullptr;
            }
            case 0xDu:  // mov.n at, as (r == 0)
                if (r != 0u)
                {
                    return "未知の RRRN 命令 (0xD)";
                }
                reg(static_cast<int>(t)) = reg(static_cast<int>(s));
                pc_ += 2;
                return nullptr;
            default:
                return "未知の命令";
        }
    }

    void and_op(std::uint32_t r, std::uint32_t s, std::uint32_t t)
    {
        reg(static_cast<int>(r)) = reg(static_cast<int>(s)) & reg(static_cast<int>(t));
    }

    const std::uint8_t* code_;
    std::size_t size_;
    std::uint8_t* mem_;
    std::size_t memSize_ = 0;
    // ゲスト RAM の窓 (読みガードを通った命令だけが触る)。
    std::uint32_t ramBase_ = 0;
    std::uint8_t* ram_ = nullptr;
    std::size_t ramSize_ = 0;
    // ページ世代の配列 (書き形の touch が触る)。
    std::uint32_t genBase_ = 0;
    std::uint8_t* gen_ = nullptr;
    std::size_t genSize_ = 0;
    // 動的分岐の飛び先を置く 1 語 (Tier D)。
    std::uint32_t mailboxBase_ = 0;
    std::uint8_t* mailbox_ = nullptr;
    std::size_t mailboxSize_ = 0;
    bool outOfRange_ = false;
    // **a12-a15 に触ったら即座に失敗させる。**
    //
    // 生成コードは a2-a11 しか使ってはいけない。呼び出し元の runBlock は
    // `entry a1, 32` (call4 の窓) でコンパイルされるので、a12 以降は
    // **窓の外 = 祖先フレームの生きた値**にあたる。書くと呼び出し元の
    // さらに呼び出し元のローカル変数を静かに壊す。
    //
    // 実機では窓のオーバーフロー例外で初めて症状が出るので、呼び出し
    // 深さに依存して散発的に現れ、原因から最も遠い形で壊れる。
    // **一度実際に踏んでいる** (ゲートウェイが a12 を使っていた)。
    //
    // ホストのミニ解釈器は 16 本すべてを持つので、a12 を使う変異を入れても
    // テストが全部通ってしまっていた。ここで縛れば、**既存の全テストが
    // そのまま制約の検査になる**。
    static constexpr int kMaxUsableReg = 11;
    mutable bool touchedForbidden_ = false;

    [[nodiscard]] std::uint32_t& reg(int i)
    {
        if (i > kMaxUsableReg)
        {
            touchedForbidden_ = true;
        }
        return a_[i];
    }

    std::uint32_t a_[16] = {};
    std::size_t pc_ = 0;
};

// --- 計画を組むための土台 ---------------------------------------------------

// 翻訳器へ渡す平坦なコードメモリ。test_block_plan.cpp と同じ形。
struct FlatCode
{
    u32 begin = 0;
    u32 end = 0;
    std::vector<u8> bytes;

    void reset(u32 windowBegin, u32 windowEnd)
    {
        begin = windowBegin;
        end = windowEnd;
        bytes.assign(windowEnd - windowBegin, 0);
    }

    void put16(u32 addr, u16 value)
    {
        bytes[addr - begin] = static_cast<u8>(value >> 8);
        bytes[addr - begin + 1] = static_cast<u8>(value & 0xFFu);
    }

    [[nodiscard]] u16 get16(u32 addr) const
    {
        if (addr + 1 >= end || addr < begin)
        {
            return 0;
        }
        return static_cast<u16>((static_cast<u16>(bytes[addr - begin]) << 8) |
                                bytes[addr - begin + 1]);
    }

    static bool read(void* ctx, u32 addr, u16& out)
    {
        auto* self = static_cast<FlatCode*>(ctx);
        if (addr < self->begin || addr + 1 >= self->end)
        {
            return false;
        }
        out = self->get16(addr);
        return true;
    }
};

// 世代は常に有効なものを返す。
u16 fakeGeneration(void*, u32)
{
    return 1;
}
u32 fakeEpoch(void*)
{
    return 7;
}

PlanSource makeSource(FlatCode& code)
{
    return PlanSource{&FlatCode::read, &code};
}
PlanGenSource makeGen()
{
    return PlanGenSource{&fakeGeneration, &fakeEpoch, nullptr};
}

// 命令語を並べて計画を組む。entryPc は 1KB ページの先頭から少し内側。
constexpr u32 kEntry = 0x2000;

bool buildPlan(const std::vector<u16>& words, BlockPlan& plan, FlatCode& code,
               const x68k::PlanCapabilities& caps)
{
    code.reset(0x1000, 0x3000);
    u32 at = kEntry;
    for (u16 w : words)
    {
        code.put16(at, w);
        at += 2;
    }
    return BlockPlanner::plan(makeSource(code), makeGen(), kEntry, plan, caps);
}

// 既定の能力 (何も制限しない) で組む。**大半のテストはこちらを呼ぶ。**
bool buildPlan(const std::vector<u16>& words, BlockPlan& plan, FlatCode& code)
{
    return buildPlan(words, plan, code, x68k::PlanCapabilities{});
}

// --- 発行と実行 -------------------------------------------------------------

struct EmitResult
{
    std::vector<std::uint8_t> buffer;
    jit::EmittedBlock info;
    bool ok = false;
};

// 偽のゲスト RAM 窓。
//
// 生成コードが読むホストアドレスは「窓の先頭 + マスク済みゲストアドレス」。
// **状態領域 ([0, sizeof(M68kState)) = 84 バイト) と重ならない値**を選ぶ。
// マスク済みゲストアドレスは高々 0xFFFFFF なので、下の基点なら
// 状態領域へ落ちてくることはない。ミニ解釈器はこの 2 領域の外を
// 触られたらその場で失敗にする。
constexpr std::uint32_t kFakeWindow = 0x01000000u;

// 生成コードから見える世代配列の先頭アドレス。
//
// **状態領域ともゲスト RAM の窓とも重ならない値**にする。ゲスト RAM の窓は
// [kFakeWindow, kFakeWindow + 2MB) なので、その先へ置く。ミニ解釈器は
// 3 領域の外を触られたらその場で失敗にするので、touch の宛先を間違える
// 変異は「ゲスト RAM を潰す」ではなく「範囲外を触った」として落ちる。
constexpr std::uint32_t kFakeGenWindow = 0x02000000u;

// 世代配列のページ数。2MB / 1KB = 2048。
constexpr u32 kGenPages = x68k::kMainRamSize >> x68k::CodeGenMap::kPageShift;

// 生成コードから見えるメールボックスの先頭アドレス (Tier D)。
//
// **状態領域ともゲスト RAM の窓とも世代配列とも重ならない値**にする。
// 4 領域とも重ならないので、飛び先の書き先を間違える変異は「ゲスト RAM を
// 潰す」でも「世代を潰す」でもなく「範囲外を触った」として落ちる。
constexpr std::uint32_t kFakeMailbox = 0x03000000u;

// 生成コードが飛び先を書き込む 1 語。**参照側は書かない。**
//
// runner の branchMailbox_ に相当する。ミニ解釈器へは 4 バイトの領域として
// 渡し、テストは走らせた後にここを読んで飛び先を問う。
std::uint32_t& execMailbox()
{
    static std::uint32_t storage = 0;
    return storage;
}

jit::EmitEnv fakeEnv()
{
    jit::EmitEnv env{};
    env.ramBaseAddr = kFakeWindow;
    env.ramLimit = static_cast<std::uint32_t>(x68k::kMainRamSize);
    env.ramReadable = true;
    env.genBaseAddr = kFakeGenWindow;
    env.genPageCount = kGenPages;
    env.mailboxAddr = kFakeMailbox;
    return env;
}

EmitResult emit(const BlockPlan& plan, const FlatCode& code, const jit::EmitEnv& env)
{
    EmitResult r{};
    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);
    const std::size_t need = jit::requiredSize(plan, ir, irc, env);
    if (need == 0)
    {
        return r;
    }
    r.buffer.assign(need, 0xCC);
    r.ok = jit::emitBlock(plan, ir, irc, env, r.buffer.data(), r.buffer.size(), r.info);
    return r;
}

EmitResult emit(const BlockPlan& plan, const FlatCode& code)
{
    return emit(plan, code, fakeEnv());
}

// M68kState を、生成コードから見えるのと同じ平坦なメモリに置いて走らせる。
//
// 生成コードは a2 に渡されたアドレスからの相対でしか触らないので、
// 「状態を 0 番地に置いた平坦なメモリ」を用意すれば解釈器で完結する。
// **範囲外を触ったら ASan / UBSan が落とす**ように、前後に余白を取る。
struct StateMemory
{
    static constexpr std::size_t kPad = 64;
    std::vector<std::uint8_t> bytes;

    StateMemory()
    {
        bytes.assign(kPad + sizeof(M68kState) + kPad, 0);
    }

    void load(const M68kState& s)
    {
        std::memcpy(bytes.data() + kPad, &s, sizeof(M68kState));
    }
    [[nodiscard]] M68kState store() const
    {
        M68kState s{};
        std::memcpy(&s, bytes.data() + kPad, sizeof(M68kState));
        return s;
    }
    std::uint8_t* base()
    {
        return bytes.data() + kPad;
    }
};

// 発行したコードを走らせ、状態と戻り値を返す。
struct NativeOutcome
{
    bool ok = false;
    const char* failure = nullptr;
    M68kState state{};
    std::uint32_t cycles = 0;
    bool branchTaken = false;
    // 読みガードが不成立で降りたか (Tier B)。
    bool guardExit = false;
    // 自ページ書き換えで降りたか (Tier C の G13/G18)。
    bool selfPageExit = false;
    // 動的な飛び先へ分岐したか (Tier D)。
    bool dynamicBranch = false;
    // そのときメールボックスに書かれていた飛び先。
    std::uint32_t mailbox = 0;
    std::uint8_t ranOps = 0;
};

// 参照側と生成側が**同じ実体**を見るゲスト RAM (定義は下)。
std::vector<u8>& execRam();

// 生成コードが touch で触るページ世代の配列 (定義は下)。
std::vector<std::uint16_t>& execGen();

NativeOutcome runEmitted(const EmitResult& e, const M68kState& initial)
{
    NativeOutcome out{};
    if (!e.ok)
    {
        out.failure = "発行に失敗した";
        return out;
    }
    StateMemory mem;
    mem.load(initial);

    // 解釈器は「コード配列の先頭 = アドレス 0」として動く。a2 には
    // M68kState の先頭アドレスを渡すが、解釈器のメモリ空間は mem 側なので、
    // ここでは 0 を渡して mem.base() を基点にする。
    XtensaCpu cpu(e.buffer.data(), e.buffer.size(), mem.base(), sizeof(M68kState));
    // 読みガードを通った命令が読む先。**参照側 (runReference) と同じ配列**を
    // 渡すので、両者は同じバイト列を見る。
    //
    // **書き形 (Tier C) はここを書き換える。** 呼び出し側は参照側を回す前に
    // 生成側の結果を控えて、両者を別々に比べること (checkWriteEquivalence)。
    cpu.setGuestRam(kFakeWindow, execRam().data(), execRam().size());
    cpu.setGenMap(kFakeGenWindow, reinterpret_cast<std::uint8_t*>(execGen().data()),
                  execGen().size() * sizeof(std::uint16_t));
    // 動的分岐の飛び先の置き場 (Tier D)。**走らせる前に潰しておく。**
    // 前回の値が残っていると、飛び先を 1 語も書かない変異が
    // 「たまたま前回と同じ値」で通ってしまう。
    execMailbox() = 0xDEADBEEFu;
    cpu.setMailbox(kFakeMailbox, reinterpret_cast<std::uint8_t*>(&execMailbox()),
                   sizeof(std::uint32_t));
    const XtensaRun r = cpu.run(e.info.entryOffset, 0);
    if (!r.ok)
    {
        out.failure = r.failure;
        return out;
    }
    out.ok = true;
    out.state = mem.store();

    const jit::BlockReturn decoded = jit::decodeBlockReturn(r.ret);
    out.branchTaken = decoded.branchTaken;
    out.cycles = decoded.cycles;
    out.guardExit = decoded.guardExit;
    out.selfPageExit = decoded.selfPageExit;
    out.dynamicBranch = decoded.dynamicBranch;
    out.mailbox = execMailbox();
    out.ranOps = decoded.ranOps;
    return out;
}

// --- 参照側 (インタプリタ) --------------------------------------------------

// Machine を立てて、同じ命令列を M68k::step() で回す。
//
// Why not 参照実装を手で書かないか: 手で書くと「発行器と参照が同じ勘違いを
// している」ことを検出できない。突き合わせ相手は必ず本物のインタプリタにする。
//
// **メインメモリを自前の配列で用意する** (test_block_plan.cpp と同じ形)。
// Machine::reset() だけでは 0x2000 に RAM が張られておらず、書いても消える。
std::vector<u8>& execRam()
{
    static std::vector<u8> storage(x68k::kMainRamSize, 0);
    return storage;
}

// ページ世代の配列。**参照側 (CodeGenMap) と生成側 (touch) が同じ実体を見る。**
//
// Tier C の肝はここで、状態だけを比べても touch の回数のずれは見えない
// (飽和が絡まないかぎり世代は状態に一切影響しない)。**世代配列をビット比較の
// 対象に入れる**ことで、`.l` の 2 回を 1 回に畳む変異が落ちるようになる。
std::vector<std::uint16_t>& execGen()
{
    static std::vector<std::uint16_t> storage(kGenPages, 0);
    return storage;
}

void ramPoke16(u32 a, u16 v)
{
    execRam()[a] = static_cast<u8>(v >> 8);
    execRam()[a + 1] = static_cast<u8>(v & 0xFFu);
}

// words を kEntry へ置き、initial から count 命令だけ回した結果を返す。
// 読み形が読むデータ。**参照側と生成側で同じバイト列を見せる**ための種。
//
// runReference は毎回 execRam を 0 で埋め直すので、データはそこへ
// 置き直さないと消える。テストが 1 箇所に書いておけば、参照実行の前に
// 必ず同じ内容が復元される。
struct GuestSeed
{
    u32 addr = 0;
    std::vector<u8> bytes;
};
std::vector<GuestSeed>& guestSeeds()
{
    static std::vector<GuestSeed> s;
    return s;
}

void applyGuestSeeds()
{
    for (const GuestSeed& seed : guestSeeds())
    {
        for (std::size_t i = 0; i < seed.bytes.size(); ++i)
        {
            execRam()[seed.addr + i] = seed.bytes[i];
        }
    }
}

// ゲスト RAM を「実行前の状態」に戻す。
//
// **参照側と生成側でまったく同じ手順を踏む**ためだけに切り出してある。
// 片方だけが命令語を置き直したり種を撒き直したりすると、書き形の比較
// (どちらもメモリを書き換える) が意味を失う。
void resetExecRam(const std::vector<u16>& words)
{
    std::fill(execRam().begin(), execRam().end(), 0);
    ramPoke16(0, 0x0000);
    ramPoke16(2, 0x8000);  // SSP
    ramPoke16(4, 0x0000);
    ramPoke16(6, static_cast<u16>(kEntry));
    // 命令列の前後を NOP で埋める。**出口の ir / irc がここから来る**ので、
    // 発行器へ渡した FlatCode と同じ値でなければ比較そのものが意味を失う。
    for (u32 a = kEntry - 0x400; a < kEntry + 0x400; a += 2)
    {
        ramPoke16(a, 0x4E71);
    }
    u32 at = kEntry;
    for (const u16 w : words)
    {
        ramPoke16(at, w);
        at += 2;
    }
    // **命令語を置いた後に種を撒く。** 順が逆だと NOP 埋めがデータを消す。
    applyGuestSeeds();
}

// 世代配列を「実行前の状態」に戻す。既定は全ページ 0。
//
// seedPage / seedGen が渡されたら、そのページだけ別の値にする
// (飽和の境界を作るのに使う)。
void resetExecGen(u32 seedPage = 0xFFFFFFFFu, std::uint16_t seedGen = 0)
{
    std::fill(execGen().begin(), execGen().end(), static_cast<std::uint16_t>(0));
    if (seedPage < execGen().size())
    {
        execGen()[seedPage] = seedGen;
    }
}

M68kState runReference(const std::vector<u16>& words, const M68kState& initial, u32 count,
                       u32& cyclesOut)
{
    resetExecRam(words);

    x68k::Machine m;
    x68k::MemoryMap map{};
    map.mainRam = execRam().data();
    m.setMemory(map);
    m.reset();

    M68kState s = initial;
    m.cpu().loadStateForTest(s);
    // 入口の事前条件 (pc == X + 4 / ir == mem16(X) / irc == mem16(X + 2)) を
    // インタプリタ自身に作らせる。手で組むと定義を取り違えたときに気づけない。
    m.cpu().refillPrefetchForTest(kEntry);

    // **世代配列を配線する。** 生成コードの touch と突き合わせる相手が
    // ここに要る。配線しないと CodeGenMap::touch は何もせず (pageCount_ == 0)、
    // 「生成側だけが世代を上げる」ことになって比較が常に落ちる。
    //
    // **loadStateForTest より後で配線する。** あれは touchAll() を呼んで
    // 全ページの世代を 1 つ進める。先に配線すると、参照側だけが全ページ
    // +1 された状態から走り出し、比較が「生成側は 0、参照側は 1」で
    // 常に落ちる。ここで測りたいのは**命令が動かした世代**だけ。
    m.cpu().codeGenMap().setStorage(execGen().data(), static_cast<u32>(execGen().size()));

    cyclesOut = 0;
    for (u32 i = 0; i < count; ++i)
    {
        cyclesOut += m.cpu().step();
    }
    return m.cpu().state();
}

// 発行コードとインタプリタを突き合わせる。
//
// 分岐成立側は生成コードが pc / ir / irc を書かない契約なので、
// **その 3 つだけは比較から外し、代わりに「分岐成立フラグが立ったこと」と
// 「飛び先が計画と一致すること」を問う。**
void compareStates(const M68kState& want, const M68kState& got, bool skipPrefetch, const char* what)
{
    INFO(std::string(what));
    for (int i = 0; i < 8; ++i)
    {
        INFO("d[", i, "]");
        CHECK(got.d[i] == want.d[i]);
    }
    for (int i = 0; i < 8; ++i)
    {
        INFO("a[", i, "]");
        CHECK(got.a[i] == want.a[i]);
    }
    INFO("sr");
    CHECK(got.sr == want.sr);
    // usp / ssp / stopped / halted は生成コードが触らない契約。
    CHECK(got.usp == want.usp);
    CHECK(got.ssp == want.ssp);
    CHECK(got.stopped == want.stopped);
    CHECK(got.halted == want.halted);
    if (!skipPrefetch)
    {
        INFO("pc");
        CHECK(got.pc == want.pc);
        INFO("ir");
        CHECK(got.ir == want.ir);
        INFO("irc");
        CHECK(got.irc == want.irc);
    }
}

// ゲスト RAM と世代配列を、参照側と生成側でビット比較する。
//
// **世代配列まで比べるのが Tier C の肝。** 状態 (M68kState) だけを比べても
// touch の回数のずれは見えない — 飽和が絡まないかぎり、世代は状態に
// 一切影響しないから。`.l` の 2 回を 1 回に畳む変異は、この比較でしか落ちない。
//
// 食い違った先頭だけを報告する。2MB / 2048 ページを全部 CHECK すると
// doctest のアサーション数が爆発する。
void compareMemory(const std::vector<u8>& wantRam, const std::vector<std::uint16_t>& wantGen,
                   const char* what)
{
    INFO(std::string(what));

    std::size_t firstRamDiff = wantRam.size();
    for (std::size_t i = 0; i < wantRam.size(); ++i)
    {
        if (execRam()[i] != wantRam[i])
        {
            firstRamDiff = i;
            break;
        }
    }
    if (firstRamDiff != wantRam.size())
    {
        INFO("guest RAM differs at byte ", firstRamDiff,
             " want=", static_cast<unsigned>(wantRam[firstRamDiff]),
             " got=", static_cast<unsigned>(execRam()[firstRamDiff]));
        CHECK(firstRamDiff == wantRam.size());
    }

    std::size_t firstGenDiff = wantGen.size();
    for (std::size_t i = 0; i < wantGen.size(); ++i)
    {
        if (execGen()[i] != wantGen[i])
        {
            firstGenDiff = i;
            break;
        }
    }
    if (firstGenDiff != wantGen.size())
    {
        INFO("page generation differs at page ", firstGenDiff, " want=", wantGen[firstGenDiff],
             " got=", execGen()[firstGenDiff]);
        CHECK(firstGenDiff == wantGen.size());
    }
}

// 命令列と初期状態を 1 組ぶん検査する。
//
// genSeedPage / genSeedGen は「実行前にそのページの世代をこの値にしておく」。
// 飽和の境界 (0xFFFE / 0xFFFF) を作るのに使う。
void checkEquivalence(const std::vector<u16>& words, const M68kState& initial, const char* what,
                      const jit::EmitEnv& env, u32 genSeedPage = 0xFFFFFFFFu,
                      std::uint16_t genSeedGen = 0)
{
    INFO(std::string(what));

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));

    // 後ろの余白を、参照側が置くのと同じ NOP で埋める。出口の ir / irc が
    // 両者で一致していないと、比較そのものが意味を失う。
    u32 at = kEntry + static_cast<u32>(words.size()) * 2u;
    for (u32 i = 0; i < 8; ++i)
    {
        code.put16(at + i * 2, static_cast<u16>(0x4E71u));
    }

    const EmitResult e = emit(plan, code, env);
    REQUIRE(e.ok);

    // **生成側と参照側を、同じ初期状態から別々に走らせる。**
    //
    // Tier B までは「どちらもメモリを書かないので同じ配列を共有してよい」
    // でよかったが、書き形は execRam と execGen を書き換える。
    // 走らせる前に必ず初期状態へ戻し、生成側の結果を控えてから
    // 参照側を回して、最後に控え同士を比べる。
    resetExecRam(words);
    resetExecGen(genSeedPage, genSeedGen);

    const NativeOutcome native = runEmitted(e, initial);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);

    // 生成側が作ったメモリと世代を控える。**参照側が上書きする前に。**
    const std::vector<u8> nativeRam = execRam();
    const std::vector<std::uint16_t> nativeGen = execGen();

    // ガードが不成立で降りたなら、**実際に走った命令数**だけ参照側を回す。
    // 出口は「その命令の直前の命令境界」なので、k 命令ぶんの状態と一致する
    // はず (G7)。降りていなければ計画の全命令。
    const u32 refCount = native.guardExit ? native.ranOps : plan.count;

    // 参照側も同じ初期状態から。runReference が resetExecRam を呼ぶので、
    // 世代だけここで戻す。
    resetExecGen(genSeedPage, genSeedGen);
    u32 refCycles = 0;
    const M68kState want = runReference(words, initial, refCount, refCycles);

    // 参照側が作ったメモリ・世代と、控えた生成側を比べる。
    //
    // **execRam / execGen は今 (参照側の結果) なので、控えた生成側と
    // 入れ替えてから compareMemory を呼ぶ。** 引数の名前は「want」だが、
    // 実体は参照側の結果。
    const std::vector<u8> refRam = execRam();
    const std::vector<std::uint16_t> refGen = execGen();
    execRam() = nativeRam;
    execGen() = nativeGen;
    compareMemory(refRam, refGen, what);
    // 以降のテストが execRam を読むかもしれないので、参照側へ戻しておく。
    execRam() = refRam;
    execGen() = refGen;

    // **動的分岐も pc / ir / irc を書かない契約。** 静的分岐の成立側と
    // 同じ理由 (飛び先のプリフェッチはページの外を読みうる) なので、
    // その 3 つを比較から外し、代わりに飛び先を別に問う。
    const bool skipPrefetch = native.branchTaken || native.dynamicBranch;
    compareStates(want, native.state, skipPrefetch, what);

    // サイクル数。**ここがずれると rasterNumber の 317 サイクル粒度が
    // 数万命令後に必ず割れる。**
    INFO("cycles");
    CHECK(native.cycles == refCycles);

    if (native.guardExit)
    {
        // ガード脱出は分岐ではない (G9: bit31 と bit30 は同時に立たない)。
        // **動的分岐とも同時には立たない** (G21 の排他)。
        INFO("guard exit is not a branch");
        CHECK_FALSE(native.branchTaken);
        CHECK_FALSE(native.dynamicBranch);
        // 降りた地点は「まだ実行していない命令の手前」なので、
        // 計画の全命令を走り切ってはいない。
        CHECK(native.ranOps < plan.count);
        return;
    }

    const bool branchTaken = native.branchTaken;

    if (plan.end == BlockEnd::kDynamicBranch)
    {
        // **必ず飛ぶ。** RTS / JSR は条件を持たない。
        INFO("dynamic branch taken");
        CHECK(native.dynamicBranch);
        CHECK_FALSE(branchTaken);
        // **飛び先はメールボックスから来る。** 参照側の pc から逆算する。
        // インタプリタは refillPrefetch(target) を通るので pc == target + 4。
        //
        // ここが同値テストの本体。**飛び先を 1 bit でも間違えると落ちる。**
        INFO("dynamic branch target");
        CHECK(native.mailbox == want.pc - 4u);
        return;
    }

    if (plan.end == BlockEnd::kBranch)
    {
        // 分岐成立の判定が参照と一致すること。参照側の pc から逆算する。
        const bool refTaken = want.pc != plan.fallThroughPc + 4u;
        INFO("branch taken");
        CHECK(branchTaken == refTaken);
        if (branchTaken)
        {
            INFO("branch target");
            CHECK(e.info.branchTarget == plan.branchTarget);
            CHECK(want.pc == plan.branchTarget + 4u);
        }
    }
    else
    {
        CHECK_FALSE(branchTaken);
    }
    // 動的分岐で終端していない計画は、bit29 を決して立てない。
    CHECK_FALSE(native.dynamicBranch);
}

// 既定の窓で検査する。**大半のテストはこちらを呼ぶ。**
void checkEquivalence(const std::vector<u16>& words, const M68kState& initial, const char* what)
{
    checkEquivalence(words, initial, what, fakeEnv());
}

// 命令語の組み立て。
constexpr u16 moveq(u32 reg, int imm)
{
    return static_cast<u16>(0x7000u | (reg << 9) | (static_cast<u32>(imm) & 0xFFu));
}
// MOVE.<size> Ds,Dd
constexpr u16 moveReg(u32 sizeGroup, u32 dst, u32 src)
{
    return static_cast<u16>((sizeGroup << 12) | (dst << 9) | src);
}
// ALU <Ds>,Dd。group は $8/$9/$B/$C/$D、opmode は 0/1/2 (byte/word/long)。
constexpr u16 aluReg(u32 group, u32 dst, u32 opmode, u32 src)
{
    return static_cast<u16>((group << 12) | (dst << 9) | (opmode << 6) | src);
}
// MOVE.<size> As,Dd / MOVEA.<size> <src>,Ad
constexpr u16 moveSrcAn(u32 sizeGroup, u32 dst, u32 srcAn)
{
    return static_cast<u16>((sizeGroup << 12) | (dst << 9) | (1u << 3) | srcAn);
}
constexpr u16 moveaFromDn(u32 sizeGroup, u32 dstAn, u32 srcDn)
{
    return static_cast<u16>((sizeGroup << 12) | (dstAn << 9) | (1u << 6) | srcDn);
}
constexpr u16 moveaFromAn(u32 sizeGroup, u32 dstAn, u32 srcAn)
{
    return static_cast<u16>((sizeGroup << 12) | (dstAn << 9) | (1u << 6) | (1u << 3) | srcAn);
}
// MOVE.<size> #imm,Dd (拡張ワードは呼び出し側が words に続けて置く)
constexpr u16 moveImm(u32 sizeGroup, u32 dst)
{
    return static_cast<u16>((sizeGroup << 12) | (dst << 9) | (7u << 3) | 4u);
}
// TST.<size> Dn / CLR.<size> Dn。sizeField は 0/1/2 = b/w/l
constexpr u16 tstDn(u32 sizeField, u32 reg)
{
    return static_cast<u16>(0x4A00u | (sizeField << 6) | reg);
}
constexpr u16 clrDn(u32 sizeField, u32 reg)
{
    return static_cast<u16>(0x4200u | (sizeField << 6) | reg);
}
// LEA (An),Ad / (d16,An),Ad / (xxx).L,Ad
constexpr u16 leaInd(u32 dstAn, u32 srcAn)
{
    return static_cast<u16>(0x41D0u | (dstAn << 9) | srcAn);
}
constexpr u16 leaDisp(u32 dstAn, u32 srcAn)
{
    return static_cast<u16>(0x41E8u | (dstAn << 9) | srcAn);
}
constexpr u16 leaAbsL(u32 dstAn)
{
    return static_cast<u16>(0x41F9u | (dstAn << 9));
}

// --- Tier B: メモリ読み形の命令語 ------------------------------------------
//
// EA の符号化は共通で mode<<3 | reg。呼び出し側が「どの mode を」を
// 名前で選べるようにしておくと、テストが符号を組み間違えにくい。
constexpr u32 kModeInd = 2u;      // (An)
constexpr u32 kModePostInc = 3u;  // (An)+
constexpr u32 kModePreDec = 4u;   // -(An)
constexpr u32 kModeDisp = 5u;     // (d16,An)
constexpr u32 kModeAbsW = 7u;     // (xxx).W  (reg = 0)
constexpr u32 kModeAbsL = 7u;     // (xxx).L  (reg = 1)

// MOVE.<size> <ea>,Dd
constexpr u16 moveMemToDn(u32 sizeGroup, u32 dst, u32 mode, u32 reg)
{
    return static_cast<u16>((sizeGroup << 12) | (dst << 9) | (mode << 3) | reg);
}
// TST.<size> <ea>
constexpr u16 tstMem(u32 sizeField, u32 mode, u32 reg)
{
    return static_cast<u16>(0x4A00u | (sizeField << 6) | (mode << 3) | reg);
}
// ALU.<size> <ea>,Dd。group は $8/$9/$B/$C/$D、opmode は 0/1/2 = b/w/l
constexpr u16 aluMemToDn(u32 group, u32 dst, u32 opmode, u32 mode, u32 reg)
{
    return static_cast<u16>((group << 12) | (dst << 9) | (opmode << 6) | (mode << 3) | reg);
}

// --- Tier C: メモリ書き形の命令語 ------------------------------------------
//
// **転送先の符号化は転送元と並びが逆** (レジスタ番号が先、モードが後)。
// ここを揃えて組むと mode と reg が入れ替わり、(A3) のつもりで
// (xxx).W を書くような取り違えになる。
//
// MOVE.<size> Dn,<ea>
constexpr u16 moveDnToMem(u32 sizeGroup, u32 srcDn, u32 mode, u32 reg)
{
    return static_cast<u16>((sizeGroup << 12) | (reg << 9) | (mode << 6) | srcDn);
}
// CLR.<size> <ea>
constexpr u16 clrMem(u32 sizeField, u32 mode, u32 reg)
{
    return static_cast<u16>(0x4200u | (sizeField << 6) | (mode << 3) | reg);
}

// --- Tier D: 動的分岐の命令語 ----------------------------------------------

constexpr u16 kRtsOp = 0x4E75u;
// JSR <ea>。mode / reg は EA の符号化そのまま。
constexpr u16 jsr(u32 mode, u32 reg)
{
    return static_cast<u16>(0x4E80u | (mode << 3) | reg);
}

constexpr u16 bcc(u32 cond, int disp8)
{
    return static_cast<u16>(0x6000u | (cond << 8) | (static_cast<u32>(disp8) & 0xFFu));
}

// 生成コードを**命令の切れ目に沿って**歩く。
//
// Why not バイトごとに走査しないか: Xtensa は 2 バイト命令と 3 バイト命令が
// 混ざる可変長で、3 バイト命令の途中のバイトも「別の正当な命令」に見える。
// バイトごとに見ると、l32r のリテラル参照や movi の即値が s16i / s32i.n に
// 化けて**偽の検出**になる (実際に踏んだ)。
//
// 命令長は op0 (下位 4bit) で決まる。0x8-0xD が 2 バイトの短縮形で、
// 残りは 3 バイト。**発行器が使う命令の範囲でしか正しくない**が、
// ミニ解釈器が知らない命令を拒否するので、範囲外が混ざればそちらで落ちる。
std::size_t xtensaInsnLength(std::uint8_t first)
{
    const std::uint32_t op0 = first & 0x0Fu;
    const bool isNarrow = op0 >= 0x8u && op0 <= 0xDu;
    return isNarrow ? 2u : 3u;
}

M68kState makeState(u32 seed)
{
    M68kState s{};
    for (u32 i = 0; i < 8; ++i)
    {
        s.d[i] = seed * 0x9E3779B9u + i * 0x01010101u;
        s.a[i] = 0x00100000u + i * 0x40u;
    }
    s.a[7] = 0x00200000u;
    // スーパーバイザ + 割り込みマスク 7。**上位バイトが保存されることを問う。**
    s.sr = static_cast<u16>(0x2700u | (seed & 0x1Fu));
    s.usp = 0x00300000u;
    s.ssp = 0x00200000u;
    return s;
}

}  // namespace

TEST_CASE("同じ計画からは同じバイト列が出る")
{
    const std::vector<u16> words{moveq(3, -5), aluReg(0xD, 3, 2, 4), bcc(0x7, 4)};

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));

    const EmitResult a = emit(plan, code);
    const EmitResult b = emit(plan, code);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.buffer.size() == b.buffer.size());
    CHECK(a.buffer == b.buffer);
    CHECK(a.info.entryOffset == b.info.entryOffset);

    // requiredSize が実際に書いた量と一致すること。
    // **これがずれると、次のブロックの先頭を踏む。**
    CHECK(jit::requiredSize(plan, code.get16(plan.fallThroughPc),
                            code.get16(plan.fallThroughPc + 2), fakeEnv()) == a.info.totalSize);
    CHECK(a.info.totalSize == a.buffer.size());
}

TEST_CASE("出口で書き戻すレジスタが 1 つも漏れていない")
{
    // 8 本の Dn すべてに書く計画は kMaxOps = 4 では組めないので、
    // 「触った Dn は必ず s32i される」ことを 1 本ずつ確かめる。
    for (u32 reg = 0; reg < 8; ++reg)
    {
        const std::vector<u16> words{moveq(reg, 0x12)};
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        // d[reg] への s32i (offset = reg*4) が含まれること。
        // s32i at, a3, off は RRI8 op0=2 / r=6 / s=3 / imm8=off/4。
        std::uint8_t want[3];
        jit::s32i(want, /*at=*/4, /*as=*/3, jit::kStateDOffset + reg * 4u);
        // at (書く値のレジスタ) は発行器の都合で変わりうるので、
        // 「s と r と imm8 が一致する 3 バイト」を探す。
        bool found = false;
        for (std::size_t i = 0; i + 3 <= e.buffer.size(); ++i)
        {
            if ((e.buffer[i] & 0x0Fu) == (want[0] & 0x0Fu) && e.buffer[i + 1] == want[1] &&
                e.buffer[i + 2] == want[2])
            {
                found = true;
                break;
            }
        }
        INFO("d[", reg, "] への s32i");
        CHECK(found);
    }

    // pc / ir / irc も同じく。非分岐終端のブロックで問う。
    const std::vector<u16> words{moveq(0, 1), moveq(1, 2)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    const auto contains = [&e](const std::uint8_t* pat)
    {
        for (std::size_t i = 0; i + 3 <= e.buffer.size(); ++i)
        {
            if ((e.buffer[i] & 0x0Fu) == (pat[0] & 0x0Fu) && e.buffer[i + 1] == pat[1] &&
                e.buffer[i + 2] == pat[2])
            {
                return true;
            }
        }
        return false;
    };
    std::uint8_t pat[3];
    jit::s32i(pat, 4, 3, jit::kStatePcOffset);
    INFO("pc への s32i");
    CHECK(contains(pat));
    jit::s16i(pat, 4, 3, jit::kStateIrOffset);
    INFO("ir への s16i");
    CHECK(contains(pat));
    jit::s16i(pat, 4, 3, jit::kStateIrcOffset);
    INFO("irc への s16i");
    CHECK(contains(pat));
    jit::s16i(pat, 4, 3, jit::kStateSrOffset);
    INFO("sr への s16i");
    CHECK(contains(pat));
}

TEST_CASE("usp / ssp / stopped / halted を触る命令を発行しない")
{
    // 生成コードに、これらのオフセットへ書く s32i / s16i / s8i が
    // **一切含まれない**こと。契約 §5.1 を機械で守る。
    const std::vector<u16> words{moveq(0, -1), aluReg(0x9, 1, 2, 0), moveReg(0x2, 2, 1)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    for (std::size_t i = e.info.entryOffset; i + 3 <= e.buffer.size(); ++i)
    {
        const std::uint32_t w = static_cast<std::uint32_t>(e.buffer[i]) |
                                (static_cast<std::uint32_t>(e.buffer[i + 1]) << 8) |
                                (static_cast<std::uint32_t>(e.buffer[i + 2]) << 16);
        if ((w & 0xFu) != 0x2u)
        {
            continue;
        }
        const std::uint32_t r = (w >> 12) & 0xFu;
        const std::uint32_t imm8 = (w >> 16) & 0xFFu;
        if (r == 0x6u)  // s32i
        {
            const std::uint32_t off = imm8 * 4u;
            INFO("s32i offset ", off);
            CHECK(off != offsetof(M68kState, usp));
            CHECK(off != offsetof(M68kState, ssp));
        }
        if (r == 0x5u)  // s16i
        {
            const std::uint32_t off = imm8 * 2u;
            INFO("s16i offset ", off);
            CHECK(off < offsetof(M68kState, stopped));
        }
    }
}

TEST_CASE("分岐成立側の出口が pc / ir / irc を書かない")
{
    // 契約 §5.2 を機械で守る。
    //
    // **これが無いと、成立側の出口は検査の死角になる。** 同値テストは
    // 成立側で pc / ir / irc を比較から外している (飛び先のプリフェッチは
    // 呼び出し側の M68k::branchTo が詰め直す契約なので、生成コードの
    // 出口では正しい値が入っていない)。そのため生成コードが成立側で
    // これらに何を書いても、同値テストは緑のまま通ってしまう。
    //
    // 危ないのは「速度のために成立側でも pc を書けば branchTo が省ける」
    // という最適化を誰かが思いつくこと。pc だけなら正しいが、ir / irc を
    // 足した瞬間に壊れる。壊れ方は「I/O 空間の余分な読み出し」か
    // 「インタプリタでは一度も起きなかったバスエラー」で、エミュレータの
    // 正しさのテストではなく実機の挙動として出る。
    //
    // usp / ssp に対して既にやっているのと同じ、バイト列の走査で問う。
    const std::vector<u16> words{moveq(0, 1), bcc(0x6, 0x10)};  // MOVEQ; BNE
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    REQUIRE(plan.end == BlockEnd::kBranch);
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    // 成立側の出口は「分岐が成立したときに通る経路」なので、バイト列
    // 全体を走査して pc / ir / irc へのストアの**総数**を数える。
    // 不成立側では 3 つとも書くので、成立側でも書いていれば数が増える。
    std::size_t pcStores = 0;
    std::size_t irStores = 0;
    std::size_t ircStores = 0;
    for (std::size_t i = e.info.entryOffset; i + 3 <= e.buffer.size(); ++i)
    {
        const std::uint32_t w = static_cast<std::uint32_t>(e.buffer[i]) |
                                (static_cast<std::uint32_t>(e.buffer[i + 1]) << 8) |
                                (static_cast<std::uint32_t>(e.buffer[i + 2]) << 16);
        if ((w & 0xFu) != 0x2u)
        {
            continue;
        }
        const std::uint32_t r = (w >> 12) & 0xFu;
        const std::uint32_t imm8 = (w >> 16) & 0xFFu;
        if (r == 0x6u && imm8 * 4u == offsetof(M68kState, pc))
        {
            ++pcStores;
        }
        if (r == 0x5u)
        {
            const std::uint32_t off = imm8 * 2u;
            if (off == offsetof(M68kState, ir))
            {
                ++irStores;
            }
            if (off == offsetof(M68kState, irc))
            {
                ++ircStores;
            }
        }
    }

    // 不成立側の出口ぶんだけ。成立側でも書いていれば 2 になる。
    INFO("pc へのストア ", pcStores, " / ir ", irStores, " / irc ", ircStores);
    CHECK(pcStores == 1);
    CHECK(irStores == 1);
    CHECK(ircStores == 1);
}

// Tier A: メモリに触れず例外も起きない形が、インタプリタと一致する。
//
// **フラグの扱いが命令ごとに違うのが要点。**
//   MOVE / TST : N/Z を立て V/C クリア、X 保存
//   CLR        : Z=1、N/V/C=0、X 保存
//   MOVEA / LEA: **1 つも変えない**
// checkEquivalence は sr を丸ごと比べるので、余計に触れば必ず落ちる。
TEST_CASE("MOVE An,Dn がインタプリタと一致する")
{
    // .w は上位が捨てられ、.l は全体が入る。bit15 が立つ値を必ず通す。
    for (u32 group : {0x2u, 0x3u})
    {
        for (u32 seed : {1u, 7u, 19u})
        {
            M68kState s = makeState(seed);
            s.a[1] = 0xFFFF8000u;  // 符号ビットが立つ
            s.a[2] = 0x00007FFFu;
            for (u32 src : {1u, 2u})
            {
                checkEquivalence({moveSrcAn(group, 3, src)}, s, "MOVE An,Dn");
            }
        }
    }
}

TEST_CASE("MOVE #imm,Dn がインタプリタと一致する")
{
    struct Case
    {
        u32 group;
        std::vector<u16> ext;
    };
    const Case cases[] = {
        {0x3u, {0x0000}},
        {0x3u, {0x8000}},
        {0x3u, {0x7FFF}},
        {0x2u, {0x1234, 0x5678}},
        {0x2u, {0x8000, 0x0000}},
        {0x1u, {0x0042}},
        {0x1u, {0x0080}},
        // **上位バイトが 0 でない byte 即値。** 68000 の .b 即値は拡張ワードの
        // 下位バイトだけを使い、上位は無視される (readEaSlow / m68k.cpp)。
        // foldImmediate の `& 0xFF` を外すと、emitMoveImmToDreg が畳む
        // N / Z がその上位バイトを拾って壊れる。
        //
        // **上位バイトが 0 のケースだけでは、この変異が捕まらない**
        // (実際に注入して確かめた)。0x0042 / 0x0080 しか通していなかった。
        {0x1u, {0xFF7F}},
        {0x1u, {0xAB00}},
        {0x1u, {0x7F80}},
    };
    for (const Case& c : cases)
    {
        for (u32 seed : {2u, 11u})
        {
            std::vector<u16> words{moveImm(c.group, 4)};
            words.insert(words.end(), c.ext.begin(), c.ext.end());
            checkEquivalence(words, makeState(seed), "MOVE #imm,Dn");
        }
    }
}

TEST_CASE("MOVEA がインタプリタと一致する")
{
    // **フラグを 1 つも変えない**ことを問う。makeState は sr の下位 5bit に
    // seed を入れるので、余計に触れば sr の比較で落ちる。
    for (u32 group : {0x2u, 0x3u})
    {
        for (u32 seed : {3u, 13u, 29u})
        {
            M68kState s = makeState(seed);
            s.d[1] = 0xFFFF8000u;  // .w の符号拡張を問う
            s.d[2] = 0x00007FFFu;
            s.a[3] = 0xFFFF8000u;
            checkEquivalence({moveaFromDn(group, 5, 1)}, s, "MOVEA Dn,An");
            checkEquivalence({moveaFromDn(group, 5, 2)}, s, "MOVEA Dn,An");
            checkEquivalence({moveaFromAn(group, 5, 3)}, s, "MOVEA An,An");
        }
    }
}

TEST_CASE("TST / CLR がインタプリタと一致する")
{
    for (u32 sizeField : {0u, 1u, 2u})
    {
        for (u32 seed : {4u, 17u, 31u})
        {
            M68kState s = makeState(seed);
            s.d[0] = 0u;           // Z が立つ
            s.d[1] = 0x80000000u;  // .l の N が立つ
            s.d[2] = 0x0000FF00u;  // .b は 0、.w は非 0
            for (u32 reg : {0u, 1u, 2u})
            {
                checkEquivalence({tstDn(sizeField, reg)}, s, "TST Dn");
                // CLR は上位バイトの保存も問う (.b/.w)
                checkEquivalence({clrDn(sizeField, reg)}, s, "CLR Dn");
            }
        }
    }
}

TEST_CASE("LEA がインタプリタと一致する")
{
    // **フラグを変えない。** アドレスを求めるだけで読まない。
    for (u32 seed : {5u, 23u})
    {
        M68kState s = makeState(seed);
        checkEquivalence({leaInd(4, 1)}, s, "LEA (An),An");
        // 変位は正負の両方。0x8000 は負変位になる。
        checkEquivalence({leaDisp(4, 1), 0x0010}, s, "LEA (d16,An),An");
        checkEquivalence({leaDisp(4, 1), 0x8000}, s, "LEA (負変位,An),An");
        checkEquivalence({leaDisp(4, 1), 0x7FFF}, s, "LEA (最大変位,An),An");
        checkEquivalence({leaAbsL(4), 0x0012, 0x3456}, s, "LEA (xxx).L,An");
    }
}

TEST_CASE("MOVEQ がインタプリタと一致する")
{
    for (int imm : {0, 1, -1, 127, -128, 0x40, -0x40})
    {
        for (u32 reg : {0u, 3u, 7u})
        {
            checkEquivalence({moveq(reg, imm)}, makeState(static_cast<u32>(imm) + reg), "MOVEQ");
        }
    }
}

TEST_CASE("MOVE Dn,Dm がインタプリタと一致する")
{
    // sizeGroup: $1 = byte / $2 = long / $3 = word。**$2 が long。**
    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        for (u32 src : {0u, 5u})
        {
            for (u32 dst : {1u, 5u})
            {
                for (u32 seed : {1u, 2u, 3u, 17u})
                {
                    M68kState s = makeState(seed);
                    // 境界値を仕込む。0 と符号ビットだけが立つ値。
                    s.d[src] = seed == 1 ? 0u : (seed == 2 ? 0x80808080u : 0x0000FF00u);
                    checkEquivalence({moveReg(group, dst, src)}, s, "MOVE Dn,Dm");
                }
            }
        }
    }
}

TEST_CASE("ADD / SUB / CMP / AND / OR がインタプリタと一致する")
{
    // group: $8 = OR / $9 = SUB / $B = CMP / $C = AND / $D = ADD
    static constexpr u32 kGroups[] = {0x8u, 0x9u, 0xBu, 0xCu, 0xDu};
    // 桁溢れと符号の境界を必ず含む。V と C はここでしか壊れない。
    static constexpr u32 kValues[] = {
        0x00000000u, 0x00000001u, 0x0000007Fu, 0x00000080u, 0x000000FFu, 0x00007FFFu, 0x00008000u,
        0x0000FFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u, 0x89ABCDEFu,
    };

    for (u32 group : kGroups)
    {
        for (u32 opmode : {0u, 1u, 2u})  // byte / word / long
        {
            for (u32 dv : kValues)
            {
                for (u32 sv : kValues)
                {
                    M68kState s = makeState(1);
                    s.d[2] = dv;
                    s.d[4] = sv;
                    // X が保存される (CMP / AND / OR) / 上書きされる (ADD / SUB)
                    // 両方を見るため、入口で X を立てておく。
                    s.sr = static_cast<u16>(0x2700u | 0x10u);
                    checkEquivalence({aluReg(group, 2, opmode, 4)}, s, "ALU Dn,Dm");
                }
            }
        }
    }
}

TEST_CASE("同じレジスタどうしの ALU も一致する")
{
    // d と s が同じレジスタだと、切り出しの順序を間違えたときにだけ落ちる。
    for (u32 group : {0x8u, 0x9u, 0xBu, 0xCu, 0xDu})
    {
        for (u32 opmode : {0u, 1u, 2u})
        {
            for (u32 v : {0x00000000u, 0x80000000u, 0xFFFFFFFFu, 0x00008000u})
            {
                M68kState s = makeState(5);
                s.d[3] = v;
                checkEquivalence({aluReg(group, 3, opmode, 3)}, s, "ALU Dn,Dn");
            }
        }
    }
}

TEST_CASE("Bcc の 16 通りの条件がインタプリタと一致する")
{
    // cond 1 (BSR) は段 1 が積まないので除く。cond 0 は BRA。
    for (u32 cond = 0; cond < 16; ++cond)
    {
        if (cond == 1)
        {
            continue;
        }
        // CCR の 16 通りを全部試す。**条件式の取り違えはここでしか出ない。**
        for (u32 ccr = 0; ccr < 32; ++ccr)
        {
            M68kState s = makeState(9);
            s.sr = static_cast<u16>(0x2700u | ccr);
            checkEquivalence({bcc(cond, 6)}, s, "Bcc");
        }
    }
}

TEST_CASE("Bcc.w (16bit 変位) も一致する")
{
    for (u32 cond : {0u, 6u, 7u, 0xCu})
    {
        for (int disp : {6, 8, 20})
        {
            for (u32 ccr : {0u, 4u, 0xFu, 0x1Fu})
            {
                M68kState s = makeState(11);
                s.sr = static_cast<u16>(0x2700u | ccr);
                // disp8 == 0 が 16bit 変位形。拡張ワードに変位を置く。
                checkEquivalence({bcc(cond, 0), static_cast<u16>(disp)}, s, "Bcc.w");
            }
        }
    }
}

TEST_CASE("複数命令のブロックが一致する")
{
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{moveq(0, 5), moveq(1, -3)}, "MOVEQ x2"},
        {{moveq(0, 5), aluReg(0xD, 1, 2, 0)}, "MOVEQ + ADD.l"},
        {{moveq(0, 0), aluReg(0xB, 1, 0, 0), bcc(0x7, 4)}, "MOVEQ + CMP.b + BEQ"},
        {{moveReg(0x2, 1, 0), moveReg(0x3, 2, 1), moveReg(0x1, 3, 2)}, "MOVE.l/.w/.b"},
        {{aluReg(0xC, 0, 2, 1), aluReg(0x8, 2, 1, 3), aluReg(0x9, 4, 0, 5)}, "AND/OR/SUB"},
        // kMaxOps = 4 ちょうど。
        {{moveq(0, 1), moveq(1, 2), moveq(2, 3), moveq(3, 4)}, "MOVEQ x4 (kCapacity)"},
        {{moveq(0, -1), aluReg(0xD, 0, 0, 1), aluReg(0x9, 0, 1, 2), bcc(0xD, 8)}, "3 命令 + BLT"},
    };

    for (const Case& c : cases)
    {
        for (u32 seed : {1u, 4u, 13u})
        {
            checkEquivalence(c.words, makeState(seed), c.what);
        }
    }
}

TEST_CASE("SR の上位バイトを壊さない")
{
    // 割り込みマスク・S・T をいろいろ変えて、CCR 以外が 1 ビットも
    // 動かないことを見る。**上位バイトを落とすと特権が外れて暴走する。**
    for (u16 upper : {u16(0x0000u), u16(0x2700u), u16(0xA700u), u16(0x8000u), u16(0x2000u)})
    {
        M68kState s = makeState(3);
        s.sr = static_cast<u16>(upper | 0x1Fu);
        checkEquivalence({aluReg(0xD, 2, 2, 4)}, s, "ADD.l と SR 上位");
        s.sr = static_cast<u16>(upper);
        checkEquivalence({moveq(0, 0)}, s, "MOVEQ と SR 上位");
    }
}

// --- Tier B: 読みガード -----------------------------------------------------
//
// 以下は「ガードが**成立する**」場合の同値性。窓の中を指すアドレスを与え、
// 生成コードとインタプリタが同じ値を読んで同じ状態を作ることを問う。
// 不成立側 (脱出) は別の TEST_CASE で扱う。

namespace
{

// 読み形のテストで使うゲストアドレス。**偶数**にしておく
// (奇数はガードが弾くので、成立側のテストには使えない)。
constexpr u32 kDataAddr = 0x00040000u;

// そこへ 4 バイト置く種を仕込む。ビッグエンディアンの組み立てを問うので、
// **4 バイトとも違う値**にする (どれか 1 つでも入れ替わったら分かる)。
void seedData(u32 addr, u8 b0, u8 b1, u8 b2, u8 b3)
{
    guestSeeds().clear();
    guestSeeds().push_back(GuestSeed{addr, {b0, b1, b2, b3}});
}

void clearSeeds()
{
    guestSeeds().clear();
}

}  // namespace

TEST_CASE("MOVE <mem>,Dn がインタプリタと一致する")
{
    // sizeGroup: $1 = byte / $2 = long / $3 = word。**$2 が long。**
    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        for (u32 seed : {1u, 2u, 5u})
        {
            // (An) — 最頻の形。
            {
                seedData(kDataAddr, 0x12, 0x34, 0x56, 0x78);
                M68kState s = makeState(seed);
                s.a[2] = kDataAddr;
                checkEquivalence({moveMemToDn(group, 1, kModeInd, 2)}, s, "MOVE (An),Dn");
            }
            // 符号ビットが立つ値。N フラグと、byte/word の切り出しを問う。
            {
                seedData(kDataAddr, 0x80, 0x00, 0x00, 0x01);
                M68kState s = makeState(seed);
                s.a[2] = kDataAddr;
                checkEquivalence({moveMemToDn(group, 3, kModeInd, 2)}, s, "MOVE (An),Dn 負値");
            }
            // 全部 0。Z フラグ。
            {
                seedData(kDataAddr, 0x00, 0x00, 0x00, 0x00);
                M68kState s = makeState(seed);
                s.a[2] = kDataAddr;
                checkEquivalence({moveMemToDn(group, 0, kModeInd, 2)}, s, "MOVE (An),Dn ゼロ");
            }
            // (An)+ — An が進むこと。
            {
                seedData(kDataAddr, 0xDE, 0xAD, 0xBE, 0xEF);
                M68kState s = makeState(seed);
                s.a[4] = kDataAddr;
                checkEquivalence({moveMemToDn(group, 2, kModePostInc, 4)}, s, "MOVE (An)+,Dn");
            }
            // -(An) — 先に引いてから読むこと。
            {
                seedData(kDataAddr, 0xDE, 0xAD, 0xBE, 0xEF);
                M68kState s = makeState(seed);
                // 引いた結果が kDataAddr になるように置く。
                s.a[5] = kDataAddr + (group == 0x1u ? 1u : (group == 0x2u ? 4u : 2u));
                checkEquivalence({moveMemToDn(group, 6, kModePreDec, 5)}, s, "MOVE -(An),Dn");
            }
            // (d16,An) — 正負の変位。
            {
                seedData(kDataAddr, 0x01, 0x02, 0x03, 0x04);
                M68kState s = makeState(seed);
                s.a[3] = kDataAddr - 0x10u;
                checkEquivalence({moveMemToDn(group, 1, kModeDisp, 3), 0x0010u}, s,
                                 "MOVE (d16,An),Dn 正変位");
                s.a[3] = kDataAddr + 0x10u;
                checkEquivalence({moveMemToDn(group, 1, kModeDisp, 3), 0xFFF0u}, s,
                                 "MOVE (d16,An),Dn 負変位");
            }
            // (xxx).L — 絶対アドレス。**ガードは翻訳時に消えている。**
            {
                seedData(kDataAddr, 0x11, 0x22, 0x33, 0x44);
                M68kState s = makeState(seed);
                checkEquivalence(
                    {moveMemToDn(group, 7, kModeAbsL, 1), static_cast<u16>(kDataAddr >> 16),
                     static_cast<u16>(kDataAddr & 0xFFFFu)},
                    s, "MOVE (xxx).L,Dn");
            }
            // (xxx).W — 符号拡張される。小さい正の番地を使う。
            {
                seedData(0x00000400u, 0xAA, 0xBB, 0xCC, 0xDD);
                M68kState s = makeState(seed);
                checkEquivalence({moveMemToDn(group, 4, kModeAbsW, 0), 0x0400u}, s,
                                 "MOVE (xxx).W,Dn");
            }
        }
    }
    clearSeeds();
}

TEST_CASE("A7 をバイトで触る (An)+ / -(An) は 2 進む")
{
    // **バイトでも A7 だけは 2 増減する** (スタックポインタが奇数に
    // ならないための特例)。1 進む実装だと、この 2 つで a[7] が食い違う。
    seedData(kDataAddr, 0x5A, 0xA5, 0x3C, 0xC3);

    M68kState post = makeState(3);
    post.a[7] = kDataAddr;
    checkEquivalence({moveMemToDn(0x1u, 1, kModePostInc, 7)}, post, "MOVE.b (A7)+,Dn");

    M68kState pre = makeState(3);
    pre.a[7] = kDataAddr + 2u;
    checkEquivalence({moveMemToDn(0x1u, 1, kModePreDec, 7)}, pre, "MOVE.b -(A7),Dn");

    clearSeeds();
}

TEST_CASE("TST <mem> がインタプリタと一致する")
{
    for (u32 sizeField : {0u, 1u, 2u})
    {
        // 負・ゼロ・正の 3 通りでフラグを問う。
        static constexpr u8 kPatterns[3][4] = {
            {0x80, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x7F}};
        for (const auto& pat : kPatterns)
        {
            seedData(kDataAddr, pat[0], pat[1], pat[2], pat[3]);
            M68kState s = makeState(4);
            s.a[2] = kDataAddr;
            // **d[] を 1 つも書かないこと**が TST の要点。
            checkEquivalence({tstMem(sizeField, kModeInd, 2)}, s, "TST (An)");

            s.a[3] = kDataAddr;
            checkEquivalence({tstMem(sizeField, kModePostInc, 3)}, s, "TST (An)+");
        }
    }
    clearSeeds();
}

TEST_CASE("ALU <mem>,Dn がインタプリタと一致する")
{
    // group: $8 = OR / $9 = SUB / $B = CMP / $C = AND / $D = ADD
    static constexpr u32 kGroups[] = {0x8u, 0x9u, 0xBu, 0xCu, 0xDu};
    for (u32 group : kGroups)
    {
        for (u32 opmode : {0u, 1u, 2u})
        {
            for (u32 seed : {1u, 3u, 9u})
            {
                // 桁上がり / 桁借り / 溢れが出る値を混ぜる。
                static constexpr u8 kPatterns[3][4] = {
                    {0xFF, 0xFF, 0xFF, 0xFF}, {0x80, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x01}};
                for (const auto& pat : kPatterns)
                {
                    seedData(kDataAddr, pat[0], pat[1], pat[2], pat[3]);
                    M68kState s = makeState(seed);
                    s.a[2] = kDataAddr;
                    checkEquivalence({aluMemToDn(group, 1, opmode, kModeInd, 2)}, s, "ALU (An),Dn");
                }
            }
        }
    }
    clearSeeds();
}

// --- Tier G: 非終端の高頻度形 ------------------------------------------------
//
// 3 系統とも「隣の命令とフラグの扱いが正反対」なのが要点。
//   CMP #imm / CMPA : フラグを**全部**変える (X 以外)
//   MOVEA <mem>     : フラグを **1 bit も**変えない
//   ADDA / SUBA     : フラグを **1 bit も**変えない
// checkEquivalence は sr を丸ごと比べるので、どちらへ間違えても落ちる。

// ALU.<size> #imm,Dd (拡張ワードは呼び出し側が words に続けて置く)
constexpr u16 aluImmToDn(u32 group, u32 dst, u32 opmode)
{
    return static_cast<u16>((group << 12) | (dst << 9) | (opmode << 6) | (7u << 3) | 4u);
}
// MOVEA.<size> <ea>,Ad。sizeGroup は $2 = long / $3 = word。
constexpr u16 moveaMemToAn(u32 sizeGroup, u32 dstAn, u32 mode, u32 reg)
{
    return static_cast<u16>((sizeGroup << 12) | (dstAn << 9) | (1u << 6) | (mode << 3) | reg);
}
// ADDA / SUBA / CMPA <src>,Ad。group は $D / $9 / $B、opmode は 3 (.w) / 7 (.l)。
// srcMode は 0 (Dn) か 1 (An)。
constexpr u16 aluaReg(u32 group, u32 dstAn, u32 opmode, u32 srcMode, u32 srcReg)
{
    return static_cast<u16>((group << 12) | (dstAn << 9) | (opmode << 6) | (srcMode << 3) | srcReg);
}

// CMP.w #imm,Dn — 実測で単独 7.3% を落としていた最軽量の的。
//
// **フラグを全部変える** (N/Z/V/C。X は保存)。V と C は桁溢れの境界でしか
// 壊れないので、即値と Dn の両方に符号の境界を通す。
//
// 落ちる変異:
//   - emitAluImm が emitConst を落とす (kTmpA が前の命令の値のまま)
//   - foldImmediate の byte マスクを外す (上位バイトが混ざる)
//   - planAlu の即値経路で size を取り違える (.w と .l が入れ替わる)
//   - CMP でフラグを書かない (emitStoreCcr を消す)
TEST_CASE("ALU #imm,Dn がインタプリタと一致する")
{
    // group: $8 = OR / $9 = SUB / $B = CMP / $C = AND / $D = ADD
    static constexpr u32 kGroups[] = {0x8u, 0x9u, 0xBu, 0xCu, 0xDu};
    // 桁溢れと符号の境界。V と C はここでしか壊れない。
    static constexpr u32 kDstValues[] = {
        0x00000000u, 0x00000001u, 0x0000007Fu, 0x00000080u, 0x00007FFFu,
        0x00008000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u,
    };

    for (u32 group : kGroups)
    {
        for (u32 dv : kDstValues)
        {
            M68kState s = makeState(1);
            s.d[2] = dv;
            // X が保存される (CMP / AND / OR) / 上書きされる (ADD / SUB)
            // 両方を見るため、入口で X を立てておく。
            s.sr = static_cast<u16>(0x2700u | 0x10u);

            // .b : 拡張ワード 1 語。**上位バイトは捨てられる** (readEaSlow)。
            checkEquivalence({aluImmToDn(group, 2, 0u), 0xFF7Fu}, s, "ALU.b #imm,Dn");
            checkEquivalence({aluImmToDn(group, 2, 0u), 0x0080u}, s, "ALU.b #imm,Dn 符号");
            // .w : 拡張ワード 1 語。
            checkEquivalence({aluImmToDn(group, 2, 1u), 0x8000u}, s, "ALU.w #imm,Dn");
            checkEquivalence({aluImmToDn(group, 2, 1u), 0x7FFFu}, s, "ALU.w #imm,Dn 正の端");
            checkEquivalence({aluImmToDn(group, 2, 1u), 0x0000u}, s, "ALU.w #imm,Dn ゼロ");
            // .l : 拡張ワード 2 語。**上位が落ちると必ず落ちる。**
            checkEquivalence({aluImmToDn(group, 2, 2u), 0x8000u, 0x0000u}, s, "ALU.l #imm,Dn");
            checkEquivalence({aluImmToDn(group, 2, 2u), 0x1234u, 0x5678u}, s, "ALU.l #imm,Dn 任意");
        }
    }
}

// MOVEA.l <mem>,An — 実測で (d16,An) 5.5% / (An) 5.4% / (xxx).L 3.6%。
//
// **フラグを 1 bit も変えない** ← MOVE との決定的な違い。
// **.w は符号拡張して 32bit 全体を書く** ← 読んだ 16bit をそのまま
// 書くと上位が 0 になる。
//
// 落ちる変異:
//   - emitMemoryRead の kMoveaMemToAreg で emitLogicFlags を呼ぶ (sr が動く)
//   - emitMemoryRead の kMoveaMemToAreg で emitSext16 を落とす (.w の上位)
//   - 転送先を aOffset ではなく dOffset にする (MOVE との取り違え)
//   - planMove が kMoveaMemToAreg ではなく kMoveMemToDreg を積む
TEST_CASE("MOVEA <mem>,An がインタプリタと一致する")
{
    // sizeGroup: $2 = long / $3 = word。**$1 (byte) は 68000 に無い。**
    for (u32 group : {0x2u, 0x3u})
    {
        for (u32 seed : {1u, 7u, 21u})
        {
            // 符号拡張を問う値。上位バイトが 0x80 なら .w で負になる。
            seedData(kDataAddr, 0x80, 0x00, 0x12, 0x34);
            M68kState s = makeState(seed);
            s.a[2] = kDataAddr;
            checkEquivalence({moveaMemToAn(group, 5, kModeInd, 2)}, s, "MOVEA (An),An");

            seedData(kDataAddr, 0x7F, 0xFF, 0xFF, 0xFFu);
            s = makeState(seed);
            s.a[2] = kDataAddr;
            checkEquivalence({moveaMemToAn(group, 5, kModeInd, 2)}, s, "MOVEA (An),An 正の端");

            seedData(kDataAddr, 0x00, 0x00, 0x00, 0x00);
            s = makeState(seed);
            s.a[2] = kDataAddr;
            // **ゼロでも Z が立たないこと。** MOVE と取り違えたらここで落ちる。
            checkEquivalence({moveaMemToAn(group, 5, kModeInd, 2)}, s, "MOVEA (An),An ゼロ");

            // (An)+ / -(An) は An を動かす。commit の順序も同時に問う。
            seedData(kDataAddr, 0xDE, 0xAD, 0xBE, 0xEF);
            s = makeState(seed);
            s.a[3] = kDataAddr;
            checkEquivalence({moveaMemToAn(group, 5, kModePostInc, 3)}, s, "MOVEA (An)+,An");
            s = makeState(seed);
            s.a[4] = kDataAddr + 4u;
            checkEquivalence({moveaMemToAn(group, 5, kModePreDec, 4)}, s, "MOVEA -(An),An");

            // (d16,An) は正負の変位。
            s = makeState(seed);
            s.a[3] = kDataAddr - 0x10u;
            checkEquivalence({moveaMemToAn(group, 5, kModeDisp, 3), 0x0010u}, s,
                             "MOVEA (d16,An),An");
            s = makeState(seed);
            s.a[3] = kDataAddr + 0x10u;
            checkEquivalence({moveaMemToAn(group, 5, kModeDisp, 3), 0xFFF0u}, s,
                             "MOVEA (負変位,An),An");

            // (xxx).L / (xxx).W は実効アドレスが翻訳時定数。ガードが畳まれる。
            s = makeState(seed);
            checkEquivalence({moveaMemToAn(group, 5, kModeAbsL, 1),
                              static_cast<u16>(kDataAddr >> 16), static_cast<u16>(kDataAddr)},
                             s, "MOVEA (xxx).L,An");
        }
    }
    clearSeeds();
}

// **転送先の An が EA の An と同じとき**、commit と本体の順序で結果が割れる。
//
// MOVEA.l (A3)+,A3 は「読んで A3 を進めてから A3 へ読んだ値を書く」ので、
// 最後に残るのは読んだ値。commit を本体の後に置くと進んだ値が残る。
//
// 落ちる変異: emitMemoryRead で commit を本体の後ろへ動かす
TEST_CASE("MOVEA の転送先が EA と同じ An でもインタプリタと一致する")
{
    for (u32 group : {0x2u, 0x3u})
    {
        seedData(kDataAddr, 0x00, 0x11, 0x22, 0x33);
        M68kState s = makeState(3);
        s.a[3] = kDataAddr;
        checkEquivalence({moveaMemToAn(group, 3, kModePostInc, 3)}, s, "MOVEA (A3)+,A3");
        s = makeState(3);
        s.a[3] = kDataAddr + 4u;
        checkEquivalence({moveaMemToAn(group, 3, kModePreDec, 3)}, s, "MOVEA -(A3),A3");
        s = makeState(3);
        s.a[3] = kDataAddr;
        checkEquivalence({moveaMemToAn(group, 3, kModeInd, 3)}, s, "MOVEA (A3),A3");
    }
    clearSeeds();
}

// ADDA / SUBA — 実測で ADDA.l Dn,An 3.7% / ADDA.l An,An 3.6%。
//
// **フラグを 1 bit も変えない** ← ADD / SUB との決定的な違い。
// **.w は符号拡張してから 32bit で足す** ← サイズで結果を切ると上位が消える。
//
// 落ちる変異:
//   - emitAdda が emitStoreCcr を呼ぶ (フラグが動く)
//   - emitAdda が emitSext16 を落とす (.w が上位を拾う / 落とす)
//   - emitAdda が結果を emitTruncate する (.w で An の上位 16bit が消える)
//   - planAlu が kAdd と kSub を取り違える ($9 と $D)
//   - サイクルを 4 にする (実測 8)
TEST_CASE("ADDA / SUBA がインタプリタと一致する")
{
    // opmode: 3 = .w / 7 = .l。group: $D = ADDA / $9 = SUBA。
    static constexpr u32 kValues[] = {
        0x00000000u, 0x00000001u, 0x00007FFFu, 0x00008000u, 0x0000FFFFu,
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x00123456u,
    };
    for (u32 group : {0x9u, 0xDu})
    {
        for (u32 opmode : {3u, 7u})
        {
            for (u32 sv : kValues)
            {
                for (u32 dv : {0x00000000u, 0x0000FFFFu, 0x7FFFFFFFu, 0x80000000u})
                {
                    M68kState s = makeState(1);
                    s.d[4] = sv;
                    s.a[4] = sv;
                    s.a[5] = dv;
                    // X が保存されることを見るため、入口で立てておく。
                    s.sr = static_cast<u16>(0x2700u | 0x1Fu);
                    checkEquivalence({aluaReg(group, 5, opmode, 0u, 4u)}, s, "ADDA/SUBA Dn,An");
                    checkEquivalence({aluaReg(group, 5, opmode, 1u, 4u)}, s, "ADDA/SUBA An,An");
                }
            }
        }
    }
}

// **転送元と転送先が同じ An** のとき、読む順序で結果が割れる。
// ADDA.l A5,A5 は A5 = A5 + A5。
TEST_CASE("同じ An どうしの ADDA / SUBA も一致する")
{
    for (u32 group : {0x9u, 0xDu})
    {
        for (u32 opmode : {3u, 7u})
        {
            for (u32 v : {0x00008000u, 0xFFFF8000u, 0x12345678u})
            {
                M68kState s = makeState(2);
                s.a[5] = v;
                checkEquivalence({aluaReg(group, 5, opmode, 1u, 5u)}, s, "ADDA/SUBA A5,A5");
            }
        }
    }
}

// CMPA — ADDA / SUBA の隣にいて、**フラグの扱いが正反対**。
//
// **比較は必ず 32bit** (m68k_ops_alu.cpp:281)。.w でも src を符号拡張して
// から引くので、size 欄でフラグ幅を決めると .w が壊れる。
//
// 落ちる変異:
//   - emitCmpa が emitArithFlagsCore に size = 4 ではなく op.size を渡す
//   - emitCmpa が emitStoreCcr を呼ばない (フラグが動かない)
//   - emitCmpa が X を書く (CMP は X を保存する)
//   - emitCmpa が a[dstReg] へ結果を書き戻す (CMP は書かない)
//   - サイクルを 4 や 8 にする (実測 6)
TEST_CASE("CMPA がインタプリタと一致する")
{
    static constexpr u32 kValues[] = {
        0x00000000u, 0x00000001u, 0x00007FFFu, 0x00008000u, 0x0000FFFFu,
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x00123456u, 0xFFFF8000u,
    };
    for (u32 opmode : {3u, 7u})
    {
        for (u32 sv : kValues)
        {
            for (u32 dv : kValues)
            {
                M68kState s = makeState(1);
                s.d[4] = sv;
                s.a[4] = sv;
                s.a[5] = dv;
                // **X が保存されること**を見るため、入口で立てておく。
                s.sr = static_cast<u16>(0x2700u | 0x10u);
                checkEquivalence({aluaReg(0xBu, 5, opmode, 0u, 4u)}, s, "CMPA Dn,An");
                checkEquivalence({aluaReg(0xBu, 5, opmode, 1u, 4u)}, s, "CMPA An,An");
            }
        }
    }
}

TEST_CASE("同じ An どうしの CMPA も一致する")
{
    // A5 と A5 の比較は必ず Z が立ち、V / C は倒れる。
    for (u32 opmode : {3u, 7u})
    {
        for (u32 v : {0x00000000u, 0x00008000u, 0xFFFF8000u, 0x80000000u})
        {
            M68kState s = makeState(2);
            s.a[5] = v;
            checkEquivalence({aluaReg(0xBu, 5, opmode, 1u, 5u)}, s, "CMPA A5,A5");
        }
    }
}

// **Tier G はどれもブロックを終端しない。** 段 0-F (BSR) は被覆率を
// 伸ばしたのにブロックを短くして速度が付いてこなかった。ここに入れた形が
// 終端側に回ったら、その効果を丸ごと失う。
//
// 落ちる変異: isBlockTerminator へ Tier G のどれかを足す
TEST_CASE("Tier G の命令はブロックを終端しない")
{
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const Case cases[] = {
        {{aluImmToDn(0xBu, 2, 1u), 0x1234u}, "CMP.w #imm,Dn"},
        {{moveaMemToAn(0x2u, 5, kModeInd, 2)}, "MOVEA.l (An),An"},
        {{moveaMemToAn(0x2u, 5, kModeDisp, 2), 0x0010u}, "MOVEA.l (d16,An),An"},
        {{aluaReg(0xDu, 5, 7u, 0u, 4u)}, "ADDA.l Dn,An"},
        {{aluaReg(0xDu, 5, 7u, 1u, 4u)}, "ADDA.l An,An"},
        {{aluaReg(0xBu, 5, 7u, 0u, 4u)}, "CMPA.l Dn,An"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        // 後ろへ MOVEQ を積む。終端していれば count == 1 で止まる。
        std::vector<u16> words = c.words;
        words.push_back(moveq(1, 7));

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        // **2 命令入ること**が主張。1 なら終端側に回っている。
        CHECK(plan.count == 2);
        CHECK(plan.end != BlockEnd::kBranch);
        CHECK(plan.end != BlockEnd::kDynamicBranch);
        // **述語そのものも名指しで固定する。** 上の count == 2 だけでは
        // 足りない: plan() が isBlockTerminator を見るのは
        // 「静的な飛び先を持たず、かつメールボックスが未配線」のときだけ
        // なので、既定の caps (dynamicBranchAllowed = true) では
        // 述語を書き換えても count は 2 のまま通る。**実際に変異を注入して
        // 確かめた** — 述語へ Tier G を足しても 745 件が全部通った。
        // 述語が真になると、メールボックス未配線の実機で Tier G が
        // 丸ごと積まれなくなる (被覆率にしか現れない)。
        for (std::uint32_t i = 0; i < plan.count; ++i)
        {
            CHECK_FALSE(x68k::isBlockTerminator(plan.ops[i].kind));
            CHECK_FALSE(x68k::hasStaticBranchTarget(plan.ops[i].kind));
        }
    }

    // メールボックスが未配線でも Tier G は積まれること。
    //
    // 上の述語検査と同じことを、**翻訳器の側から**も問う。
    // canEmitDynamicBranch が false を返す env は実機で実際に起きる
    // (mailboxAddr == 0)。そこで Tier G が落ちたら被覆率が黙って減る。
    for (const Case& c : cases)
    {
        INFO(std::string(c.what), " / メールボックス未配線");
        std::vector<u16> words = c.words;
        words.push_back(moveq(1, 7));

        FlatCode code;
        BlockPlan plan{};
        x68k::PlanCapabilities caps{};
        caps.canEmitDynamicBranch = [](void*) { return false; };
        REQUIRE(buildPlan(words, plan, code, caps));
        CHECK(plan.count == 2);
    }
}

// --- Tier H: 命令長デコーダの拡張が解禁した形 -------------------------------

// CMPI.<size> #imm,Dn : 0000 110 ss 000 rrr。
constexpr u16 cmpiToDn(u32 dst, u32 sizeBits)
{
    return static_cast<u16>(0x0C00u | (sizeBits << 6) | dst);
}
// BTST #imm,Dn : 0000 100 00 000 rrr。
constexpr u16 btstImmToDn(u32 dst)
{
    return static_cast<u16>(0x0800u | dst);
}
// ADDQ / SUBQ #data,<ea> : 0101 ddd s ss mmm rrr。
// data は 1-8 (8 は符号化では 0)。isSub で bit8 が立つ。
constexpr u16 addqTo(u32 data, bool isSub, u32 sizeBits, u32 mode, u32 reg)
{
    const u32 field = data == 8 ? 0u : data;
    return static_cast<u16>(0x5000u | (field << 9) | (isSub ? 0x0100u : 0u) | (sizeBits << 6) |
                            (mode << 3) | reg);
}

// CMPI #imm,Dn — 実測でこの段の最大の的。
//
// **CMP #imm,Dn ($Bxxx の即値形) と意味はビット単位で同じ。** 違うのは
// 符号とサイクル (CMPI は 8、CMP #imm は 4) だけなので、planner は
// kAluImmToDreg として積み、cycles だけを分ける。
//
// 落ちる変異:
//   - planImmediate の cycles を 4 にする (CMP #imm と取り違え)
//   - CMPI の即値語数を .l で 1 語にする (デコーダ側。上位が落ちる)
//   - aluOp を kSub にする (結果が Dn へ書き戻され、d[] が壊れる)
TEST_CASE("CMPI #imm,Dn がインタプリタと一致する")
{
    // 桁溢れと符号の境界。V と C はここでしか壊れない。
    static constexpr u32 kDstValues[] = {
        0x00000000u, 0x00000001u, 0x0000007Fu, 0x00000080u, 0x00007FFFu,
        0x00008000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u,
    };

    for (u32 dv : kDstValues)
    {
        M68kState s = makeState(1);
        s.d[2] = dv;
        // **X が保存されることを問う。** CMPI は applyResultFlags(.., false)
        // なので X を触らない。入口で立てておけば、触った変異で落ちる。
        s.sr = static_cast<u16>(0x2700u | 0x10u);

        // .b : 即値 1 語。**上位バイトは捨てられる** (readEaSlow の 7.4)。
        checkEquivalence({cmpiToDn(2, 0u), 0xFF7Fu}, s, "CMPI.b #imm,Dn");
        checkEquivalence({cmpiToDn(2, 0u), 0x0080u}, s, "CMPI.b #imm,Dn 符号");
        checkEquivalence({cmpiToDn(2, 0u), 0x0000u}, s, "CMPI.b #imm,Dn ゼロ");
        // .w : 即値 1 語。
        checkEquivalence({cmpiToDn(2, 1u), 0x8000u}, s, "CMPI.w #imm,Dn");
        checkEquivalence({cmpiToDn(2, 1u), 0x7FFFu}, s, "CMPI.w #imm,Dn 正の端");
        checkEquivalence({cmpiToDn(2, 1u), 0x0000u}, s, "CMPI.w #imm,Dn ゼロ");
        // .l : 即値 **2 語**。上位が落ちると必ず落ちる。
        checkEquivalence({cmpiToDn(2, 2u), 0x8000u, 0x0000u}, s, "CMPI.l #imm,Dn");
        checkEquivalence({cmpiToDn(2, 2u), 0x1234u, 0x5678u}, s, "CMPI.l #imm,Dn 任意");
        checkEquivalence({cmpiToDn(2, 2u), 0xFFFFu, 0xFFFFu}, s, "CMPI.l #imm,Dn -1");
    }
}

// ADDQ / SUBQ #imm,Dn — **フラグを変える。**
//
// **即値は命令語の bit9-11 に埋まっている。** 拡張ワードを持たないので、
// foldImmediate が ext0 から畳み直すと即値が黙って 0 になる。
//
// 落ちる変異:
//   - foldImmediate の kAluImmToDreg で length == 2 の枝を落とす
//     (即値が 0 になり、ADDQ #1 が ADD #0 に化ける)
//   - 符号化の data == 0 を 8 と読まない (ADDQ #8 が ADDQ #0 になる)
//   - planQuickAlu の cycles を 4 にする (ADD #imm と取り違え)
//   - applyResultFlags の X を落とす (ADDQ は X を書く)
TEST_CASE("ADDQ / SUBQ #imm,Dn がインタプリタと一致する")
{
    static constexpr u32 kDstValues[] = {
        0x00000000u, 0x00000001u, 0x0000007Fu, 0x00000080u, 0x000000FFu, 0x00007FFFu,
        0x00008000u, 0x0000FFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u,
    };

    // **data は 1-8 を全部回す。** 8 は符号化で 0 なので、そこだけ
    // 特別扱いする実装 (data == 0 なら 8) が要る。
    for (u32 data = 1; data <= 8; ++data)
    {
        for (bool isSub : {false, true})
        {
            for (u32 dv : kDstValues)
            {
                for (u32 sizeBits : {0u, 1u, 2u})
                {
                    M68kState s = makeState(1);
                    s.d[3] = dv;
                    // X が上書きされること (ADDQ/SUBQ は書く) を問う。
                    s.sr = static_cast<u16>(0x2700u | 0x10u);
                    checkEquivalence({addqTo(data, isSub, sizeBits, 0u, 3u)}, s,
                                     "ADDQ/SUBQ #imm,Dn");
                }
            }
        }
    }
}

// ADDQ / SUBQ #imm,An — **フラグを 1 bit も変えず、常に 32bit で作用する。**
//
// Dn 宛てと取り違えると CCR が動く。.w でも a[] の 32bit 全体に効くので、
// size で結果を切ると .w が上位を落とす。
//
// 落ちる変異:
//   - emitAddqImmToAreg で emitStoreCcr を呼ぶ (フラグが動く)
//   - 転送先を aOffset ではなく dOffset にする (Dn 宛てとの取り違え)
//   - op.size で結果を切る (.w が上位を落とす)
//   - planQuickAlu が An 宛てを kAluImmToDreg として積む
TEST_CASE("ADDQ / SUBQ #imm,An がインタプリタと一致する")
{
    // 32bit の折り返しと符号の境界。**フラグ不変なので、CCR が動いたら
    // どの値でも落ちる。**
    static constexpr u32 kDstValues[] = {
        0x00000000u, 0x00000001u, 0x00007FFFu, 0x00008000u,
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x00FFFFF8u,
    };

    for (u32 data = 1; data <= 8; ++data)
    {
        for (bool isSub : {false, true})
        {
            for (u32 dv : kDstValues)
            {
                // **.w と .l の両方。** どちらも 32bit で効くので、
                // 同じ結果になることが主張。
                for (u32 sizeBits : {1u, 2u})
                {
                    M68kState s = makeState(1);
                    s.a[3] = dv;
                    // **CCR を全部立てておく。** 1 bit でも落ちたら落ちる。
                    s.sr = static_cast<u16>(0x2700u | 0x1Fu);
                    checkEquivalence({addqTo(data, isSub, sizeBits, 1u, 3u)}, s,
                                     "ADDQ/SUBQ #imm,An");
                }
            }
        }
    }

    // **planner は符号化のサイズを size 欄へ入れる** (kLong で埋めない)。
    //
    // エミッタはこの欄を読まないが、**常に 4 にすると欄が情報を失い、
    // エミッタが誤って結果を size で切っても何も切られない**。
    // そうなると上の差分テストが 1 件も落ちない (実際に変異を注入して
    // 確かめた: kLong を入れた実装では emitTruncate を足しても
    // 751 件が全部通った)。欄の中身をここで名指しで固定する。
    {
        x68k::PlannedOp p{};
        REQUIRE(x68k::BlockPlanner::planOne(addqTo(1, false, 1u, 1u, 3u), kEntry, p));
        CHECK(p.kind == PlanKind::kAddqImmToAreg);
        CHECK(p.size == 2);  // .w は 2。**4 ではない**
        REQUIRE(x68k::BlockPlanner::planOne(addqTo(1, false, 2u, 1u, 3u), kEntry, p));
        CHECK(p.size == 4);  // .l は 4
    }

    // **A7 でも同じ。** 特例 (byte の -(A7) が 2 進む) は ADDQ には
    // 無いので、A7 だけ別扱いにした変異があれば落ちる。
    for (u32 data : {1u, 8u})
    {
        M68kState s = makeState(1);
        s.a[7] = 0x00001000u;
        s.sr = static_cast<u16>(0x2700u | 0x1Fu);
        checkEquivalence({addqTo(data, false, 2u, 1u, 7u)}, s, "ADDQ.l #imm,A7");
        checkEquivalence({addqTo(data, true, 2u, 1u, 7u)}, s, "SUBQ.l #imm,A7");
    }
}

// BTST #imm,Dn — **Z フラグだけを変える。** N/V/C/X は保存される。
//
// 対象が Dn なので 32bit で回り、ビット番号は 31 でマスクされる。
// Z は「テストしたビットが **0 だったか**」なので論理が逆。
//
// 落ちる変異:
//   - emitBtstImm の clearMask を kLogicClear にする (N/V/C が落ちる)
//   - Z の論理を反転する (moveqz を movnez にする)
//   - foldImmediate の & 31 を落とす (シフト量が 32 以上で未定義)
//   - foldImmediate の & 0xFF を落とす (拡張ワードの上位が混じる)
TEST_CASE("BTST #imm,Dn がインタプリタと一致する")
{
    // **ビット番号を 0-31 で全部回す。** 境界 (0 と 31) だけでなく
    // 途中も見るのは、シフト量の取り違えがどこで出るか分からないため。
    for (u32 bit = 0; bit < 32; ++bit)
    {
        for (u32 dv : {0x00000000u, 0xFFFFFFFFu, 0x80000001u, 0x12345678u, 0xAAAAAAAAu})
        {
            M68kState s = makeState(1);
            s.d[4] = dv;
            // **CCR を全部立てておく。** Z 以外が落ちたらそこで落ちる。
            s.sr = static_cast<u16>(0x2700u | 0x1Fu);
            checkEquivalence({btstImmToDn(4), static_cast<u16>(bit)}, s, "BTST #imm,Dn");

            // **Z が 0 の入口からも問う。** 上の入口は Z が立っているので、
            // 「Z を書かない」変異が「立てる」ケースで素通りする。
            M68kState s2 = makeState(1);
            s2.d[4] = dv;
            s2.sr = static_cast<u16>(0x2700u);
            checkEquivalence({btstImmToDn(4), static_cast<u16>(bit)}, s2, "BTST #imm,Dn (Z=0)");
        }
    }

    // **ビット番号は 31 でマスクされる。** 拡張ワードに 32 以上を置くと、
    // インタプリタは bitNumber &= 31 で折り返す。
    for (u32 raw : {32u, 33u, 63u, 0xFFu, 0x1FFu})
    {
        M68kState s = makeState(1);
        s.d[4] = 0x12345678u;
        s.sr = static_cast<u16>(0x2700u | 0x1Fu);
        checkEquivalence({btstImmToDn(4), static_cast<u16>(raw)}, s, "BTST #imm,Dn マスク");
    }
}

// Tier H もブロックを終端しない。
//
// **これが段 0-H の要点。** 段 0-F (BSR) の教訓 (終端はブロックを切る) から、
// 非終端の形だけを選んである。終端側に回ると被覆率は上がっても速度が
// 付いてこない。
//
// 落ちる変異: isBlockTerminator に Tier H の kind を足す
TEST_CASE("Tier H の命令はブロックを終端しない")
{
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const Case cases[] = {
        {{cmpiToDn(2, 1u), 0x1234u}, "CMPI.w #imm,Dn"},
        {{cmpiToDn(2, 2u), 0x1234u, 0x5678u}, "CMPI.l #imm,Dn"},
        {{btstImmToDn(4), 0x0005u}, "BTST #imm,Dn"},
        {{addqTo(1, false, 1u, 0u, 3u)}, "ADDQ.w #1,Dn"},
        {{addqTo(8, true, 2u, 0u, 3u)}, "SUBQ.l #8,Dn"},
        {{addqTo(1, false, 2u, 1u, 3u)}, "ADDQ.l #1,An"},
        {{addqTo(4, true, 1u, 1u, 3u)}, "SUBQ.w #4,An"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        std::vector<u16> words = c.words;
        words.push_back(moveq(1, 7));

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        // **2 命令入ること**が主張。1 なら終端側に回っている。
        CHECK(plan.count == 2);
        CHECK(plan.end != BlockEnd::kBranch);
        CHECK(plan.end != BlockEnd::kDynamicBranch);
        for (std::uint32_t i = 0; i < plan.count; ++i)
        {
            CHECK_FALSE(x68k::isBlockTerminator(plan.ops[i].kind));
            CHECK_FALSE(x68k::hasStaticBranchTarget(plan.ops[i].kind));
        }
    }

    // **窓が一切使えなくても積まれること。** Tier H はどれもメモリに
    // 触れないので、読み・書き・メールボックスのどれが未配線でも
    // 落ちてはいけない。落ちたら被覆率が黙って減る。
    for (const Case& c : cases)
    {
        INFO(std::string(c.what), " / 窓もメールボックスも未配線");
        std::vector<u16> words = c.words;
        words.push_back(moveq(1, 7));

        FlatCode code;
        BlockPlan plan{};
        x68k::PlanCapabilities caps{};
        caps.canEmitReads = [](void*) { return false; };
        caps.canEmitWrites = [](void*) { return false; };
        caps.canEmitDynamicBranch = [](void*) { return false; };
        REQUIRE(buildPlan(words, plan, code, caps));
        CHECK(plan.count == 2);
    }
}

// **DBcc と TRAP はデコーダが長さを返すが、翻訳対象には入らない。**
//
// これが段 0-H の切り分け。長さが分かると、planner は
// 「未対応命令の手前で終端 (kUnsupported)」として数えられるようになる。
// 長さ不明のままだと kUnknownLength に混ざり、どちらで落ちているかが
// 統計で見えなくなる (block_plan.h の BlockEnd)。
//
// **手前までは積める。** DBcc / TRAP を含む区間でも、その手前の命令は
// ブロックに入る。デコーダを直すこと自体が、翻訳対象に入れていない
// 命令の周辺にも効く理由がこれ。
//
// 落ちる変異:
//   - planQuickAlu が ss == 11 を弾かない (DBcc / Scc が入る)
//   - planMisc が TRAP を受ける (例外に入る命令がブロックへ入る)
TEST_CASE("DBcc と TRAP は長さが分かるが翻訳対象には入らない")
{
    struct Case
    {
        u16 op;
        u32 length;
        const char* what;
    };
    const Case cases[] = {
        {0x51C8u, 4u, "DBRA D0,<label>"}, {0x57CFu, 4u, "DBEQ D7,<label>"},
        {0x50C0u, 2u, "ST D0 (Scc)"},     {0x4E40u, 2u, "TRAP #0"},
        {0x4E4Fu, 2u, "TRAP #15"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        // 長さは返る。
        CHECK(x68k::instructionLength(c.op) == c.length);
        // だが許可リストには入らない。
        x68k::PlannedOp planned{};
        CHECK_FALSE(x68k::BlockPlanner::planOne(c.op, kEntry, planned));

        // **手前の命令は積める。** MOVEQ を 2 つ置いてから対象を置くと、
        // ブロックは 2 命令ぶん積んで「未対応命令の手前」で切れる。
        FlatCode code;
        BlockPlan plan{};
        const std::vector<u16> words = {moveq(1, 7), moveq(2, 3), c.op, 0x0000u};
        REQUIRE(buildPlan(words, plan, code));
        CHECK(plan.count == 2);
        // **長さが分かっているので kUnsupported。** kUnknownLength ではない。
        // ここが段 0-H でデコーダを直した効果そのもの。
        CHECK(plan.end == BlockEnd::kUnsupported);
    }
}

// MOVEA <mem>,An も窓ガードの外なら**状態を 1 bit も変えずに降りる** (G3/G7)。
//
// 読み形 (Tier B) と同じ島へ降りるが、**転送先が a[] になった**ので、
// ガードを消した変異は「An が書き換わる」形で出る。
//
// 落ちる変異:
//   - emitReadGuard を消す (窓の外を読み、An が壊れる)
//   - bound を limit にする (窓の末尾で範囲外を読む)
//   - commit をガードより前へ動かす (An が進んだまま降りる)
TEST_CASE("MOVEA の窓ガードが不成立なら状態が 1 bit も変わらない")
{
    const u32 outside = static_cast<u32>(x68k::kMainRamSize) + 0x1000u;
    for (u32 group : {0x2u, 0x3u})
    {
        for (u32 mode : {kModeInd, kModePostInc, kModePreDec})
        {
            M68kState s = makeState(5);
            s.a[2] = outside;
            // 手前に 1 命令置いて、**その命令ぶんだけ実行して降りる**ことも問う。
            checkEquivalence({moveq(0, 3), moveaMemToAn(group, 5, mode, 2)}, s, "MOVEA 窓の外");
        }
    }
}

// 窓の端ちょうど。**a == limit - size は成立、その先は不成立。**
// MOVEA.w は 2 バイト、MOVEA.l は 4 バイト。size 欄を 4 に固定すると
// .w の端 2 バイトを落とす。
//
// 落ちる変異: planMove の MOVEA 読み形で size を常に kLong にする
TEST_CASE("MOVEA の窓の境界がインタプリタと一致する")
{
    const u32 limit = static_cast<u32>(x68k::kMainRamSize);
    for (u32 group : {0x2u, 0x3u})
    {
        const u32 size = group == 0x2u ? 4u : 2u;
        for (u32 addr : {limit - size, limit - size + 2u, limit - 2u, limit})
        {
            M68kState s = makeState(6);
            s.a[2] = addr;
            checkEquivalence({moveaMemToAn(group, 5, kModeInd, 2)}, s, "MOVEA 窓の端");
        }
    }
}

TEST_CASE("読みガードの境界がインタプリタと一致する")
{
    // 窓の端ちょうど。**a == limit - size は成立、その先は不成立。**
    //
    // **「成立したこと」を明示的に問う。** 同値比較だけだと、ガードが
    // 保守的に外れても checkEquivalence は「脱出した地点までは一致」で
    // 緑になる。範囲を 1 バイト狭める変異は正しさを壊さないので、
    // 諦めた回数を数えないかぎり永遠に見えない
    // (保守的なフォールバックはテストの盲点になる)。
    const u32 limit = static_cast<u32>(x68k::kMainRamSize);

    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        const u32 size = group == 0x1u ? 1u : (group == 0x2u ? 4u : 2u);
        {
            // ちょうど収まる最後のアドレス。**脱出してはいけない。**
            M68kState s = makeState(2);
            s.a[2] = limit - size;
            const std::vector<u16> words{moveMemToDn(group, 1, kModeInd, 2)};
            checkEquivalence(words, s, "境界ちょうど (成立)");

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            std::fill(execRam().begin(), execRam().end(), 0);
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("size=", size, " addr=limit-size");
            CHECK_FALSE(native.guardExit);
        }
        {
            // 1 語外。ガードが不成立になり、**必ず脱出する**。
            M68kState s = makeState(2);
            s.a[2] = limit - size + 2u;  // 偶数を保ったまま外へ出す
            const std::vector<u16> words{moveMemToDn(group, 1, kModeInd, 2)};
            checkEquivalence(words, s, "境界の外 (不成立)");

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            std::fill(execRam().begin(), execRam().end(), 0);
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("size=", size, " addr=limit-size+2");
            CHECK(native.guardExit);
            CHECK(native.ranOps == 0);
        }
        {
            // **窓の最初の 1 バイト外ちょうど (a == limit)。**
            //
            // 範囲を 1 だけ広げる変異は、word / long なら「そのアドレスが
            // 奇数になる」ので整列判定に救われて見えない。**byte には
            // 整列判定が無い**ので、ここだけが 1 バイト外の読みを捕まえる。
            // 偶数刻みで外へ出すテストでは a == limit を踏まない。
            M68kState s = makeState(2);
            s.a[2] = limit;
            const std::vector<u16> words{moveMemToDn(group, 1, kModeInd, 2)};

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            std::fill(execRam().begin(), execRam().end(), 0);
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("size=", size, " addr=limit ちょうど");
            CHECK(native.guardExit);
            CHECK(native.ranOps == 0);
        }
    }
    clearSeeds();
}

TEST_CASE("窓の中を指す読みはガードを通り抜ける")
{
    // ガードが**成立する側**を数える。
    //
    // 諦めても正しさは壊れないので、「脱出しなかったこと」を問わないと
    // ガードが保守的に外れていることに気づけない。上の境界テストと
    // 合わせて、成立/不成立の両側を固定する。
    seedData(kDataAddr, 0x12, 0x34, 0x56, 0x78);

    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        for (u32 mode : {kModeInd, kModePostInc, kModePreDec, kModeDisp})
        {
            std::vector<u16> words{moveMemToDn(group, 1, mode, 2)};
            if (mode == kModeDisp)
            {
                words.push_back(0x0000u);
            }
            M68kState s = makeState(3);
            s.a[2] = mode == kModePreDec
                         ? kDataAddr + (group == 0x1u ? 1u : (group == 0x2u ? 4u : 2u))
                         : kDataAddr;

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            std::fill(execRam().begin(), execRam().end(), 0);
            applyGuestSeeds();
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("group=", group, " mode=", mode);
            CHECK_FALSE(native.guardExit);
        }
    }
    clearSeeds();
}

TEST_CASE("ガード不成立で状態が 1 bit も変わらない")
{
    // **(An)+ の An が進んでいないこと**が要点 (G3/G4)。
    // ガードより前に commit する実装だと、ここで a[] が食い違う。
    for (u32 group : {0x2u, 0x3u})
    {
        for (u32 mode : {kModeInd, kModePostInc, kModePreDec})
        {
            M68kState s = makeState(6);
            // 窓の外を指す。
            s.a[4] = static_cast<u32>(x68k::kMainRamSize) + 0x1000u;
            checkEquivalence({moveMemToDn(group, 1, mode, 4)}, s, "窓の外で脱出");
        }
    }

    // 奇数アドレス。word / long は必ず不成立 (byte は成立する)。
    for (u32 group : {0x2u, 0x3u})
    {
        M68kState s = makeState(6);
        s.a[4] = kDataAddr + 1u;
        checkEquivalence({moveMemToDn(group, 1, kModeInd, 4)}, s, "奇数アドレスで脱出");
    }
    // byte は奇数でも読める。
    {
        seedData(kDataAddr, 0x11, 0x99, 0x33, 0x44);
        M68kState s = makeState(6);
        s.a[4] = kDataAddr + 1u;
        checkEquivalence({moveMemToDn(0x1u, 1, kModeInd, 4)}, s, "byte は奇数でも読める");
        clearSeeds();
    }
}

TEST_CASE("ブロックの途中で脱出したときの境界状態")
{
    // 先頭は必ず成立する命令、2 番目で脱出させる。
    //
    // **出口は「2 番目の命令の直前の命令境界」** (G7)。pc / ir / irc と
    // サイクル (1 命令ぶんだけ) の 4 つを、参照側の 1 命令実行と比べる。
    M68kState s = makeState(8);
    s.a[4] = static_cast<u32>(x68k::kMainRamSize) + 0x2000u;  // 窓の外
    checkEquivalence({moveq(0, 1), moveMemToDn(0x3u, 1, kModeInd, 4)}, s, "2 命令目で脱出");

    // 3 命令目で脱出する形。**手前 2 命令ぶんのサイクルだけ返すこと。**
    M68kState t = makeState(9);
    t.a[5] = static_cast<u32>(x68k::kMainRamSize) + 0x2000u;
    checkEquivalence({moveq(0, 7), moveq(1, -3), moveMemToDn(0x2u, 2, kModeInd, 5)}, t,
                     "3 命令目で脱出");
}

TEST_CASE("-(An) が 0 からラップしてもインタプリタと一致する")
{
    // a[n] == 0 で -(An) すると 32bit で 0xFFFFFFFC へ回り込む。
    // マスク後は 0x00FFFFFC で、2MB の窓の外なのでガードは不成立。
    // **無マスクの環算**であることを、a[] の値そのもので問う。
    M68kState s = makeState(5);
    s.a[3] = 0;
    checkEquivalence({moveMemToDn(0x2u, 1, kModePreDec, 3)}, s, "-(An) が 0 からラップ");
}

TEST_CASE("上位バイト付きアドレスは 24bit にマスクされる")
{
    // インタプリタは addr & 0x00FFFFFF で読む (m68k.cpp の read8/16/32)。
    // **マスクを落とすと窓の外と判定してしまう** (偽の脱出) ので、
    // ガードが成立して正しい値を読むことを問う。
    seedData(kDataAddr, 0xC0, 0xFF, 0xEE, 0x00);
    M68kState s = makeState(7);
    s.a[2] = 0xFF000000u | kDataAddr;
    checkEquivalence({moveMemToDn(0x2u, 1, kModeInd, 2)}, s, "上位バイト付き (An)");

    // (An)+ なら a[] に**マスクしていない**値 + step が入る。
    M68kState t = makeState(7);
    t.a[2] = 0xFF000000u | kDataAddr;
    checkEquivalence({moveMemToDn(0x2u, 1, kModePostInc, 2)}, t, "上位バイト付き (An)+");
    clearSeeds();
}

TEST_CASE("読み形と分岐終端を混ぜたブロック")
{
    seedData(kDataAddr, 0x00, 0x00, 0x00, 0x00);
    for (u32 cond : {0x6u, 0x7u})  // NE / EQ
    {
        M68kState s = makeState(4);
        s.a[2] = kDataAddr;
        // TST が Z を立て、その Z で分岐する。**ガードと分岐が同居する形。**
        checkEquivalence({tstMem(2u, kModeInd, 2), bcc(cond, 6)}, s, "TST (An) + Bcc");
    }
    clearSeeds();
}

TEST_CASE("窓が読めない写像では読み形を発行しない")
{
    // G12: ROM 写像中 (ramReadable == false) やウォッチ中 (base == 0) は、
    // 読み形を焼かない。**負のキャッシュに入っても、写像が戻れば
    // epoch の変化で捨てられる。**
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({moveMemToDn(0x2u, 1, kModeInd, 2)}, plan, code));
    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);

    // 読める窓なら発行できる。
    CHECK(jit::requiredSize(plan, ir, irc, fakeEnv()) > 0);

    // 読めない写像。
    jit::EmitEnv unreadable = fakeEnv();
    unreadable.ramReadable = false;
    CHECK(jit::requiredSize(plan, ir, irc, unreadable) == 0);

    // 窓が無い (ウォッチ中)。
    jit::EmitEnv noWindow = fakeEnv();
    noWindow.ramBaseAddr = 0;
    CHECK(jit::requiredSize(plan, ir, irc, noWindow) == 0);

    // 窓が短すぎてそのサイズを一度も許さない。
    jit::EmitEnv tiny = fakeEnv();
    tiny.ramLimit = 2;
    CHECK(jit::requiredSize(plan, ir, irc, tiny) == 0);

    // Tier A だけの計画なら、窓が読めなくても発行できる
    // (メモリを触らないので窓に依存しない)。
    BlockPlan tierA{};
    FlatCode codeA;
    REQUIRE(buildPlan({moveq(1, 5)}, tierA, codeA));
    CHECK(jit::requiredSize(tierA, codeA.get16(tierA.fallThroughPc),
                            codeA.get16(tierA.fallThroughPc + 2), unreadable) > 0);
}

TEST_CASE("窓の外を指す絶対アドレスは翻訳時に弾く")
{
    // G6: 絶対アドレスは実効アドレスが翻訳時に決まるので、**その場で
    // 判定する。** 走らせてから諦める形にすると、絶対に成立しない
    // ガードを毎回踏むブロックができる。
    FlatCode code;
    BlockPlan plan{};
    const u32 outside = static_cast<u32>(x68k::kMainRamSize) + 0x1000u;
    REQUIRE(buildPlan({moveMemToDn(0x2u, 1, kModeAbsL, 1), static_cast<u16>(outside >> 16),
                       static_cast<u16>(outside & 0xFFFFu)},
                      plan, code));
    CHECK(jit::requiredSize(plan, code.get16(plan.fallThroughPc),
                            code.get16(plan.fallThroughPc + 2), fakeEnv()) == 0);

    // 奇数の絶対アドレスも同じく積まない (word 以上)。
    FlatCode oddCode;
    BlockPlan oddPlan{};
    const u32 odd = kDataAddr + 1u;
    REQUIRE(buildPlan({moveMemToDn(0x2u, 1, kModeAbsL, 1), static_cast<u16>(odd >> 16),
                       static_cast<u16>(odd & 0xFFFFu)},
                      oddPlan, oddCode));
    CHECK(jit::requiredSize(oddPlan, oddCode.get16(oddPlan.fallThroughPc),
                            oddCode.get16(oddPlan.fallThroughPc + 2), fakeEnv()) == 0);
}

// --- Tier C: 書きガード -----------------------------------------------------
//
// 保証するのは 4 つ:
//
//   G13 ページ凍結  自ブロックのページへの書きは、書く前に脱出する
//   G14 書きの同値  書くバイトと**世代の動き方**がインタプリタと一致する。
//                   `.l` は同一ページでも touch 2 回
//   G16 touch の位置 最後のガードの後・store の前
//   G17 絶対形の二重防御 翻訳時に積まない + 実行時ガードも吐く
//
// **checkEquivalence は世代配列とゲスト RAM も比較する**ので、以下の
// TEST_CASE は書いた値・書いた位置・世代の動きを一度に問うている。

namespace
{

// 書き形のテストで使うゲストアドレス。**自ブロックのページ (kEntry >> 10)
// から遠い**ところにする。近いと G13 の脱出が混ざって比較の意味が変わる。
constexpr u32 kWriteAddr = 0x00080000u;

}  // namespace

TEST_CASE("MOVE Dn,<mem> がインタプリタと一致する")
{
    clearSeeds();
    // sizeGroup: $1 = byte / $2 = long / $3 = word。**$2 が long。**
    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        const u32 size = group == 0x1u ? 1u : (group == 0x2u ? 4u : 2u);
        for (u32 seed : {1u, 2u, 5u})
        {
            // (An) — 最頻の形。
            {
                M68kState s = makeState(seed);
                s.a[2] = kWriteAddr;
                checkEquivalence({moveDnToMem(group, 1, kModeInd, 2)}, s, "MOVE Dn,(An)");
            }
            // 値が 0 になる形。**Z フラグ**と、書いたバイトが全部 0 になること。
            {
                M68kState s = makeState(seed);
                s.d[3] = 0;
                s.a[2] = kWriteAddr;
                checkEquivalence({moveDnToMem(group, 3, kModeInd, 2)}, s, "MOVE Dn,(An) ゼロ");
            }
            // 符号ビットが立つ値。N フラグと、切り出しの位置。
            {
                M68kState s = makeState(seed);
                s.d[4] = 0x80808080u;
                s.a[2] = kWriteAddr;
                checkEquivalence({moveDnToMem(group, 4, kModeInd, 2)}, s, "MOVE Dn,(An) 負値");
            }
            // (An)+ — An が進むこと。
            {
                M68kState s = makeState(seed);
                s.a[4] = kWriteAddr;
                checkEquivalence({moveDnToMem(group, 2, kModePostInc, 4)}, s, "MOVE Dn,(An)+");
            }
            // -(An) — 先に引いてから書くこと。
            {
                M68kState s = makeState(seed);
                s.a[5] = kWriteAddr + size;
                checkEquivalence({moveDnToMem(group, 6, kModePreDec, 5)}, s, "MOVE Dn,-(An)");
            }
            // (d16,An) — 正負の変位。
            {
                M68kState s = makeState(seed);
                s.a[3] = kWriteAddr - 0x10u;
                checkEquivalence({moveDnToMem(group, 1, kModeDisp, 3), 0x0010u}, s,
                                 "MOVE Dn,(d16,An) 正変位");
                s.a[3] = kWriteAddr + 0x10u;
                checkEquivalence({moveDnToMem(group, 1, kModeDisp, 3), 0xFFF0u}, s,
                                 "MOVE Dn,(d16,An) 負変位");
            }
            // (xxx).L — 絶対アドレス。**ガードは翻訳時にも実行時にも在る (G17)。**
            {
                M68kState s = makeState(seed);
                checkEquivalence(
                    {moveDnToMem(group, 7, kModeAbsL, 1), static_cast<u16>(kWriteAddr >> 16),
                     static_cast<u16>(kWriteAddr & 0xFFFFu)},
                    s, "MOVE Dn,(xxx).L");
            }
            // (xxx).W — 符号拡張される。小さい正の番地を使う。
            {
                M68kState s = makeState(seed);
                checkEquivalence({moveDnToMem(group, 4, kModeAbsW, 0), 0x0400u}, s,
                                 "MOVE Dn,(xxx).W");
            }
        }
    }
}

TEST_CASE("CLR <mem> がインタプリタと一致する")
{
    clearSeeds();
    for (u32 sizeField : {0u, 1u, 2u})
    {
        const u32 size = sizeField == 0 ? 1u : (sizeField == 1 ? 2u : 4u);
        // 書き先に 0 でないバイトを置いておく。**0 を書いたことが見える**
        // ようにするため (最初から 0 だと、書かない変異が緑で通る)。
        seedData(kWriteAddr, 0x11, 0x22, 0x33, 0x44);

        for (u32 mode : {kModeInd, kModePostInc})
        {
            M68kState s = makeState(4);
            s.a[2] = kWriteAddr;
            checkEquivalence({clrMem(sizeField, mode, 2)}, s, "CLR <mem>");
        }
        {
            M68kState s = makeState(4);
            s.a[3] = kWriteAddr + size;
            checkEquivalence({clrMem(sizeField, kModePreDec, 3)}, s, "CLR -(An)");
        }
        {
            M68kState s = makeState(4);
            s.a[5] = kWriteAddr - 0x20u;
            checkEquivalence({clrMem(sizeField, kModeDisp, 5), 0x0020u}, s, "CLR (d16,An)");
        }
        {
            M68kState s = makeState(4);
            checkEquivalence({clrMem(sizeField, kModeAbsL, 1), static_cast<u16>(kWriteAddr >> 16),
                              static_cast<u16>(kWriteAddr & 0xFFFFu)},
                             s, "CLR (xxx).L");
        }
    }
    clearSeeds();
}

TEST_CASE("A7 をバイトで書く (An)+ / -(An) は 2 進む")
{
    // 読み側と同じ特例 (m68k.h:440-458)。**eaRegOf を srcReg 直書きに
    // 戻す変異**はここで落ちる: 書き形の srcReg は転送元の Dn 番号なので、
    // MOVE.b D7,-(A7) と MOVE.b D0,-(A7) で減るレジスタが変わってしまう。
    clearSeeds();

    M68kState post = makeState(3);
    post.a[7] = kWriteAddr;
    checkEquivalence({moveDnToMem(0x1u, 0, kModePostInc, 7)}, post, "MOVE.b D0,(A7)+");

    M68kState pre = makeState(3);
    pre.a[7] = kWriteAddr + 2u;
    checkEquivalence({moveDnToMem(0x1u, 0, kModePreDec, 7)}, pre, "MOVE.b D0,-(A7)");

    // **srcReg == 7 でも dstReg == 7 でない形**。srcReg 直書きの変異は
    // 「D7 を書くから 2 進む」と誤判定して、A2 が 2 進んでしまう。
    M68kState wrong = makeState(3);
    wrong.a[2] = kWriteAddr;
    checkEquivalence({moveDnToMem(0x1u, 7, kModePostInc, 2)}, wrong, "MOVE.b D7,(A2)+");

    // CLR.b -(A7) も同じ特例。CLR は srcReg を使わない (0 のまま) ので、
    // srcReg 直書きだと A0 が減る。
    M68kState clr = makeState(3);
    clr.a[7] = kWriteAddr + 2u;
    checkEquivalence({clrMem(0u, kModePreDec, 7)}, clr, "CLR.b -(A7)");
}

TEST_CASE("書きガードの境界がインタプリタと一致する")
{
    clearSeeds();
    const u32 limit = static_cast<u32>(x68k::kMainRamSize);

    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        const u32 size = group == 0x1u ? 1u : (group == 0x2u ? 4u : 2u);
        const std::vector<u16> words{moveDnToMem(group, 1, kModeInd, 2)};

        // ちょうど収まる最後のアドレス。**脱出してはいけない。**
        //
        // **「成立したこと」を明示的に問う。** 同値比較だけだと、ガードが
        // 保守的に外れても「脱出した地点までは一致」で緑になる。
        {
            M68kState s = makeState(2);
            s.a[2] = limit - size;
            checkEquivalence(words, s, "書き 境界ちょうど (成立)");

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            resetExecRam(words);
            resetExecGen();
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("size=", size, " addr=limit-size");
            CHECK_FALSE(native.guardExit);
        }
        // 1 語外。ガードが不成立になり、**必ず脱出する**。
        {
            M68kState s = makeState(2);
            s.a[2] = limit - size + 2u;
            checkEquivalence(words, s, "書き 境界の外 (不成立)");

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            resetExecRam(words);
            resetExecGen();
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("size=", size, " addr=limit-size+2");
            CHECK(native.guardExit);
            CHECK_FALSE(native.selfPageExit);
            CHECK(native.ranOps == 0);
        }
        // **窓の最初の 1 バイト外ちょうど (a == limit)。**
        //
        // 範囲を 1 だけ広げる変異は、word / long なら「そのアドレスが
        // 奇数になる」ので整列判定に救われて見えない。byte だけが捕まえる。
        {
            M68kState s = makeState(2);
            s.a[2] = limit;
            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            resetExecRam(words);
            resetExecGen();
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("size=", size, " addr=limit ちょうど");
            CHECK(native.guardExit);
            CHECK(native.ranOps == 0);
        }
    }
}

TEST_CASE("書きガード不成立で状態もメモリも世代も 1 bit も変わらない")
{
    clearSeeds();
    // **(An)+ の An が進んでいないこと**と、**世代が動いていないこと**
    // (G16: ガード不成立の経路では touch しない) が要点。
    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        for (u32 mode : {kModeInd, kModePostInc, kModePreDec})
        {
            M68kState s = makeState(6);
            s.a[4] = static_cast<u32>(x68k::kMainRamSize) + 0x1000u;  // 窓の外
            checkEquivalence({moveDnToMem(group, 1, mode, 4)}, s, "書き 窓の外で脱出");
            checkEquivalence({clrMem(group == 0x1u ? 0u : (group == 0x2u ? 2u : 1u), mode, 4)}, s,
                             "CLR 窓の外で脱出");
        }
    }

    // 奇数アドレス。word / long は必ず不成立 (byte は成立する)。
    for (u32 group : {0x2u, 0x3u})
    {
        M68kState s = makeState(6);
        s.a[4] = kWriteAddr + 1u;
        checkEquivalence({moveDnToMem(group, 1, kModeInd, 4)}, s, "書き 奇数アドレスで脱出");
    }
    // byte は奇数でも書ける。
    {
        M68kState s = makeState(6);
        s.a[4] = kWriteAddr + 1u;
        checkEquivalence({moveDnToMem(0x1u, 1, kModeInd, 4)}, s, "byte は奇数でも書ける");
    }
}

TEST_CASE("自ページへの書きは書く前に脱出する")
{
    clearSeeds();
    // G13: 焼いた定数 (ops / 出口の ir/irc) が実行中に古くならないよう、
    // **自ブロックのページへは 1 バイトも書かない。**
    //
    // kEntry は 0x2000 なのでページは 8。そのページのどこかを指す。
    const u32 selfPageAddr = (kEntry & ~0x3FFu) + 0x100u;

    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        for (u32 mode : {kModeInd, kModePostInc, kModePreDec})
        {
            const u32 size = group == 0x1u ? 1u : (group == 0x2u ? 4u : 2u);
            const std::vector<u16> words{moveDnToMem(group, 1, mode, 4)};
            M68kState s = makeState(6);
            s.a[4] = mode == kModePreDec ? selfPageAddr + size : selfPageAddr;
            // **状態・メモリ・世代の 3 つとも「1 命令も実行していない」と
            // 一致すること。** checkEquivalence が refCount = ranOps = 0 で
            // 参照側を回すので、初期状態そのものと比べる形になる。
            checkEquivalence(words, s, "自ページ書きで脱出");

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            resetExecRam(words);
            resetExecGen();
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("group=", group, " mode=", mode);
            CHECK(native.guardExit);
            // **bit23 が立つこと** (G18)。runner がこれを見て負のキャッシュへ
            // 「世代不問」を焼き、再翻訳の嵐を止める。
            CHECK(native.selfPageExit);
            CHECK(native.ranOps == 0);
        }
    }
}

TEST_CASE("ページ境界を跨ぐ .l の書きは両端を見る")
{
    clearSeeds();
    // G13: write32 は touch(a) と touch(a+3) の **2 ページ**に触る。
    // 自ページ判定を片方だけにすると、境界を跨いだ長語書きで自ページの
    // 端 (先頭 3 バイトか末尾 1 バイト) を黙って書く。
    const u32 selfPageBase = kEntry & ~0x3FFu;

    // (a) 手前のページから書き始めて、末尾 1 バイトが自ページへ食い込む。
    //     page(a) != plan.page だが page(a+3) == plan.page。
    {
        M68kState s = makeState(6);
        s.a[4] = selfPageBase - 2u;
        const std::vector<u16> words{moveDnToMem(0x2u, 1, kModeInd, 4)};
        checkEquivalence(words, s, ".l が自ページの先頭へ食い込む");

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        CHECK(native.guardExit);
        CHECK(native.selfPageExit);
        CHECK(native.ranOps == 0);
    }

    // (b) 自ページの末尾から書き始めて、次のページへ食い込む。
    //     page(a) == plan.page。**片方だけ見る変異でも捕まる側。**
    {
        M68kState s = makeState(6);
        s.a[4] = selfPageBase + 0x400u - 2u;
        const std::vector<u16> words{moveDnToMem(0x2u, 1, kModeInd, 4)};
        checkEquivalence(words, s, ".l が自ページの末尾から跨ぐ");

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        CHECK(native.guardExit);
        CHECK(native.selfPageExit);
        CHECK(native.ranOps == 0);
    }
}

TEST_CASE("ページ境界を跨ぐ .l は 2 ページの世代を上げる")
{
    clearSeeds();
    // G14: write32 は touch(a) と touch(a+3) を無条件に 2 回呼ぶ
    // (m68k.cpp:377-378)。**同一ページでも 2 回、跨げば 2 ページ。**
    //
    // 自ページから遠い境界を選ぶ。
    const u32 crossing = 0x00080000u - 2u;  // page 511 の末尾 2 バイト
    M68kState s = makeState(6);
    s.a[4] = crossing;
    checkEquivalence({moveDnToMem(0x2u, 1, kModeInd, 4)}, s, ".l がページを跨ぐ");

    // 世代が両ページとも動いたことを、参照側の値として明示的に問う。
    const u32 firstPage = crossing >> x68k::CodeGenMap::kPageShift;
    INFO("page ", firstPage, " / ", firstPage + 1);
    CHECK(execGen()[firstPage] == 1);
    CHECK(execGen()[firstPage + 1] == 1);
}

TEST_CASE("同一ページの .l は世代を 2 つ進める")
{
    clearSeeds();
    // **G14 の核心。** write32 は同一ページでも touch を 2 回呼ぶので、
    // 世代は 2 つ進む。「同一ページなら 1 回に畳む」変異はここで落ちる。
    //
    // 状態だけを比べるテストでは**永遠に見えない** (世代は状態に影響しない)。
    M68kState s = makeState(6);
    s.a[4] = kWriteAddr;
    checkEquivalence({moveDnToMem(0x2u, 1, kModeInd, 4)}, s, "同一ページの .l");

    const u32 page = kWriteAddr >> x68k::CodeGenMap::kPageShift;
    INFO("page ", page);
    CHECK(execGen()[page] == 2);

    // byte / word は 1 回だけ。
    for (u32 group : {0x1u, 0x3u})
    {
        M68kState t = makeState(6);
        t.a[4] = kWriteAddr;
        checkEquivalence({moveDnToMem(group, 1, kModeInd, 4)}, t, "同一ページの .b/.w");
        CHECK(execGen()[page] == 1);
    }
}

TEST_CASE("世代の飽和がインタプリタと一致する")
{
    clearSeeds();
    // G14: touch は飽和つき。kAlwaysStale (0xFFFF) に達したら据え置く。
    //
    // **`.l` の 2 回が効く境界を踏む。** 0xFFFD から `.l` を 1 回書くと
    // インタプリタは 0xFFFF (飽和して止まる)、畳んだ変異体は 0xFFFE。
    const u32 page = kWriteAddr >> x68k::CodeGenMap::kPageShift;

    struct Case
    {
        std::uint16_t before;
        const char* what;
    };
    for (const Case& c : {Case{0xFFFCu, "飽和の 3 手前"}, Case{0xFFFDu, "飽和の 2 手前"},
                          Case{0xFFFEu, "飽和の 1 手前"}, Case{0xFFFFu, "飽和済み"}})
    {
        for (u32 group : {0x1u, 0x2u, 0x3u})
        {
            M68kState s = makeState(6);
            s.a[4] = kWriteAddr;
            checkEquivalence({moveDnToMem(group, 1, kModeInd, 4)}, s, c.what, fakeEnv(), page,
                             c.before);
        }
    }

    // 飽和済み (0xFFFF) は据え置き。**インクリメントすると 0 へ回り、
    // 書き換えられたページが「変わっていない」と判定される。**
    M68kState s = makeState(6);
    s.a[4] = kWriteAddr;
    checkEquivalence({moveDnToMem(0x2u, 1, kModeInd, 4)}, s, "飽和済みは据え置き", fakeEnv(), page,
                     0xFFFFu);
    CHECK(execGen()[page] == 0xFFFFu);
}

TEST_CASE("書き形とほかの形を混ぜたブロック")
{
    clearSeeds();
    // 複数命令のブロックで、書きが途中に入っても前後の状態がそろうこと。
    {
        M68kState s = makeState(7);
        s.a[2] = kWriteAddr;
        checkEquivalence({moveq(1, 0x42), moveDnToMem(0x2u, 1, kModeInd, 2), tstDn(2, 1)}, s,
                         "MOVEQ → 書き → TST");
    }
    // 書きが 2 つ並ぶ形。**2 つめの touch が 1 つめを踏み潰さないこと。**
    {
        M68kState s = makeState(7);
        s.a[2] = kWriteAddr;
        s.a[3] = kWriteAddr + 0x800u;  // 別のページ
        checkEquivalence({moveDnToMem(0x2u, 1, kModeInd, 2), moveDnToMem(0x2u, 2, kModeInd, 3)}, s,
                         "書き 2 つ (別ページ)");
    }
    // 同じページへ 2 回。世代が 4 進む (`.l` x 2)。
    {
        M68kState s = makeState(7);
        s.a[2] = kWriteAddr;
        s.a[3] = kWriteAddr + 8u;
        checkEquivalence({moveDnToMem(0x2u, 1, kModeInd, 2), moveDnToMem(0x2u, 2, kModeInd, 3)}, s,
                         "書き 2 つ (同じページ)");
        CHECK(execGen()[kWriteAddr >> x68k::CodeGenMap::kPageShift] == 4);
    }
    // 読みと書きを混ぜる。
    {
        seedData(kWriteAddr + 0x100u, 0xAA, 0xBB, 0xCC, 0xDD);
        M68kState s = makeState(7);
        s.a[2] = kWriteAddr + 0x100u;
        s.a[3] = kWriteAddr;
        checkEquivalence({moveMemToDn(0x2u, 1, kModeInd, 2), moveDnToMem(0x2u, 1, kModeInd, 3)}, s,
                         "読み → 書き");
        clearSeeds();
    }
    // 書きの後ろに分岐。
    {
        M68kState s = makeState(7);
        s.a[2] = kWriteAddr;
        checkEquivalence({moveDnToMem(0x3u, 1, kModeInd, 2), bcc(0x6u, 4)}, s, "書き → BNE");
    }
    // **2 命令目の書きで脱出する形。** 1 命令ぶんだけ進んだ境界状態。
    {
        M68kState s = makeState(8);
        s.a[4] = static_cast<u32>(x68k::kMainRamSize) + 0x2000u;
        checkEquivalence({moveq(0, 1), moveDnToMem(0x3u, 1, kModeInd, 4)}, s,
                         "2 命令目の書きで脱出");
    }
}

TEST_CASE("上位バイト付きアドレスへの書きは 24bit にマスクされる")
{
    clearSeeds();
    // インタプリタは addr & 0x00FFFFFF で書く (m68k.cpp の write8/16/32)。
    // マスクを落とすと窓の外と判定して偽の脱出になる。
    M68kState s = makeState(7);
    s.a[2] = 0xFF000000u | kWriteAddr;
    checkEquivalence({moveDnToMem(0x2u, 1, kModeInd, 2)}, s, "上位バイト付き 書き (An)");

    // (An)+ なら a[] に**マスクしていない**値 + step が入る。
    M68kState t = makeState(7);
    t.a[2] = 0xFF000000u | kWriteAddr;
    checkEquivalence({moveDnToMem(0x2u, 1, kModePostInc, 2)}, t, "上位バイト付き 書き (An)+");
}

TEST_CASE("-(An) が 0 からラップする書きもインタプリタと一致する")
{
    clearSeeds();
    // a[n] == 0 で -(An) すると 32bit で 0xFFFFFFFC へ回り込む。
    // マスク後は 0x00FFFFFC で、2MB の窓の外なのでガードは不成立。
    M68kState s = makeState(5);
    s.a[3] = 0;
    checkEquivalence({moveDnToMem(0x2u, 1, kModePreDec, 3)}, s, "書き -(An) が 0 からラップ");
}

TEST_CASE("世代配列が無い窓では書き形を発行しない")
{
    clearSeeds();
    // G19: 世代配列が無いと touch を再現できない。窓が短すぎて
    // 「範囲ガード成立 ⇒ ページ番号が配列の中」が導けない場合も同じ。
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({moveDnToMem(0x2u, 1, kModeInd, 2)}, plan, code));
    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);

    CHECK(jit::requiredSize(plan, ir, irc, fakeEnv()) > 0);

    // 世代配列が無い。
    jit::EmitEnv noGen = fakeEnv();
    noGen.genBaseAddr = 0;
    CHECK(jit::requiredSize(plan, ir, irc, noGen) == 0);
    CHECK_FALSE(jit::canEmitWritesIn(noGen));

    // ページ数が足りない。**窓の端の書きが世代配列の外へ s16i する。**
    jit::EmitEnv shortGen = fakeEnv();
    shortGen.genPageCount = kGenPages - 1;
    CHECK(jit::requiredSize(plan, ir, irc, shortGen) == 0);
    CHECK_FALSE(jit::canEmitWritesIn(shortGen));

    // ちょうど足りるなら発行できる。**境界を明示的に問う** (>= を > に
    // する変異はここでだけ落ちる)。
    jit::EmitEnv exact = fakeEnv();
    exact.genPageCount = kGenPages;
    CHECK(jit::canEmitWritesIn(exact));

    // 窓が無い。
    jit::EmitEnv noWindow = fakeEnv();
    noWindow.ramBaseAddr = 0;
    CHECK(jit::requiredSize(plan, ir, irc, noWindow) == 0);

    // **窓が読めなくても書き形は焼ける。** 書き経路は fastRamReadable_ を
    // 見ない (m68k.cpp:331-334 — ROM 写像中も RAM へは書ける)。
    jit::EmitEnv unreadable = fakeEnv();
    unreadable.ramReadable = false;
    CHECK(jit::requiredSize(plan, ir, irc, unreadable) > 0);
}

TEST_CASE("窓が読めない写像では CLR <mem> を発行しない")
{
    clearSeeds();
    // G20: CLR は読み脚を省いている。省略の前提は「ガードが成立する範囲では
    // read8/16/32 の fast path に副作用が無い」ことで、それは
    // fastRamReadable_ が立っているときだけ成り立つ。
    //
    // **MOVE Dn,<mem> は読まないので、この制約を負わない。**
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({clrMem(2u, kModeInd, 2)}, plan, code));
    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);

    CHECK(jit::requiredSize(plan, ir, irc, fakeEnv()) > 0);

    jit::EmitEnv unreadable = fakeEnv();
    unreadable.ramReadable = false;
    CHECK(jit::requiredSize(plan, ir, irc, unreadable) == 0);
}

TEST_CASE("自ページを指す絶対アドレスの書きは翻訳時に弾く")
{
    clearSeeds();
    // G17 (a): 実効アドレスが翻訳時に決まっているので、積んでも
    // 「必ず自ページ脱出するブロック」にしかならない。しかも脱出は
    // runner に負のキャッシュを焼かせるので、積むとその番地の JIT を失う。
    const u32 selfPageAddr = (kEntry & ~0x3FFu) + 0x100u;

    // **翻訳器が手前で終端すること。** 積んだうえでエミッタが断ると
    // ブロックが丸ごと失われる (Tier B で踏んだ形)。
    FlatCode code;
    BlockPlan plan{};
    const bool planned =
        buildPlan({moveq(1, 3), moveDnToMem(0x2u, 1, kModeAbsL, 1),
                   static_cast<u16>(selfPageAddr >> 16), static_cast<u16>(selfPageAddr & 0xFFFFu)},
                  plan, code);
    REQUIRE(planned);
    // MOVEQ だけが積まれ、書きの手前で終端する。
    CHECK(plan.count == 1);
    CHECK(plan.end == BlockEnd::kUnsupported);

    // ページ境界を跨いで自ページへ食い込む `.l` も同じ。
    FlatCode crossCode;
    BlockPlan crossPlan{};
    const u32 crossAddr = (kEntry & ~0x3FFu) - 3u;
    REQUIRE(buildPlan({moveq(1, 3), moveDnToMem(0x2u, 1, kModeAbsL, 1),
                       static_cast<u16>(crossAddr >> 16), static_cast<u16>(crossAddr & 0xFFFFu)},
                      crossPlan, crossCode));
    CHECK(crossPlan.count == 1);

    // 自ページから外れていれば積む。
    FlatCode okCode;
    BlockPlan okPlan{};
    REQUIRE(buildPlan({moveq(1, 3), moveDnToMem(0x2u, 1, kModeAbsL, 1),
                       static_cast<u16>(kWriteAddr >> 16), static_cast<u16>(kWriteAddr & 0xFFFFu)},
                      okPlan, okCode));
    CHECK(okPlan.count == 2);
}

TEST_CASE("窓の外を指す絶対アドレスの書きは翻訳時に弾く")
{
    clearSeeds();
    // G17 (a) の残り半分。範囲と整列を翻訳時に判定する。
    const u32 outside = static_cast<u32>(x68k::kMainRamSize) + 0x1000u;
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({moveDnToMem(0x2u, 1, kModeAbsL, 1), static_cast<u16>(outside >> 16),
                       static_cast<u16>(outside & 0xFFFFu)},
                      plan, code));
    CHECK(jit::requiredSize(plan, code.get16(plan.fallThroughPc),
                            code.get16(plan.fallThroughPc + 2), fakeEnv()) == 0);

    // 奇数の絶対アドレス (word 以上)。
    FlatCode oddCode;
    BlockPlan oddPlan{};
    const u32 odd = kWriteAddr + 1u;
    REQUIRE(buildPlan({moveDnToMem(0x2u, 1, kModeAbsL, 1), static_cast<u16>(odd >> 16),
                       static_cast<u16>(odd & 0xFFFFu)},
                      oddPlan, oddCode));
    CHECK(jit::requiredSize(oddPlan, oddCode.get16(oddPlan.fallThroughPc),
                            oddCode.get16(oddPlan.fallThroughPc + 2), fakeEnv()) == 0);
}

TEST_CASE("絶対アドレスの書きにも実行時ガードが吐かれる")
{
    clearSeeds();
    // G17 (b): 翻訳時判定の変異は「ガード脱出が増える」という観測可能な
    // 形でしか現れず、**ホストメモリ破壊としては現れない**。
    //
    // 翻訳時判定を通した絶対形に、実行時ガードが実在することを問う。
    // 翻訳時の窓 (2MB) より実行時の窓を狭くして走らせると、
    // 生成コードのガードが不成立になって脱出する。畳んで消していれば
    // 脱出せず、窓の外を書いてしまう。
    const std::vector<u16> words{moveDnToMem(0x2u, 1, kModeAbsL, 1),
                                 static_cast<u16>(kWriteAddr >> 16),
                                 static_cast<u16>(kWriteAddr & 0xFFFFu)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));

    // **翻訳時は広い窓**で焼く。
    jit::EmitEnv wide = fakeEnv();
    const EmitResult e = emit(plan, code, wide);
    REQUIRE(e.ok);

    // バイト列に beqz か bnez (ガードの分岐) が在ること。
    // 畳んで消す実装ではどちらも出ない。
    bool hasBranch = false;
    for (std::size_t i = e.info.entryOffset; i + 3 <= e.buffer.size(); ++i)
    {
        const std::uint32_t op0 = e.buffer[i] & 0x0Fu;
        const std::uint32_t sub = (e.buffer[i] >> 4) & 0x0Fu;
        if (op0 == 0x6u && (sub == 0x1u || sub == 0x5u))
        {
            hasBranch = true;
            break;
        }
    }
    INFO("絶対形の書きにガードの分岐が無い");
    CHECK(hasBranch);

    // 走らせて、正しい番地へ書けること (ガードは成立側)。
    M68kState s = makeState(3);
    checkEquivalence(words, s, "絶対形の書き (ガード成立)");
}

namespace
{

// 発行されたバイト列を**命令の切れ目で**なめる。
//
// Why not 1 バイトずつずらして探さないか: 3 バイト命令の途中のバイトが、
// 別の命令の先頭に見えることがある。実際、書き形はコードが長いので
// 「l32r のリテラル変位の下位バイトが s16i の符号に見える」形を踏んだ。
// 誤検出は**テストが赤になる**という安全側の失敗だが、赤の理由が
// 「そこに無いものが在る」なので追いようがない。
//
// 命令長は先頭バイトの下位 4bit で決まる (Xtensa の density option:
// op0 >= 8 が 16bit 形)。エミッタが吐くのは 2 バイト形と 3 バイト形だけ。
struct XInsn
{
    std::size_t at = 0;
    std::size_t len = 0;
    std::uint32_t op0 = 0;
    std::uint32_t t = 0;
    std::uint32_t s = 0;
    std::uint32_t r = 0;
};

std::vector<XInsn> decodeStream(const std::vector<std::uint8_t>& buf, std::size_t entry,
                                std::size_t end)
{
    std::vector<XInsn> out;
    std::size_t at = entry;
    while (at + 2 <= end)
    {
        XInsn insn{};
        insn.at = at;
        insn.op0 = buf[at] & 0x0Fu;
        insn.len = insn.op0 >= 0x8u ? 2u : 3u;
        if (at + insn.len > end)
        {
            break;
        }
        insn.t = (buf[at] >> 4) & 0x0Fu;
        insn.s = buf[at + 1] & 0x0Fu;
        insn.r = (buf[at + 1] >> 4) & 0x0Fu;
        out.push_back(insn);
        at += insn.len;
    }
    return out;
}

// ガードの分岐か (beqz / bnez)。BRI12 は op0 = 0x6 / t = 0x1 か 0x5。
bool isGuardBranch(const XInsn& insn)
{
    return insn.op0 == 0x6u && (insn.t == 0x1u || insn.t == 0x5u);
}

// M68kState (a3 基底) へのストアか。
bool isStateStore(const XInsn& insn)
{
    const bool wide = insn.op0 == 0x2u && insn.s == 3u && (insn.r == 0x6u || insn.r == 0x5u);
    const bool narrow = insn.op0 == 0x9u && insn.s == 3u;
    return wide || narrow;
}

// ゲスト RAM へのストアか (s8i)。基底は問わない。
bool isGuestStore(const XInsn& insn)
{
    return insn.op0 == 0x2u && insn.r == 0x4u;
}

// 世代配列へのストアか (a3 基底でない s16i)。
bool isGenStore(const XInsn& insn)
{
    return insn.op0 == 0x2u && insn.r == 0x5u && insn.s != 3u;
}

}  // namespace

TEST_CASE("書きガードより前に状態を書く命令もメモリを書く命令も無い")
{
    clearSeeds();
    // G3 / G16 の機械検査。**バイト列を走査して確かめる。**
    //
    // 生成コードが触ってよくないのは 2 つ:
    //   kState (a3) 基底の s32i / s16i / s32i.n — アーキテクチャ状態
    //   s8i / s16i (基底を問わない)             — ゲスト RAM と世代配列
    //
    // どちらも**最後のガード分岐より後**にしか現れてはいけない。
    // 「最初の分岐まで」ではなく「最後の分岐まで」を見るのが要点で、
    // 自ページ判定 (2 本目以降) の前に touch を出す変異を捕まえる。
    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        for (u32 mode : {kModeInd, kModePostInc, kModePreDec, kModeAbsL})
        {
            std::vector<u16> words{moveDnToMem(group, 1, mode, mode == kModeAbsL ? 1u : 2u)};
            if (mode == kModeAbsL)
            {
                words.push_back(static_cast<u16>(kWriteAddr >> 16));
                words.push_back(static_cast<u16>(kWriteAddr & 0xFFFFu));
            }
            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            INFO("group=", group, " mode=", mode);
            REQUIRE(e.ok);

            const std::vector<XInsn> stream =
                decodeStream(e.buffer, e.info.entryOffset, e.buffer.size());

            // **最後の**ガード分岐 (beqz / bnez) を探す。
            //
            // 島には分岐が無い (島は l32r / movi / s32i / s16i / ret.n だけ)
            // ので、本体の最後のガードがそのまま最後の分岐になる。
            std::size_t lastGuard = 0;
            bool found = false;
            for (const XInsn& insn : stream)
            {
                if (isGuardBranch(insn))
                {
                    lastGuard = insn.at;
                    found = true;
                }
            }
            REQUIRE(found);

            bool wroteState = false;
            bool wroteMemory = false;
            for (const XInsn& insn : stream)
            {
                if (insn.at >= lastGuard)
                {
                    break;
                }
                if (isStateStore(insn))
                {
                    wroteState = true;
                }
                if (isGuestStore(insn) || isGenStore(insn))
                {
                    wroteMemory = true;
                }
            }
            CHECK_FALSE(wroteState);
            CHECK_FALSE(wroteMemory);
        }
    }
}

TEST_CASE("世代への書きはゲスト RAM への書きより前に出る")
{
    clearSeeds();
    // G16: touch はゲスト RAM の store より**前**。
    //
    // 逆順でも最終状態は同じなので、同値テストでは見えない。
    // バイト列の並びとしてだけ固定できる。
    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        const std::vector<u16> words{moveDnToMem(group, 1, kModeInd, 2)};
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        INFO("group=", group);
        REQUIRE(e.ok);

        // 最初の s8i (ゲスト RAM) と、最後の s16i (世代配列; a3 基底ではない) の位置。
        const std::vector<XInsn> stream =
            decodeStream(e.buffer, e.info.entryOffset, e.buffer.size());
        std::size_t firstGuestStore = e.buffer.size();
        std::size_t lastGenStore = 0;
        bool sawGen = false;
        for (const XInsn& insn : stream)
        {
            if (isGuestStore(insn) && firstGuestStore == e.buffer.size())
            {
                firstGuestStore = insn.at;
            }
            if (isGenStore(insn))
            {
                lastGenStore = insn.at;
                sawGen = true;
            }
        }
        REQUIRE(firstGuestStore < e.buffer.size());
        REQUIRE(sawGen);
        CHECK(lastGenStore < firstGuestStore);
    }
}

TEST_CASE("書き形いっぱいのブロックがリテラルを使い切らない")
{
    clearSeeds();
    // kMaxLiterals (56) の余裕を、**最も定数を食う形**で確かめる。
    //
    // 溢れると発行を諦めるだけなので正しさは損なわれないが、
    // 諦めた分は素通りするので気づきにくい。
    const std::vector<u16> words{clrMem(2u, kModeInd, 2), clrMem(2u, kModeInd, 3),
                                 clrMem(2u, kModeInd, 4), clrMem(2u, kModeInd, 5)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    REQUIRE(plan.count == x68k::kMaxOps);
    const std::size_t need = jit::requiredSize(plan, code.get16(plan.fallThroughPc),
                                               code.get16(plan.fallThroughPc + 2), fakeEnv());
    INFO("need=", need);
    CHECK(need > 0);
    // BlockRunner::kStagingBytes (1536) に収まること。**収まらないと
    // translate が黙って諦める。**
    CHECK(need <= 1536);

    // 走らせても一致すること。
    M68kState s = makeState(2);
    s.a[2] = kWriteAddr;
    s.a[3] = kWriteAddr + 0x10u;
    s.a[4] = kWriteAddr + 0x20u;
    s.a[5] = kWriteAddr + 0x30u;
    checkEquivalence(words, s, "CLR.l x 4");
}

// staging に収まることを **全 EA モード** で問う。
//
// **1 つの形しか測らないと、最悪ケースを見落とす。** 既存のテストは
// CLR.l (An) だけを測っていたが、実際の最悪は -(An) / (An)+ で、
// 旧上限 1024 に対して余裕が 10 バイトを切っていた。
//
// 溢れたときの症状は例外でも赤いテストでもなく、**translate が黙って
// 諦める**こと。被覆がじわじわ落ちるだけなので、原因に辿り着けない。
// だから上限そのものを検査対象にする。
TEST_CASE("どの EA モードでも staging に収まる")
{
    struct ModeCase
    {
        std::uint8_t mode;
        const char* name;
    };
    const ModeCase modes[] = {
        {kModeInd, "(An)"},
        {kModePostInc, "(An)+"},
        {kModePreDec, "-(An)"},
        {kModeDisp, "(d16,An)"},
    };

    std::size_t worst = 0;
    const char* worstName = "";
    for (const ModeCase& m : modes)
    {
        for (const std::uint8_t size : {1u, 2u, 4u})
        {
            for (const bool isClr : {false, true})
            {
                BlockPlan plan{};
                plan.entryPc = kEntry;
                plan.count = static_cast<std::uint8_t>(x68k::kMaxOps);
                plan.page = kEntry >> 10;
                plan.end = BlockEnd::kUnsupported;
                plan.fallThroughPc = kEntry + x68k::kMaxOps * 8u;
                for (std::uint32_t i = 0; i < x68k::kMaxOps; ++i)
                {
                    PlannedOp& op = plan.ops[i];
                    op.pc = kEntry + i * 8u;
                    op.length = 2;
                    op.kind = isClr ? PlanKind::kClrMem : PlanKind::kMoveDregToMem;
                    op.size = static_cast<std::uint8_t>(size);
                    op.cycles = isClr ? 6 : 4;
                    op.eaMode = m.mode;
                    op.srcReg = 1;
                    op.dstReg = 2;
                }
                const std::size_t need = jit::requiredSize(plan, 0x4E71, 0x4E71, fakeEnv());
                if (need > worst)
                {
                    worst = need;
                    worstName = m.name;
                }
            }
        }
    }

    INFO("最悪 ", worst, " バイト (", worstName, ")");
    CHECK(worst > 0);
    CHECK(worst <= 1536);
    // **余裕が 20% を切ったら気づけるようにする。** 命令やガードを 1 つ
    // 足しただけで崖に戻るので、上限ぎりぎりで通っている状態を許さない。
    CHECK(worst <= 1536 * 4 / 5);
}

// Tier G / H を**最も重い形で 4 つ並べた**ブロックが kMaxBlockBytes に収まる。
//
// 段 0-E で `requiredSize` が上限を超えると emitBlock が黙って false を返し、
// **正しさは無傷のまま、その段が狙う形そのものを落とす**ことを踏んでいる
// (docs/knowledge/event-driven-implementation.md「staging が黙って翻訳を
// 落としていた」)。落ちたことは被覆率にしか現れないので、上限は
// 「最悪ケースを実際に組んで測る」形でしか守れない。
//
// 落ちる変異: kMaxBlockBytes を 1024 へ下げる / emitAluImm を l32r 固定にする
TEST_CASE("Tier G / H を 4 つ並べたブロックが kMaxBlockBytes に収まる")
{
    struct WorstCase
    {
        PlanKind kind;
        std::uint8_t eaMode;
        std::uint8_t size;
        PlanAluOp aluOp;
        const char* name;
    };
    // 定数を最も食う形を選ぶ。
    //   ALU #imm : .l の CMP (即値が l32r へ落ちる + フラグ 5 本ぶん)
    //   MOVEA    : (d16,An) の .l (EA 定数 + ガード + commit)
    //   CMPA     : .w (符号拡張 4 命令 + フラグ全部)
    const WorstCase cases[] = {
        {PlanKind::kAluImmToDreg, x68k::kEaNone, 4, PlanAluOp::kCmp, "CMP.l #imm,Dn"},
        {PlanKind::kAluImmToDreg, x68k::kEaNone, 4, PlanAluOp::kAdd, "ADD.l #imm,Dn"},
        {PlanKind::kMoveaMemToAreg, x68k::kEaDisp16, 4, PlanAluOp::kAdd, "MOVEA.l (d16,An),An"},
        {PlanKind::kMoveaMemToAreg, x68k::kEaDisp16, 2, PlanAluOp::kAdd, "MOVEA.w (d16,An),An"},
        {PlanKind::kCmpaDregToAreg, x68k::kEaNone, 2, PlanAluOp::kCmp, "CMPA.w Dn,An"},
        {PlanKind::kCmpaAregToAreg, x68k::kEaNone, 2, PlanAluOp::kCmp, "CMPA.w An,An"},
        {PlanKind::kAddaDregToAreg, x68k::kEaNone, 2, PlanAluOp::kAdd, "ADDA.w Dn,An"},
        {PlanKind::kAddaAregToAreg, x68k::kEaNone, 2, PlanAluOp::kSub, "SUBA.w An,An"},
        // --- Tier H ---
        //
        // **CMPI と ADDQ の Dn 宛ては kAluImmToDreg** なので上の 2 つが
        // 既に測っている。ここに足すのは Tier H 固有の 2 つ。
        //   ADDQ #imm,An : 定数 1 本 + load/add/store
        //   BTST #imm,Dn : 定数 2 本 (マスクと kCcrZ) + CCR の読み書き
        {PlanKind::kAddqImmToAreg, x68k::kEaNone, 4, PlanAluOp::kAdd, "ADDQ.l #imm,An"},
        {PlanKind::kAddqImmToAreg, x68k::kEaNone, 2, PlanAluOp::kSub, "SUBQ.w #imm,An"},
        {PlanKind::kBtstImmToDreg, x68k::kEaNone, 4, PlanAluOp::kAdd, "BTST #imm,Dn"},
    };

    std::size_t worst = 0;
    const char* worstName = "";
    for (const WorstCase& c : cases)
    {
        BlockPlan plan{};
        plan.entryPc = kEntry;
        plan.count = static_cast<std::uint8_t>(x68k::kMaxOps);
        plan.page = kEntry >> 10;
        plan.end = BlockEnd::kUnsupported;
        plan.fallThroughPc = kEntry + x68k::kMaxOps * 8u;
        for (std::uint32_t i = 0; i < x68k::kMaxOps; ++i)
        {
            PlannedOp& op = plan.ops[i];
            op.pc = kEntry + i * 8u;
            op.length = 6;
            op.kind = c.kind;
            op.eaMode = c.eaMode;
            op.size = c.size;
            op.aluOp = c.aluOp;
            op.cycles = 8;
            op.srcReg = 1;
            op.dstReg = 2;
            // **リテラルを共有させない。** 命令ごとに違う定数にしておかないと、
            // literalIndex が畳んで最悪ケースにならない。
            //
            // **BTST だけは imm がビット番号 (0-31)。** planner が 31 で
            // マスクした値しか入らない欄なので、ここでも契約を守る。
            // 大きい値を入れると 1u << imm が未定義になり、測っているのが
            // 「起こりえない形」になる。ビット 24 以上ならマスク定数が
            // 16bit に収まらないので、l32r へ落ちる側 (= 重い側) を選ぶ。
            const bool immIsBitNumber = c.kind == PlanKind::kBtstImmToDreg;
            op.imm = immIsBitNumber ? (24u + i) : (0x00123456u + i * 0x1111u);
        }
        const std::size_t need = jit::requiredSize(plan, 0x4E71, 0x4E71, fakeEnv());
        INFO(std::string(c.name), " = ", need, " バイト");
        CHECK(need > 0);
        CHECK(need <= x68k::jit::kMaxBlockBytes);
        if (need > worst)
        {
            worst = need;
            worstName = c.name;
        }
    }

    INFO("最悪 ", worst, " バイト (", std::string(worstName), ")");
    // **余裕が 20% を切ったら気づけるようにする。** 命令やガードを 1 つ
    // 足しただけで崖に戻るので、上限ぎりぎりで通っている状態を許さない。
    CHECK(worst * 5u <= x68k::jit::kMaxBlockBytes * 4u);

    // **系統を混ぜた形も測る。** リテラルは種類ごとに違う定数を食うので、
    // 同じ形を 4 つ並べるより混ぜた方が l32r の共有が効かず重くなりうる。
    {
        BlockPlan plan{};
        plan.entryPc = kEntry;
        plan.count = static_cast<std::uint8_t>(x68k::kMaxOps);
        plan.page = kEntry >> 10;
        plan.end = BlockEnd::kUnsupported;
        plan.fallThroughPc = kEntry + x68k::kMaxOps * 8u;
        const WorstCase mix[] = {cases[2], cases[0], cases[4], cases[3]};
        for (std::uint32_t i = 0; i < x68k::kMaxOps; ++i)
        {
            PlannedOp& op = plan.ops[i];
            op.pc = kEntry + i * 8u;
            op.length = 6;
            op.kind = mix[i].kind;
            op.eaMode = mix[i].eaMode;
            op.size = mix[i].size;
            op.aluOp = mix[i].aluOp;
            op.cycles = 8;
            op.srcReg = 1;
            op.dstReg = 2;
            op.imm = 0x00654321u + i * 0x2222u;
        }
        const std::size_t need = jit::requiredSize(plan, 0x4E71, 0x4E71, fakeEnv());
        INFO("混成 = ", need, " バイト");
        CHECK(need > 0);
        CHECK(need * 5u <= x68k::jit::kMaxBlockBytes * 4u);
    }
}

TEST_CASE("書きガード脱出の irc が実メモリの mem16(opPc + 2) と一致する")
{
    clearSeeds();
    // I11 の書き形版。**長さ 6 ((xxx).L) がここで初めて島を作る。**
    //
    // 読み側は絶対形のガードを翻訳時に畳んで消しているので長さ 6 の島が
    // 無いが、書き側は G17 (b) で実行時ガードを残すので在る。
    // 導出を間違えると、その形が脱出したときにしか現れない。
    const u32 outside = static_cast<u32>(x68k::kMainRamSize) + 0x1000u;
    const u32 selfPageAddr = (kEntry & ~0x3FFu) + 0x100u;

    struct Case
    {
        std::vector<u16> words;
        u32 aReg;
        u32 aValue;
        const char* what;
    };
    const std::vector<Case> cases{
        // 長さ 2: opPc + 2 は次の命令語
        {{moveDnToMem(0x2u, 1, kModeInd, 4)}, 4, outside, "Dn,(An) 単独"},
        {{moveDnToMem(0x2u, 1, kModeInd, 4), moveq(5, 0x42)}, 4, outside, "Dn,(An) の後ろに MOVEQ"},
        {{moveq(0, 3), moveDnToMem(0x2u, 1, kModePostInc, 4), moveq(6, 0x21)},
         4,
         outside,
         "2 命令目が Dn,(An)+"},
        // 長さ 4: opPc + 2 は第 1 拡張ワード
        {{moveDnToMem(0x2u, 1, kModeDisp, 4), 0x1234u}, 4, outside, "Dn,(d16,An)"},
        {{moveDnToMem(0x2u, 1, kModeDisp, 4), 0x1234u, moveq(7, 0x33)},
         4,
         outside,
         "Dn,(d16,An) の後ろに MOVEQ"},
        // CLR も同じ導出を通る。
        {{clrMem(2u, kModeInd, 4)}, 4, outside, "CLR (An)"},
    };

    for (const Case& c : cases)
    {
        M68kState s = makeState(11);
        s.a[c.aReg] = c.aValue;
        checkEquivalence(c.words, s, c.what);

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(c.words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        INFO(std::string(c.what));
        CHECK(native.guardExit);
    }

    // **長さ 6 ((xxx).L)。** 自ページ脱出で島へ降りる形を作る。
    //
    // 絶対形は翻訳時に自ページを弾く (G17 (a)) ので、自ページでは
    // 積まれない。代わりに**実行時だけ窓を狭める**形で脱出させる…
    // のはミニ解釈器の窓を動かす必要があって別の検証面になるので、
    // ここでは「積まれた (xxx).L が脱出したときの irc が正しい」ことを
    // guardExitIrc の導出そのものとして問う。
    //
    // 実効的には: 窓の端に掛かる (xxx).L を翻訳時に通し、実行時に
    // 同じ窓で走らせると成立するので脱出しない。**脱出させるには
    // 翻訳時と実行時で窓が違う必要があり、それは epoch の鍵が
    // 弾く設計** (G8)。したがって長さ 6 の島は「作られはするが
    // 通常は到達しない」経路になる。ここではバイト列として島が
    // 存在することだけを問う。
    const std::vector<u16> absWords{moveDnToMem(0x2u, 1, kModeAbsL, 1),
                                    static_cast<u16>(kWriteAddr >> 16),
                                    static_cast<u16>(kWriteAddr & 0xFFFFu), moveq(5, 0x11)};
    FlatCode absCode;
    BlockPlan absPlan{};
    REQUIRE(buildPlan(absWords, absPlan, absCode));
    const EmitResult absE = emit(absPlan, absCode);
    REQUIRE(absE.ok);
    // 島の irc は (xxx).L の上位語。s16i でその定数が置かれているはず。
    // **導出が間違っていれば、この語がバイト列のどこにも現れない。**
    M68kState absState = makeState(3);
    checkEquivalence(absWords, absState, "(xxx).L の書き (成立側)");

    (void)selfPageAddr;
}

// --- Tier E: MOVEM ----------------------------------------------------------
//
// 対象は 2 形だけ:
//
//   MOVEM.L (An)+,<regs>   ビット i ↔ D0..A7 (i 昇順)
//   MOVEM.L <regs>,-(An)   ビット i ↔ A7..D0 (i 昇順、アドレスは降順)
//
// 単一転送との決定的な違いは、**1 命令が最大 4 か所へ連続して触る**こと。
// ガード不成立で降りる先はその命令の直前の命令境界しかない (G7) ので、
// 転送を途中まで済ませて降りることは許されない。全事前検査が要る。

namespace
{

// MOVEM.L (An)+,<regs> = $4CD8 | reg。拡張ワードにマスクが続く。
constexpr u16 movemToRegs(u32 reg)
{
    return static_cast<u16>(0x4CD8u | reg);
}

// MOVEM.L <regs>,-(An) = $48E0 | reg。拡張ワードにマスクが続く。
constexpr u16 movemToPreDec(u32 reg)
{
    return static_cast<u16>(0x48E0u | reg);
}

// MOVEM のテストで使うゲストアドレス。**自ブロックのページから遠く**、
// かつ偶数。近いと G13 の脱出が混ざって比較の意味が変わる。
constexpr u32 kMovemAddr = 0x00060000u;

// 連続する len バイトを、**全部違う値**で埋める種を仕込む。
//
// seedData は 4 バイトしか置けない。MOVEM は最大 16 バイトを触るので、
// バイトが 1 つでも入れ替わったら分かるだけの長さが要る。
void seedRun(u32 addr, u32 len, u8 first)
{
    guestSeeds().clear();
    GuestSeed seed;
    seed.addr = addr;
    for (u32 i = 0; i < len; ++i)
    {
        seed.bytes.push_back(static_cast<u8>(first + i * 7u));
    }
    guestSeeds().push_back(seed);
}

// マスクの立っているビット数。
u32 popcount16(u32 mask)
{
    u32 n = 0;
    for (u32 i = 0; i < 16; ++i)
    {
        n += (mask >> i) & 1u;
    }
    return n;
}

}  // namespace

// --- T-E1: 差分テスト -------------------------------------------------------

TEST_CASE("MOVEM.L (An)+,<regs> がインタプリタと一致する")
{
    // **本数もマスクの位置も散らす。** D だけ / A だけ / 混在 / 端 (D0, A7) を
    // 含む形。ビット ↔ レジスタの写像を 1 つでも取り違えたら落ちる。
    const u32 masks[] = {
        0x0001u,  // D0 だけ (1 本、最下位ビット)
        0x8000u,  // A7 だけ (1 本、最上位ビット)
        0x0003u,  // D0,D1
        0x000Fu,  // D0-D3 (4 本、上限)
        0x0F00u,  // A0-A3
        0x8001u,  // D0 と A7 (両端)
        0x1248u,  // 散らばった 4 本
        0xC000u,  // A6,A7
    };

    for (const u32 mask : masks)
    {
        for (const u32 seed : {1u, 3u, 7u})
        {
            const u32 count = popcount16(mask);
            seedRun(kMovemAddr, count * 4u, static_cast<u8>(0x11u + seed));
            M68kState s = makeState(seed);
            s.a[2] = kMovemAddr;
            checkEquivalence({movemToRegs(2), static_cast<u16>(mask)}, s, "MOVEM.L (An)+,<regs>");
        }
    }
}

TEST_CASE("MOVEM.L <regs>,-(An) がインタプリタと一致する")
{
    const u32 masks[] = {
        0x0001u, 0x8000u, 0x0003u, 0x000Fu, 0x0F00u, 0x8001u, 0x1248u, 0xC000u,
    };

    for (const u32 mask : masks)
    {
        for (const u32 seed : {1u, 3u, 7u})
        {
            // 書き先に 0 でないバイトを置く。**書かない変異が緑で通らない**
            // ようにするため。
            const u32 count = popcount16(mask);
            seedRun(kMovemAddr - 64u, 128u, static_cast<u8>(0xA5u + seed));
            M68kState s = makeState(seed);
            s.a[3] = kMovemAddr;
            (void)count;
            checkEquivalence({movemToPreDec(3), static_cast<u16>(mask)}, s, "MOVEM.L <regs>,-(An)");
        }
    }
}

TEST_CASE("MOVEM は CCR を 1 bit も変えない")
{
    // インタプリタの MOVEM は st_.sr にまったく触らない。**sr への任意の
    // 書き込みを入れる変異が、ここで落ちる。**
    //
    // checkEquivalence が sr を含めて全状態を比べるので、CCR の初期値を
    // 散らしておけば「どの向きに壊しても」検出できる。
    for (const u32 seed : {0u, 1u, 2u, 15u, 31u})
    {
        seedRun(kMovemAddr, 16u, 0x31u);
        M68kState s = makeState(4);
        // 下位 5bit (CCR) を総当たりに近い形で散らす。
        s.sr = static_cast<u16>(0x2700u | seed);
        s.a[2] = kMovemAddr;
        checkEquivalence({movemToRegs(2), 0x000Fu}, s, "MOVEM 読み CCR 不変");

        M68kState w = makeState(4);
        w.sr = static_cast<u16>(0x2700u | seed);
        w.a[3] = kMovemAddr;
        checkEquivalence({movemToPreDec(3), 0x000Fu}, w, "MOVEM 書き CCR 不変");
    }
}

// --- T-E2: 書き形のレジスタ順 -----------------------------------------------

TEST_CASE("MOVEM.L <regs>,-(An) のビット i は A7 から数えて i 番目")
{
    // **ここを昇順に取り違えるとスタックフレームが壊れる** (インタプリタの
    // コメントが名指ししている失敗)。checkEquivalence でも捕まるが、
    // メモリ像をバイトで直に照合して「どのレジスタがどこへ出たか」を固定する。
    clearSeeds();

    // 全 16 レジスタへ相異なる値を置く。**上位バイトまで違う**ので、
    // 1 本でも入れ替わればバイト照合が落ちる。
    M68kState s{};
    for (u32 i = 0; i < 8; ++i)
    {
        s.d[i] = 0xD0000000u + i * 0x11111111u;
        s.a[i] = 0xA0000000u + i * 0x11111111u;
    }
    s.sr = 0x2700u;
    s.a[3] = kMovemAddr;

    // **ビット i は A7 から数えて i 番目。** ビット 0,1,8,9 を立てると、
    // 積まれるのは A7, A6, D7, D6 (D0,D1,A0,A1 では**ない**)。
    // ここを取り違えるのがまさに防ぎたい失敗なので、期待値は
    // インタプリタの式 (regIndex = 15 - i) から起こす。
    const u32 mask = 0x0303u;
    const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);
    resetExecRam(words);
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);
    REQUIRE_FALSE(native.guardExit);

    // インタプリタは**ビット昇順でアドレスを減らしながら**置く:
    //   bit 0 (A7) → An-4、bit 1 (A6) → An-8、bit 8 (D7) → An-12、
    //   bit 9 (D6) → An-16。
    // つまり低いアドレスから並べると D6, D7, A6, A7。
    const u32 base = kMovemAddr - 16u;
    const u32 expect[4] = {s.d[6], s.d[7], s.a[6], s.a[7]};
    for (u32 i = 0; i < 4; ++i)
    {
        const u32 at = base + i * 4u;
        // ビッグエンディアンでバイト照合する。
        for (u32 b = 0; b < 4; ++b)
        {
            const u8 want = static_cast<u8>((expect[i] >> ((3u - b) * 8u)) & 0xFFu);
            INFO("i=", i, " b=", b);
            CHECK(execRam()[at + b] == want);
        }
    }
    // An は範囲の先頭まで下がる。
    CHECK(native.state.a[3] == base);
}

TEST_CASE("MOVEM.L (An)+,<regs> のビット i は D0 から数えて i 番目")
{
    // 読み形の写像も同じ流儀で固定する。**降順への反転がここで落ちる。**
    clearSeeds();

    // 4 語ぶんの相異なるパターンを置く。
    GuestSeed seed;
    seed.addr = kMovemAddr;
    const u32 values[4] = {0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u};
    for (const u32 v : values)
    {
        for (u32 b = 0; b < 4; ++b)
        {
            seed.bytes.push_back(static_cast<u8>((v >> ((3u - b) * 8u)) & 0xFFu));
        }
    }
    guestSeeds().clear();
    guestSeeds().push_back(seed);

    M68kState s = makeState(2);
    s.a[2] = kMovemAddr;

    // D1, D3, A0, A5 を読む。ビット: D1=1, D3=3, A0=8, A5=13。
    const u32 mask = (1u << 1) | (1u << 3) | (1u << 8) | (1u << 13);
    const std::vector<u16> words{movemToRegs(2), static_cast<u16>(mask)};

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);
    resetExecRam(words);
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);
    REQUIRE_FALSE(native.guardExit);

    // ビット昇順に、先頭から順に入る。
    CHECK(native.state.d[1] == values[0]);
    CHECK(native.state.d[3] == values[1]);
    CHECK(native.state.a[0] == values[2]);
    CHECK(native.state.a[5] == values[3]);
    // 触っていないレジスタは元のまま。
    CHECK(native.state.d[0] == s.d[0]);
    CHECK(native.state.d[2] == s.d[2]);
    CHECK(native.state.a[1] == s.a[1]);
    // An は読み終わった位置まで進む。
    CHECK(native.state.a[2] == kMovemAddr + 16u);
}

// --- T-E3: base がマスクに入っている形 --------------------------------------

TEST_CASE("読み形は base の An にロードした値を commit が上書きする")
{
    // **G4 の要点。** インタプリタは (m68k_ops_group4.cpp) ループの後に
    // st_.a[reg] = addr を無条件で書くので、マスクに base 自身が入っていても
    // **最後に勝つのは commit の値**。
    //
    // commit を転送より前へ動かす変異は、ここで落ちる。
    clearSeeds();
    GuestSeed seed;
    seed.addr = kMovemAddr;
    for (u32 b = 0; b < 8; ++b)
    {
        seed.bytes.push_back(static_cast<u8>(0xE0u + b));
    }
    guestSeeds().clear();
    guestSeeds().push_back(seed);

    M68kState s = makeState(5);
    s.a[2] = kMovemAddr;
    // D0 と A2 (= base 自身) を読む。ビット: D0=0, A2=10。
    const u32 mask = (1u << 0) | (1u << 10);
    checkEquivalence({movemToRegs(2), static_cast<u16>(mask)}, s, "読み形 base がマスクに入る");

    // 値も名指しで固定する。
    const std::vector<u16> words{movemToRegs(2), static_cast<u16>(mask)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);
    resetExecRam(words);
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    REQUIRE(native.ok);
    REQUIRE_FALSE(native.guardExit);
    // **ロードした値 (0xE4E5E6E7) ではなく、commit の値。**
    CHECK(native.state.a[2] == kMovemAddr + 8u);
}

TEST_CASE("書き形は base の An について元の値を書く")
{
    // -(An) 形で base 自身がマスクに入ると、書かれるのは**減らす前の An**。
    // インタプリタは a[] を書き換える前に値を読むので、そうなる。
    clearSeeds();
    seedRun(kMovemAddr - 64u, 128u, 0x5Au);

    M68kState s = makeState(6);
    s.a[3] = kMovemAddr;
    // D0 と A3 (= base 自身) を積む。**書き形のビットは A7 から数える**ので、
    // A3 は regIndex 11 = ビット 4、D0 は regIndex 0 = ビット 15。
    const u32 mask = (1u << 4) | (1u << 15);
    checkEquivalence({movemToPreDec(3), static_cast<u16>(mask)}, s, "書き形 base がマスクに入る");

    const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);
    resetExecRam(words);
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    REQUIRE(native.ok);
    REQUIRE_FALSE(native.guardExit);

    // 2 本ぶん (8 バイト) 下がる。低い方が D0、高い方が A3 (= base 自身)。
    const u32 base = kMovemAddr - 8u;
    CHECK(native.state.a[3] == base);
    // 高い方の 4 バイトが**元の A3** であること (減らした後の値ではない)。
    for (u32 b = 0; b < 4; ++b)
    {
        const u8 want = static_cast<u8>((kMovemAddr >> ((3u - b) * 8u)) & 0xFFu);
        INFO("b=", b);
        CHECK(execRam()[base + 4u + b] == want);
    }
}

// --- T-E4: all-or-nothing ---------------------------------------------------

TEST_CASE("MOVEM は範囲の一部でも窓を外れたら 1 bit も変えずに降りる")
{
    // **最重要。** ガードの bound を extent (= 4 * 本数) ではなく size (4) に
    // 戻す変異は、ここでしか落ちない。
    //
    // 「1 本目は窓の中、3 本目で外れる」位置に base を置く。bound が 4 なら
    // ガードは通ってしまい、2 本目以降が窓の外へ出る。extent なら降りる。
    clearSeeds();
    const u32 limit = static_cast<u32>(x68k::kMainRamSize);

    for (const u32 count : {2u, 3u, 4u})
    {
        // 下位 count ビット = D0..D(count-1)。
        const u32 mask = (1u << count) - 1u;
        const u32 extent = count * 4u;

        // 末尾 1 語だけが窓の外へ出る位置。
        // base + extent - 4 == limit なので、最後の 1 本が外れる。
        {
            M68kState s = makeState(3);
            s.a[2] = limit - extent + 4u;
            checkEquivalence({movemToRegs(2), static_cast<u16>(mask)}, s, "読み 末尾が窓の外");

            const std::vector<u16> words{movemToRegs(2), static_cast<u16>(mask)};
            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            resetExecRam(words);
            resetExecGen();
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("count=", count);
            // **ガードで降りること。** bound を 4 に戻す変異はここで落ちる。
            CHECK(native.guardExit);
            // G7: 降りる先は MOVEM の直前の命令境界。手前に命令が無いので 0。
            CHECK(native.ranOps == 0);
            // 状態が 1 bit も変わっていないこと (An が進んでいない)。
            CHECK(native.state.a[2] == s.a[2]);
            for (u32 i = 0; i < 8; ++i)
            {
                CHECK(native.state.d[i] == s.d[i]);
            }
        }

        // 書き形も同じ。-(An) は base = An - extent なので、
        // An == limit + 4 に置くと先頭 1 語ぶんが窓の外へ出る。
        {
            M68kState s = makeState(3);
            s.a[3] = limit + 4u;
            const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
            checkEquivalence(words, s, "書き 先頭が窓の外");

            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);
            resetExecRam(words);
            resetExecGen();
            const std::vector<u8> before = execRam();
            const std::vector<std::uint16_t> beforeGen = execGen();
            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);
            INFO("count=", count);
            CHECK(native.guardExit);
            CHECK(native.ranOps == 0);
            CHECK(native.state.a[3] == s.a[3]);
            // **メモリも世代も 1 bit も変わっていないこと** (G16: 降りる
            // 経路では touch もしない)。
            CHECK(execRam() == before);
            CHECK(execGen() == beforeGen);
        }
    }
}

TEST_CASE("MOVEM は手前の命令ぶんだけ実行して命令境界で降りる")
{
    // G7 の本体。**手前に命令を置いて、ranOps がそこまでを指すこと。**
    clearSeeds();
    const u32 limit = static_cast<u32>(x68k::kMainRamSize);

    M68kState s = makeState(4);
    s.a[2] = limit - 8u;  // 4 本 (16 バイト) は入らない

    // 手前に 2 命令 (どちらもメモリに触らない Tier A)。
    const std::vector<u16> words{moveq(1, 7), moveq(2, -3), movemToRegs(2), 0x000Fu};
    checkEquivalence(words, s, "手前 2 命令 + MOVEM で脱出");

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);
    resetExecRam(words);
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    REQUIRE(native.ok);
    CHECK(native.guardExit);
    // **手前の 2 命令ぶん。** MOVEM は 1 bit も実行していない。
    CHECK(native.ranOps == 2);
    CHECK(native.state.a[2] == s.a[2]);
}

// --- T-E5: 奇数 An と予減ラップ ---------------------------------------------

TEST_CASE("奇数の An を持つ MOVEM は状態を変えずに降りる")
{
    // G7 / emitRts と同じ論法。**アドレスエラーは再現しない。** 降りて
    // step() に再演させると、インタプリタがアドレスエラーへ入る。
    clearSeeds();

    for (const u32 mask : {0x0001u, 0x000Fu})
    {
        {
            M68kState s = makeState(5);
            s.a[2] = kMovemAddr + 1u;
            checkEquivalence({movemToRegs(2), static_cast<u16>(mask)}, s, "読み 奇数 An");
        }
        {
            M68kState s = makeState(5);
            s.a[3] = kMovemAddr + 1u;
            checkEquivalence({movemToPreDec(3), static_cast<u16>(mask)}, s, "書き 奇数 An");
        }
    }
}

TEST_CASE("予減が 0 からラップする MOVEM はガード不成立で降りる")
{
    // An < extent だと -(An) は 32bit で環算し、インタプリタはアクセス毎の
    // マスクでメモリ最上部へ回る。生成コードの範囲ガードはこれを
    // **不成立側**に落とす。保守的だが正しい (step() が再演する)。
    clearSeeds();

    for (const u32 count : {1u, 2u, 4u})
    {
        const u32 mask = (1u << count) - 1u;
        M68kState s = makeState(2);
        s.a[3] = count * 4u - 4u;  // extent より小さい
        const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
        checkEquivalence(words, s, "書き 予減ラップ");

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        INFO("count=", count);
        CHECK(native.guardExit);
        CHECK(native.ranOps == 0);
        CHECK(native.state.a[3] == s.a[3]);
    }
}

// --- T-E6: touch の回数 -----------------------------------------------------

TEST_CASE("MOVEM の書きは転送ごとに touch を 2 回する")
{
    // G14/G16。**転送 1 回あたり 1 回へ畳む変異がここで落ちる。**
    //
    // 飽和つきなので回数が意味を持つ。世代が飽和の手前から始まる状況を
    // 作って、インタプリタと同じ回数だけ上がることを問う。
    clearSeeds();
    seedRun(kMovemAddr - 64u, 128u, 0x77u);

    for (const u32 count : {1u, 2u, 4u})
    {
        const u32 mask = (1u << count) - 1u;
        M68kState s = makeState(3);
        s.a[3] = kMovemAddr;
        const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};

        // まず素の一致 (checkEquivalence が世代配列もバイト単位で比べる)。
        checkEquivalence(words, s, "MOVEM 書き 世代");

        // 回数を直に数える。全転送が同一ページに載る位置なので、
        // そのページの世代が **2 * count** 上がるはず。
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        REQUIRE_FALSE(native.guardExit);

        const u32 page = (kMovemAddr - count * 4u) >> 10;
        INFO("count=", count, " page=", page);
        // **転送ごとに 2 回。** 畳む変異なら count になる。
        CHECK(execGen()[page] == static_cast<std::uint16_t>(2u * count));
    }
}

TEST_CASE("MOVEM の書きの世代が飽和の境界でインタプリタと一致する")
{
    // 飽和つきの回数が意味を持つ形。0xFFFF で止まることと、
    // そこへ至る回数がインタプリタと同じであること。
    clearSeeds();
    seedRun(kMovemAddr - 64u, 128u, 0x21u);

    const u32 mask = 0x000Fu;  // 4 本 = touch 8 回
    M68kState s = makeState(3);
    s.a[3] = kMovemAddr;
    const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
    const u32 page = (kMovemAddr - 16u) >> 10;

    // 飽和の手前・ちょうど・跨ぐところを問う。
    for (const std::uint16_t seedGen :
         {static_cast<std::uint16_t>(0xFFF0u), static_cast<std::uint16_t>(0xFFF8u),
          static_cast<std::uint16_t>(0xFFFEu), static_cast<std::uint16_t>(0xFFFFu)})
    {
        checkEquivalence(words, s, "MOVEM 書き 世代の飽和", fakeEnv(), page, seedGen);
    }
}

// --- T-E7: 自ページへの書き -------------------------------------------------

TEST_CASE("自ページに掛かる MOVEM の書きは書く前に脱出する")
{
    // G13。**範囲が plan.page に掛かったら 1 バイトも書かない。**
    clearSeeds();
    const u32 selfPageBase = kEntry & ~0x3FFu;

    // (a) 範囲の末尾が自ページの先頭へ食い込む。
    //     base は手前のページだが base + extent - 1 が自ページ。
    {
        const u32 mask = 0x0003u;  // 2 本 = 8 バイト
        M68kState s = makeState(6);
        s.a[3] = selfPageBase + 4u;  // base = selfPageBase - 4
        const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
        checkEquivalence(words, s, "MOVEM が自ページの先頭へ食い込む");

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(words);
        resetExecGen();
        const std::vector<u8> before = execRam();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        CHECK(native.guardExit);
        // **bit23 が立つこと** (G18)。
        CHECK(native.selfPageExit);
        CHECK(native.ranOps == 0);
        CHECK(execRam() == before);
    }

    // (b) 範囲がまるごと自ページに載る。
    {
        const u32 mask = 0x000Fu;
        M68kState s = makeState(6);
        s.a[3] = selfPageBase + 0x200u;
        const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
        checkEquivalence(words, s, "MOVEM がまるごと自ページ");

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        CHECK(native.guardExit);
        CHECK(native.selfPageExit);
        CHECK(native.ranOps == 0);
    }

    // (c) **対の成立側。** 両端が別ページで、範囲も自ページに掛からない
    //     ところなら通ること。ここが無いと「常に脱出する」変異が緑で通る。
    {
        const u32 mask = 0x000Fu;
        seedRun(kMovemAddr - 64u, 128u, 0x3Cu);
        M68kState s = makeState(6);
        s.a[3] = kMovemAddr;  // 自ページから遠い
        const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
        checkEquivalence(words, s, "MOVEM 自ページに掛からない (成立側)");

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        resetExecRam(words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        CHECK_FALSE(native.guardExit);
    }

    // (d) 範囲がページ境界を跨ぐが、どちらも自ページではない形も通ること。
    {
        const u32 mask = 0x000Fu;
        const u32 crossing = (kMovemAddr & ~0x3FFu) + 8u;  // base = crossing - 16 で跨ぐ
        seedRun(crossing - 64u, 128u, 0x4Du);
        M68kState s = makeState(6);
        s.a[3] = crossing;
        const std::vector<u16> words{movemToPreDec(3), static_cast<u16>(mask)};
        checkEquivalence(words, s, "MOVEM がページ境界を跨ぐ (成立側)");
    }
}

// --- T-E8: requiredSize -----------------------------------------------------

TEST_CASE("MOVEM 4 本を並べたブロックが staging に収まる")
{
    // **書き形 MOVEM ×4 が最悪。** 発行できるか、できないなら
    // false で綺麗に断ること (黙って壊れた列を吐かないこと)。
    BlockPlan plan{};
    plan.entryPc = kEntry;
    plan.count = static_cast<std::uint8_t>(x68k::kMaxOps);
    plan.page = kEntry >> 10;
    plan.end = BlockEnd::kUnsupported;
    plan.fallThroughPc = kEntry + x68k::kMaxOps * 4u;
    for (std::uint32_t i = 0; i < x68k::kMaxOps; ++i)
    {
        PlannedOp& op = plan.ops[i];
        op.pc = kEntry + i * 4u;
        op.length = 4;
        op.kind = PlanKind::kMovemRegsToPredec;
        op.size = 4;
        op.cycles = static_cast<std::uint8_t>(8u + 4u * 8u);
        op.eaMode = kModePreDec;
        op.dstReg = 3;
        op.imm = 0x000Fu;  // 4 本 (上限)
    }

    const std::size_t need = jit::requiredSize(plan, 0x4E71, 0x4E71, fakeEnv());
    INFO("MOVEM x4 で ", need, " バイト");
    // 発行できること。**0 なら断っているので、そこで気づける。**
    CHECK(need > 0);
    // **書き形 MOVEM x4 が全 Tier を通じての最悪ケース。** 4 本 x 4 命令で
    // 転送 16 回、touch 32 回ぶんの直線コードになる。runner の控えに
    // 収まらないと、この形のブロックは黙って翻訳されない (= 素通りする)。
    CHECK(need <= x68k::jit::kMaxBlockBytes);

    // 読み形 4 つも同じく。
    BlockPlan readPlan = plan;
    for (std::uint32_t i = 0; i < x68k::kMaxOps; ++i)
    {
        readPlan.ops[i].kind = PlanKind::kMovemPostIncToRegs;
        readPlan.ops[i].eaMode = kModePostInc;
        readPlan.ops[i].srcReg = 2;
        readPlan.ops[i].dstReg = 0;
    }
    const std::size_t readNeed = jit::requiredSize(readPlan, 0x4E71, 0x4E71, fakeEnv());
    INFO("MOVEM 読み x4 で ", readNeed, " バイト");
    CHECK(readNeed > 0);
    CHECK(readNeed <= x68k::jit::kMaxBlockBytes);
}

TEST_CASE("窓が短すぎる MOVEM は発行しない")
{
    // **extent で見ていること。** op.size (4) で見る変異なら、
    // 「4 本ぶんは入らないが 4 バイトなら入る」窓で発行してしまう。
    BlockPlan plan{};
    plan.entryPc = kEntry;
    plan.count = 1;
    plan.page = kEntry >> 10;
    plan.end = BlockEnd::kUnsupported;
    plan.fallThroughPc = kEntry + 4u;
    PlannedOp& op = plan.ops[0];
    op.pc = kEntry;
    op.length = 4;
    op.kind = PlanKind::kMovemPostIncToRegs;
    op.size = 4;
    op.cycles = 44;
    op.eaMode = kModePostInc;
    op.srcReg = 2;
    op.imm = 0x000Fu;  // 4 本 = 16 バイト

    // 16 バイトぶんを一度も許さない窓。**4 バイトなら許す**ので、
    // size で見る変異はここで通ってしまう。
    jit::EmitEnv tooSmall = fakeEnv();
    tooSmall.ramLimit = 8;
    CHECK(jit::requiredSize(plan, 0x4E71, 0x4E71, tooSmall) == 0);

    // 足りる窓なら発行できること (対の成立側)。
    CHECK(jit::requiredSize(plan, 0x4E71, 0x4E71, fakeEnv()) > 0);
}

TEST_CASE("本数が上限を超える MOVEM はエミッタ単体でも断る")
{
    // 翻訳器が先に弾いているが、**エミッタ単体でも断れること。**
    // 計画を直接組んで問う。
    for (const u32 mask : {0x0000u, 0x001Fu, 0xFFFFu})
    {
        BlockPlan plan{};
        plan.entryPc = kEntry;
        plan.count = 1;
        plan.page = kEntry >> 10;
        plan.end = BlockEnd::kUnsupported;
        plan.fallThroughPc = kEntry + 4u;
        PlannedOp& op = plan.ops[0];
        op.pc = kEntry;
        op.length = 4;
        op.kind = PlanKind::kMovemPostIncToRegs;
        op.size = 4;
        op.cycles = 44;
        op.eaMode = kModePostInc;
        op.srcReg = 2;
        op.imm = mask;

        INFO("mask=", mask);
        CHECK(jit::requiredSize(plan, 0x4E71, 0x4E71, fakeEnv()) == 0);
    }
}

// --- Tier D: 動的分岐 (RTS / JSR) -------------------------------------------
//
// 静的分岐 (Tier A の Bcc) との違いは、飛び先が**実行時にしか分からない**こと。
// 生成コードはそれをメールボックスへ書き、runner が branchTo へ渡す。
//
// **飛び先の一致がここでの同値性の中心。** checkEquivalence が
// 「メールボックスの値 == 参照側の pc - 4」を問うので、飛び先を 1 bit でも
// 間違える変異は必ず落ちる。

namespace
{

// 動的分岐のテストで使うスタックアドレス。**偶数**で、コードのページ
// (kEntry = 0x2000 → ページ 8) から十分離す。
constexpr u32 kStackAddr = 0x00050000u;

// 飛び先として使うアドレス。**偶数**にする (奇数はガードが弾く)。
constexpr u32 kTargetAddr = 0x00060000u;

// スタックへ 32bit の戻り先を積んだ状態を作る。
void seedReturnAddress(u32 stackAddr, u32 returnTo)
{
    guestSeeds().clear();
    guestSeeds().push_back(
        GuestSeed{stackAddr,
                  {static_cast<u8>(returnTo >> 24), static_cast<u8>(returnTo >> 16),
                   static_cast<u8>(returnTo >> 8), static_cast<u8>(returnTo)}});
}

}  // namespace

TEST_CASE("RTS がインタプリタと一致する")
{
    for (u32 seed : {1u, 3u, 7u})
    {
        // 素直な形。A7 が窓の中を指し、戻り先も偶数。
        {
            seedReturnAddress(kStackAddr, kTargetAddr);
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({kRtsOp}, s, "RTS");
        }
        // 戻り先の 4 バイトが全部違う値。**ビッグエンディアンの組み立て**を問う。
        // どれか 1 バイトでも入れ替わったら飛び先が変わって落ちる。
        {
            seedReturnAddress(kStackAddr, 0x00123456u);
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({kRtsOp}, s, "RTS 戻り先が 4 バイトとも違う");
        }
        // 上位バイト付きの戻り先。**マスクしないこと**を問う。
        //
        // X68000 の IOCS は未初期化ベクタに $43FF0540 のような値を埋める。
        // インタプリタは refillPrefetch へ無マスクで渡し、pc にもその値が入る
        // (m68k.cpp:180 のコメント)。生成コードがここでマスクすると、
        // pc が $00FF0540 になって食い違う。
        {
            seedReturnAddress(kStackAddr, 0x43FF0540u);
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({kRtsOp}, s, "RTS 上位バイト付きの戻り先");
        }
        // 戻り先が 0。
        {
            seedReturnAddress(kStackAddr, 0x00000000u);
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({kRtsOp}, s, "RTS 戻り先が 0");
        }
    }
    clearSeeds();
}

TEST_CASE("RTS が A7 を 4 進める")
{
    // **飛び先だけでなく A7 も見る。** compareStates が a[7] を比べるので
    // checkEquivalence だけでも落ちるが、進む向きと量を名指しで固定する。
    seedReturnAddress(kStackAddr, kTargetAddr);
    M68kState s = makeState(2);
    s.a[7] = kStackAddr;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({kRtsOp}, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam({kRtsOp});
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);
    CHECK(native.dynamicBranch);
    CHECK(native.state.a[7] == kStackAddr + 4u);
    CHECK(native.mailbox == kTargetAddr);
    clearSeeds();
}

TEST_CASE("RTS は世代を 1 つも上げない")
{
    // **RTS は読むだけ。** touch を吐く変異はここで落ちる
    // (checkEquivalence の compareMemory でも落ちるが、名指しで固定する)。
    seedReturnAddress(kStackAddr, kTargetAddr);
    M68kState s = makeState(5);
    s.a[7] = kStackAddr;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({kRtsOp}, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam({kRtsOp});
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    REQUIRE(native.ok);
    for (std::size_t i = 0; i < execGen().size(); ++i)
    {
        if (execGen()[i] != 0)
        {
            INFO("page ", i, " generation moved to ", execGen()[i]);
            CHECK(execGen()[i] == 0);
            break;
        }
    }
    clearSeeds();
}

TEST_CASE("RTS の戻り先が奇数なら A7 を 1 bit も動かさない")
{
    // **これが Tier D の要点。** インタプリタは A7 を進めてから
    // refillPrefetch でアドレスエラーへ入る (m68k_ops_group4.cpp:96-102) が、
    // ネイティブは 1 bit も変えずに降りて step() に再演させる。
    //
    // 落ちる変異: 奇数ガードを A7 の更新より後ろへ動かす
    seedReturnAddress(kStackAddr, 0x00060001u);  // 奇数
    M68kState s = makeState(4);
    s.a[7] = kStackAddr;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({kRtsOp}, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam({kRtsOp});
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);

    // ガード脱出で降りた。**動的分岐ではない。**
    CHECK(native.guardExit);
    CHECK_FALSE(native.dynamicBranch);
    CHECK(native.ranOps == 0);
    CHECK(native.cycles == 0);

    // **状態が 1 bit も変わっていない。** 出口の pc / ir / irc は
    // 「その命令の直前の命令境界」を指すので、そこだけは別に見る。
    M68kState want = s;
    want.pc = kEntry + 4u;
    want.ir = kRtsOp;
    // **irc は「この命令の次の語」= mem16(opPc + 2)。** buildPlan が使う
    // FlatCode は置いた語以外がゼロなので、そこから引く
    // (定数で書くと、埋め方を変えたときに気づけない)。
    want.irc = code.get16(kEntry + 2u);
    compareStates(want, native.state, /*skipPrefetch=*/false, "RTS 奇数戻り先で脱出");
    clearSeeds();
}

TEST_CASE("RTS のスタックが窓の外なら降りる")
{
    struct Case
    {
        u32 a7;
        const char* what;
    };
    const std::vector<Case> cases = {
        // 奇数の A7。read32 がアドレスエラーへ入る形。
        {kStackAddr + 1u, "A7 が奇数"},
        // 窓の末尾を跨ぐ。a + 3 < limit を満たさない。
        {static_cast<u32>(x68k::kMainRamSize) - 2u, "A7 + 3 が窓の外"},
        {static_cast<u32>(x68k::kMainRamSize), "A7 が窓のちょうど外"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        clearSeeds();
        M68kState s = makeState(6);
        s.a[7] = c.a7;

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan({kRtsOp}, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        resetExecRam({kRtsOp});
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
        REQUIRE(native.ok);

        CHECK(native.guardExit);
        CHECK_FALSE(native.dynamicBranch);
        CHECK(native.ranOps == 0);

        M68kState want = s;
        want.pc = kEntry + 4u;
        want.ir = kRtsOp;
        want.irc = code.get16(kEntry + 2u);
        compareStates(want, native.state, /*skipPrefetch=*/false, c.what);
    }
    clearSeeds();
}

TEST_CASE("RTS の窓の境界がインタプリタと一致する")
{
    // 窓の末尾ちょうどに収まる A7 は通り、1 バイトでも出れば降りる。
    //
    // **境界の値を名指しで問う。** `<` を `<=` にする変異はここでしか落ちない。
    const u32 limit = static_cast<u32>(x68k::kMainRamSize);
    for (u32 a7 : {limit - 4u, limit - 6u})
    {
        INFO("a7 = ", a7);
        seedReturnAddress(a7, kTargetAddr);
        M68kState s = makeState(8);
        s.a[7] = a7;
        // ガードが成立するので、インタプリタと丸ごと一致するはず。
        checkEquivalence({kRtsOp}, s, "RTS 窓の末尾ぎりぎり");
    }
    clearSeeds();
}

TEST_CASE("JSR がインタプリタと一致する")
{
    for (u32 seed : {1u, 3u, 6u})
    {
        // (An) — 最頻の形。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            s.a[3] = kTargetAddr;
            checkEquivalence({jsr(kModeInd, 3)}, s, "JSR (An)");
        }
        // (d16,An) — 正の変位。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            s.a[4] = kTargetAddr;
            checkEquivalence({jsr(kModeDisp, 4), 0x0100u}, s, "JSR (d16,An) 正");
        }
        // (d16,An) — 負の変位。**符号拡張**を問う。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            s.a[4] = kTargetAddr;
            checkEquivalence({jsr(kModeDisp, 4), 0xFF00u}, s, "JSR (d16,An) 負");
        }
        // (xxx).W — 符号拡張された絶対アドレス。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({jsr(kModeAbsW, 0), 0x1000u}, s, "JSR (xxx).W");
        }
        // (xxx).L — 2 語の連結。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({jsr(kModeAbsL, 1), static_cast<u16>(kTargetAddr >> 16),
                              static_cast<u16>(kTargetAddr & 0xFFFFu)},
                             s, "JSR (xxx).L");
        }
    }
    clearSeeds();
}

TEST_CASE("JSR が積む戻り先が次の命令のアドレス")
{
    // **長さごとに戻り先が変わる。** インタプリタの returnAddr = pc - 4 は
    // 「この命令の次のアドレス」で、JSR の長さ L に対して entryPc + L。
    // ここを op.pc + 2 に固定する変異は、(d16,An) と (xxx).L で落ちる。
    struct Case
    {
        std::vector<u16> words;
        u32 length;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{jsr(kModeInd, 3)}, 2u, "JSR (An)"},
        {{jsr(kModeDisp, 4), 0x0000u}, 4u, "JSR (d16,An)"},
        {{jsr(kModeAbsW, 0), 0x1000u}, 4u, "JSR (xxx).W"},
        {{jsr(kModeAbsL, 1), 0x0006u, 0x0000u}, 6u, "JSR (xxx).L"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        clearSeeds();
        M68kState s = makeState(2);
        s.a[7] = kStackAddr;
        s.a[3] = kTargetAddr;
        s.a[4] = kTargetAddr;

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        REQUIRE(plan.ops[0].length == c.length);
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        resetExecRam(c.words);
        resetExecGen();
        const NativeOutcome native = runEmitted(e, s);
        INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
        REQUIRE(native.ok);
        REQUIRE(native.dynamicBranch);

        // A7 が 4 減っている。
        CHECK(native.state.a[7] == kStackAddr - 4u);
        // 積まれた戻り先がビッグエンディアンで、値は entryPc + length。
        const u32 want = kEntry + c.length;
        const u32 pushed = (static_cast<u32>(execRam()[kStackAddr - 4]) << 24) |
                           (static_cast<u32>(execRam()[kStackAddr - 3]) << 16) |
                           (static_cast<u32>(execRam()[kStackAddr - 2]) << 8) |
                           execRam()[kStackAddr - 1];
        CHECK(pushed == want);
    }
    clearSeeds();
}

TEST_CASE("JSR がスタックの 2 ページぶんの世代を上げる")
{
    // **write32 は page(a) と page(a+3) を別々に touch する** (m68k.cpp:377-378)。
    // 畳む変異は、同一ページでも「世代が 2 でなく 1」で落ちる。
    clearSeeds();
    M68kState s = makeState(3);
    s.a[7] = kStackAddr;
    s.a[3] = kTargetAddr;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({jsr(kModeInd, 3)}, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam({jsr(kModeInd, 3)});
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    REQUIRE(native.ok);
    REQUIRE(native.dynamicBranch);

    // 積み先は kStackAddr - 4。同一ページなので 1 ページが 2 進む。
    const u32 page = (kStackAddr - 4u) >> x68k::CodeGenMap::kPageShift;
    CHECK(execGen()[page] == 2);
}

TEST_CASE("JSR の飛び先が奇数ならスタックを 1 bit も動かさない")
{
    // インタプリタは積んでから refillPrefetch でアドレスエラーへ入るが、
    // ネイティブは 1 bit も変えずに降りて step() に再演させる。
    //
    // 落ちる変異: 飛び先の整列ガードを積んだ後ろへ動かす
    //
    // **動的 EA でしか作れない。** 絶対形の奇数は翻訳器が積まない。
    clearSeeds();
    M68kState s = makeState(4);
    s.a[7] = kStackAddr;
    s.a[3] = kTargetAddr + 1u;  // 奇数

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({jsr(kModeInd, 3)}, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam({jsr(kModeInd, 3)});
    resetExecGen();
    const std::vector<u8> before = execRam();
    const NativeOutcome native = runEmitted(e, s);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);

    CHECK(native.guardExit);
    CHECK_FALSE(native.dynamicBranch);
    CHECK(native.ranOps == 0);

    M68kState want = s;
    want.pc = kEntry + 4u;
    want.ir = jsr(kModeInd, 3);
    want.irc = code.get16(kEntry + 2u);
    compareStates(want, native.state, /*skipPrefetch=*/false, "JSR 奇数飛び先で脱出");

    // メモリも世代も動いていない。
    compareMemory(before, std::vector<std::uint16_t>(kGenPages, 0), "JSR 奇数飛び先で脱出");
}

TEST_CASE("JSR のスタックが窓の外なら降りる")
{
    struct Case
    {
        u32 a7;
        const char* what;
    };
    const std::vector<Case> cases = {
        // A7 - 4 が奇数。write32 がアドレスエラーへ入る形。
        {kStackAddr + 1u, "A7 - 4 が奇数"},
        // A7 - 4 が窓の外 (0 からラップして $00FFFFFC になる)。
        {0u, "A7 が 0 (ラップして窓の外)"},
        // A7 - 4 + 3 が窓を跨ぐ。
        {static_cast<u32>(x68k::kMainRamSize) + 2u, "A7 - 4 + 3 が窓の外"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        clearSeeds();
        M68kState s = makeState(7);
        s.a[7] = c.a7;
        s.a[3] = kTargetAddr;

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan({jsr(kModeInd, 3)}, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        resetExecRam({jsr(kModeInd, 3)});
        resetExecGen();
        const std::vector<u8> before = execRam();
        const NativeOutcome native = runEmitted(e, s);
        INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
        REQUIRE(native.ok);

        CHECK(native.guardExit);
        CHECK_FALSE(native.dynamicBranch);
        CHECK(native.ranOps == 0);

        M68kState want = s;
        want.pc = kEntry + 4u;
        want.ir = jsr(kModeInd, 3);
        want.irc = code.get16(kEntry + 2u);
        compareStates(want, native.state, /*skipPrefetch=*/false, c.what);
        compareMemory(before, std::vector<std::uint16_t>(kGenPages, 0), c.what);
    }
    clearSeeds();
}

TEST_CASE("JSR が自ページへ積むなら書く前に脱出する")
{
    // **G13。** 積み先が自ブロックのページなら、焼いた定数 (出口の ir/irc) が
    // 実行中に古くなる。書く前に降りる。
    //
    // A7 をコードのすぐ後ろに置いて、-(A7) が kEntry のページへ掛かるようにする。
    clearSeeds();
    const u32 selfPageStack = kEntry + 0x40u;  // kEntry と同じ 1KB ページ
    M68kState s = makeState(5);
    s.a[7] = selfPageStack;
    s.a[3] = kTargetAddr;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({jsr(kModeInd, 3)}, plan, code));
    REQUIRE(plan.page == (kEntry >> x68k::CodeGenMap::kPageShift));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam({jsr(kModeInd, 3)});
    resetExecGen();
    const std::vector<u8> before = execRam();
    const NativeOutcome native = runEmitted(e, s);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);

    CHECK(native.guardExit);
    // **自ページ脱出の印が立つ。** runner はこれを見て「世代不問」を焼く。
    CHECK(native.selfPageExit);
    CHECK_FALSE(native.dynamicBranch);
    CHECK(native.ranOps == 0);

    M68kState want = s;
    want.pc = kEntry + 4u;
    want.ir = jsr(kModeInd, 3);
    want.irc = code.get16(kEntry + 2u);
    compareStates(want, native.state, /*skipPrefetch=*/false, "JSR 自ページへ積む");
    compareMemory(before, std::vector<std::uint16_t>(kGenPages, 0), "JSR 自ページへ積む");
}

TEST_CASE("動的分岐の手前の命令はちゃんと実行される")
{
    // **ブロック末尾の扱いが kBranch と同じであること。** 本体の命令を
    // 落とす変異 (bodyCount を plan.count のままにする等) はここで落ちる。
    for (u32 seed : {1u, 9u})
    {
        {
            seedReturnAddress(kStackAddr, kTargetAddr);
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({moveq(0, 5), moveq(1, -3), kRtsOp}, s, "MOVEQ x2 + RTS");
        }
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            s.a[3] = kTargetAddr;
            checkEquivalence({moveq(2, 7), aluReg(0xD, 2, 2, 1), jsr(kModeInd, 3)}, s,
                             "MOVEQ + ADD.l + JSR");
        }
        // 読み形 (Tier B) と混ぜる。
        {
            seedReturnAddress(kStackAddr, kTargetAddr);
            guestSeeds().push_back(GuestSeed{kDataAddr, {0x11, 0x22, 0x33, 0x44}});
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            s.a[2] = kDataAddr;
            checkEquivalence({moveMemToDn(0x2u, 1, kModeInd, 2), kRtsOp}, s,
                             "MOVE.l (An),Dn + RTS");
        }
    }
    clearSeeds();
}

TEST_CASE("動的分岐の手前でガードが不成立なら、その手前で降りる")
{
    // **島の出口が「その命令の直前の命令境界」であること** (G7)。
    // 動的分岐そのものではなく、手前の読み形が降りる形を問う。
    clearSeeds();
    M68kState s = makeState(3);
    s.a[7] = kStackAddr;
    s.a[2] = static_cast<u32>(x68k::kMainRamSize);  // 窓の外

    const std::vector<u16> words{moveq(0, 5), moveMemToDn(0x2u, 1, kModeInd, 2), kRtsOp};

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    REQUIRE(plan.count == 3);
    REQUIRE(plan.end == BlockEnd::kDynamicBranch);
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam(words);
    resetExecGen();
    const NativeOutcome native = runEmitted(e, s);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);

    CHECK(native.guardExit);
    CHECK_FALSE(native.dynamicBranch);
    // MOVEQ の 1 命令だけ実行して降りた。
    CHECK(native.ranOps == 1);
    CHECK(native.cycles == 4);
    // 出口は 2 番目の命令の手前。
    CHECK(native.state.pc == kEntry + 2u + 4u);
    CHECK(native.state.ir == moveMemToDn(0x2u, 1, kModeInd, 2));
}

TEST_CASE("メールボックスが無い窓では動的分岐を発行しない")
{
    // **G21 の前提。** 飛び先の置き場が無いなら焼かない。焼くと
    // 生成コードが 0 番地 (= 状態領域の d[0]) へ飛び先を書く。
    //
    // 落ちる変異: canEmitDynamicBranchIn を常に true にする
    jit::EmitEnv env = fakeEnv();
    env.mailboxAddr = 0;

    FlatCode code;
    BlockPlan plan{};
    // 翻訳器には「入れてよい」と言わせたまま (既定の PlanCapabilities)
    // 計画を作り、エミッタ単体が断ることを問う。
    REQUIRE(buildPlan({kRtsOp}, plan, code));
    REQUIRE(plan.end == BlockEnd::kDynamicBranch);

    CHECK(jit::requiredSize(plan, 0x4E71u, 0x4E71u, env) == 0u);
    CHECK(jit::canEmitDynamicBranchIn(fakeEnv()));
    CHECK_FALSE(jit::canEmitDynamicBranchIn(env));
}

TEST_CASE("窓が読めない写像では RTS を発行しない")
{
    // **RTS は eaMode を持たない** ので、eaMode だけを見る canEmitReads は
    // RTS を「読まない形」と判定する。needsReadWindow / エミッタ側の
    // canEmitDynamicBranchFor が別に見ないと、ROM 写像中に RTS を焼いて
    // **窓の外のホストメモリを読む**。
    //
    // 落ちる変異: canEmitDynamicBranchFor から kRts の窓の判定を消す
    jit::EmitEnv env = fakeEnv();
    env.ramReadable = false;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({kRtsOp}, plan, code));
    CHECK(jit::requiredSize(plan, 0x4E71u, 0x4E71u, env) == 0u);

    // JSR は書き経路なので **ramReadable を要らない** (m68k.cpp:331-334)。
    // ROM 写像中でも RAM へは書けるので、こちらは発行できる。
    BlockPlan jsrPlan{};
    FlatCode jsrCode;
    REQUIRE(buildPlan({jsr(kModeInd, 3)}, jsrPlan, jsrCode));
    CHECK(jit::requiredSize(jsrPlan, 0x4E71u, 0x4E71u, env) > 0u);
}

TEST_CASE("世代配列が無い窓では JSR を発行しない")
{
    // **JSR は -(A7) へ write32 する。** touch を再現できないと、
    // 世代が JIT ON/OFF で割れる。
    //
    // 落ちる変異: canEmitDynamicBranchFor から kJsr の canEmitWritesIn を消す
    jit::EmitEnv env = fakeEnv();
    env.genBaseAddr = 0;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({jsr(kModeInd, 3)}, plan, code));
    CHECK(jit::requiredSize(plan, 0x4E71u, 0x4E71u, env) == 0u);

    // RTS は読むだけなので世代配列が無くても焼ける。
    BlockPlan rtsPlan{};
    FlatCode rtsCode;
    REQUIRE(buildPlan({kRtsOp}, rtsPlan, rtsCode));
    CHECK(jit::requiredSize(rtsPlan, 0x4E71u, 0x4E71u, env) > 0u);
}

TEST_CASE("動的分岐は必ずブロック末尾になる")
{
    // **I6 / G23。** RTS / JSR の後ろに命令があっても積まない。
    // 積むと、飛んだ後に実行されないはずの命令が実行される。
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{kRtsOp, moveq(0, 1)}, "RTS の後ろ"},
        {{jsr(kModeInd, 3), moveq(0, 1)}, "JSR の後ろ"},
        {{moveq(0, 1), kRtsOp, moveq(1, 2)}, "MOVEQ + RTS の後ろ"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        CHECK(plan.end == BlockEnd::kDynamicBranch);
        // 末尾が動的分岐で、それ以降が積まれていない。
        CHECK(plan.count == c.words.size() - 1u);
        const PlanKind lastKind = plan.ops[plan.count - 1u].kind;
        const bool lastIsDynamic = lastKind == PlanKind::kRts || lastKind == PlanKind::kJsr;
        CHECK(lastIsDynamic);
    }
}

TEST_CASE("JSR の奇数な絶対飛び先は翻訳時に弾く")
{
    // **I7 の Tier D 版。** 翻訳時に分かる奇数は積まない。積むと
    // 「毎周ガードを踏むだけのブロック」になる。
    //
    // 落ちる変異: plan() の isAbsoluteJumpTarget の判定を消す
    // (xxx).L の下位語を奇数にする。**先頭に置くと 1 命令も積めない**ので
    // plan() は false を返す (呼び出し側は kDeferToStep で step() へ落とす)。
    {
        FlatCode code;
        BlockPlan plan{};
        const std::vector<u16> words{jsr(kModeAbsL, 1), 0x0006u, 0x0001u};
        CHECK_FALSE(buildPlan(words, plan, code));
    }

    // 手前に命令があれば、そこまでを積んで**奇数の JSR の手前で終端する**。
    // これが「積まない」ことの本体で、上のケースは 0 命令になっただけ。
    {
        FlatCode code;
        BlockPlan plan{};
        const std::vector<u16> words{moveq(0, 1), jsr(kModeAbsL, 1), 0x0006u, 0x0001u};
        REQUIRE(buildPlan(words, plan, code));
        CHECK(plan.count == 1u);
        CHECK(plan.end == BlockEnd::kUnsupported);
    }

    // 偶数なら積む。**弾く条件が「奇数」であって「絶対形」ではない**ことを
    // 対で問う。片方だけだと、絶対形を丸ごと拒否する変異が通ってしまう。
    {
        FlatCode code;
        BlockPlan plan{};
        const std::vector<u16> words{jsr(kModeAbsL, 1), 0x0006u, 0x0000u};
        REQUIRE(buildPlan(words, plan, code));
        CHECK(plan.count == 1u);
        CHECK(plan.end == BlockEnd::kDynamicBranch);
    }
}

TEST_CASE("動的分岐を含むブロックが staging に収まる")
{
    // **BlockRunner::kStagingBytes (1536) に収まること。** 収まらないと
    // 黙って諦める (素通りする) 形になるので、気づきにくい。
    //
    // 最悪ケース: 書き形 3 つ + JSR (xxx).L。書き形が一番長く、JSR は
    // 「飛び先 + ガード 4 本 + touch 2 組 + store 4 本 + 島」を持つ。
    constexpr std::size_t kStagingBytes = 1536;

    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{kRtsOp}, "RTS 単体"},
        {{jsr(kModeAbsL, 1), 0x0006u, 0x0000u}, "JSR (xxx).L 単体"},
        {{moveDnToMem(0x2u, 0, kModeInd, 1), moveDnToMem(0x2u, 1, kModeInd, 2),
          moveDnToMem(0x2u, 2, kModeInd, 3), kRtsOp},
         "MOVE.l Dn,(An) x3 + RTS"},
        {{clrMem(2u, kModeInd, 1), clrMem(2u, kModeInd, 2), clrMem(2u, kModeInd, 3),
          jsr(kModeInd, 4)},
         "CLR.l (An) x3 + JSR (An)"},
    };

    std::size_t worst = 0;
    const char* worstWhat = "";
    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        const u16 ir = code.get16(plan.fallThroughPc);
        const u16 irc = code.get16(plan.fallThroughPc + 2);
        const std::size_t need = jit::requiredSize(plan, ir, irc, fakeEnv());
        // **0 なら発行できていない。** 諦めた形を「収まった」と読まない。
        REQUIRE(need > 0u);
        CHECK(need <= kStagingBytes);
        if (need > worst)
        {
            worst = need;
            worstWhat = c.what;
        }
    }
    INFO("worst = ", worst, " (", std::string(worstWhat), ")");
    // 余裕が 20% 以上あること。命令を 1 つ足しただけで超えるのは危うい。
    CHECK(worst * 5u <= kStagingBytes * 4u);
}

TEST_CASE("動的分岐の出口が pc / ir / irc を書かない")
{
    // **分岐成立側 (Tier A) と同じ契約。** 飛び先のプリフェッチは
    // ページの外を読みうるので、runner が branchTo で詰め直す。
    // 生成コードが書くと、そこで書いた値が branchTo の結果と食い違う。
    //
    // 落ちる変異: emitDynamicBranchExit で emitBoundaryExit を呼ぶ
    seedReturnAddress(kStackAddr, kTargetAddr);
    M68kState initial = makeState(2);
    initial.a[7] = kStackAddr;
    // pc / ir / irc に見分けのつく値を入れておく。
    initial.pc = 0x0BADF00Du;
    initial.ir = 0xBEEFu;
    initial.irc = 0xCAFEu;

    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({kRtsOp}, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    resetExecRam({kRtsOp});
    resetExecGen();
    const NativeOutcome native = runEmitted(e, initial);
    REQUIRE(native.ok);
    REQUIRE(native.dynamicBranch);

    CHECK(native.state.pc == initial.pc);
    CHECK(native.state.ir == initial.ir);
    CHECK(native.state.irc == initial.irc);
    clearSeeds();
}

TEST_CASE("JSR は CCR を 1 bit も変えない")
{
    // **JSR はフラグを触らない** (m68k_ops_group4.cpp:354-371)。
    // checkEquivalence の compareStates が sr を比べるので落ちるが、
    // 「飛び先の置き場に kTmpCcr を使っている」という実装の都合が
    // CCR を壊さないことを名指しで固定する。
    for (u16 sr : {u16(0x2700u), u16(0x271Fu), u16(0xA715u), u16(0x0000u)})
    {
        clearSeeds();
        M68kState s = makeState(1);
        s.sr = sr;
        s.a[7] = kStackAddr;
        s.a[3] = kTargetAddr;
        checkEquivalence({jsr(kModeInd, 3)}, s, "JSR は CCR を変えない");
    }
    clearSeeds();
}

// --- Tier F: BSR -------------------------------------------------------------
//
// **JSR のスタック操作 + Bcc の静的な飛び先。** ここで問うのは 4 つ:
//
//   飛び先が BlockSlot::branchTarget から来ること (メールボックスではない)
//   積む戻り先が「次の命令のアドレス」であること
//   ガードが all-or-nothing であること (A7 も pc も動かさずに降りる)
//   サイクルが .s / .w とも 18 であること
//
// checkEquivalence が「参照側の pc == branchTarget + 4」と
// 「kBranchTakenFlag が立ち kDynamicBranchFlag が立たない」を問うので、
// 飛び先を取り違える変異と出口のビットを取り違える変異は必ず落ちる。

namespace
{

// BSR.s。disp8 は $00 / $FF を除く。
constexpr u16 bsrShort(int disp8)
{
    return static_cast<u16>(0x6100u | (static_cast<u32>(disp8) & 0xFFu));
}
constexpr u16 kBsrWordOp = 0x6100u;

}  // namespace

TEST_CASE("BSR がインタプリタと一致する")
{
    for (u32 seed : {1u, 3u, 7u})
    {
        // BSR.s。前方へ飛ぶ。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({bsrShort(4)}, s, "BSR.s 前方");
        }
        // BSR.s。**後方へ飛ぶ (負の変位)。** 符号拡張を間違えると落ちる。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({bsrShort(-0x40)}, s, "BSR.s 後方");
        }
        // BSR.w。拡張ワードの変位。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({kBsrWordOp, 0x0010u}, s, "BSR.w 前方");
        }
        // BSR.w。負の 16bit 変位。
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({kBsrWordOp, 0xFF00u}, s, "BSR.w 後方");
        }
        // 手前に命令があるブロック。**BSR が末尾でも前の命令の結果が残る。**
        {
            clearSeeds();
            M68kState s = makeState(seed);
            s.a[7] = kStackAddr;
            checkEquivalence({moveq(1, 0x42), bsrShort(6)}, s, "MOVEQ + BSR.s");
        }
    }
    clearSeeds();
}

TEST_CASE("BSR が積む戻り先が次の命令のアドレス")
{
    // **インタプリタの returnAddr は .s で base、.w で base + 2**
    // (base = 命令語 + 2)。どちらも「命令語 + 命令長」に等しい。
    //
    // 落ちる変異:
    //   - 戻り先を op.pc + op.length ± 2 にする
    //   - .s と .w で同じ定数 (op.pc + 2 など) を使う
    struct Case
    {
        std::vector<u16> words;
        u32 length;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{bsrShort(4)}, 2u, "BSR.s"},
        {{kBsrWordOp, 0x0010u}, 4u, "BSR.w"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        clearSeeds();
        M68kState s = makeState(2);
        s.a[7] = kStackAddr;

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        REQUIRE(plan.end == BlockEnd::kBranch);
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        resetExecRam(c.words);
        resetExecGen(0xFFFFFFFFu, 0);
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);

        // A7 は 4 減っている。
        CHECK(native.state.a[7] == kStackAddr - 4u);
        // 積まれた 4 バイトがビッグエンディアンで kEntry + length。
        const u32 want = kEntry + c.length;
        const u32 sp = kStackAddr - 4u;
        const u32 got =
            (static_cast<u32>(execRam()[sp]) << 24) | (static_cast<u32>(execRam()[sp + 1]) << 16) |
            (static_cast<u32>(execRam()[sp + 2]) << 8) | static_cast<u32>(execRam()[sp + 3]);
        INFO("return address");
        CHECK(got == want);
    }
    clearSeeds();
}

TEST_CASE("BSR の飛び先は静的で、メールボックスを使わない")
{
    // **kBranchTakenFlag を立てて BlockSlot::branchTarget から飛ぶ。**
    // kDynamicBranchFlag を立てる実装だと、runner が前のブロックが残した
    // メールボックスの値へ飛ぶ (= 原因から最も遠い暴走)。
    //
    // 落ちる変異:
    //   - emitBsr の出口を emitDynamicBranchExit にする
    //   - hasStaticBranchTarget から kBsr を外す (branchTarget が 0 になる)
    clearSeeds();
    const std::vector<u16> words{bsrShort(4)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    REQUIRE(plan.end == BlockEnd::kBranch);
    // 飛び先 = (kEntry + 2) + 4。
    REQUIRE(plan.branchTarget == kEntry + 6u);

    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);
    CHECK(e.info.endsWithBranch);
    CHECK_FALSE(e.info.endsWithDynamicBranch);
    CHECK(e.info.branchTarget == plan.branchTarget);

    M68kState s = makeState(4);
    s.a[7] = kStackAddr;
    resetExecRam(words);
    resetExecGen(0xFFFFFFFFu, 0);
    const NativeOutcome native = runEmitted(e, s);
    REQUIRE(native.ok);

    CHECK(native.branchTaken);
    CHECK_FALSE(native.dynamicBranch);
    CHECK_FALSE(native.guardExit);
    // **メールボックスは runEmitted が走らせる前に 0xDEADBEEF で潰している。**
    // 生成コードが 1 語も書かないこと = 潰した値がそのまま残ること。
    INFO("mailbox untouched");
    CHECK(native.mailbox == 0xDEADBEEFu);

    // runner のディスパッチが静的な飛び先を選ぶこと。
    // **メールボックスの値を渡しても、そちらを採らない。**
    jit::BlockReturn decoded{};
    decoded.branchTaken = native.branchTaken;
    decoded.dynamicBranch = native.dynamicBranch;
    decoded.guardExit = native.guardExit;
    const jit::BranchDecision decision =
        jit::nextBranch(decoded, plan.branchTarget, native.mailbox);
    CHECK(decision.source == jit::BranchSource::kStaticTarget);
    CHECK(decision.target == plan.branchTarget);
    clearSeeds();
}

TEST_CASE("BSR のサイクルは .s も .w も 18")
{
    // **groupBranch の BSR 経路は disp8 を見ずに 18 を返す**
    // (m68k_ops_move.cpp:126)。Bcc のように形で分かれると思って
    // 12 / 8 を入れると、rasterNumber の 317 サイクル粒度が数万命令後に割れる。
    //
    // 落ちる変異: kBsr の cyclesTaken を 10 (Bcc の成立側) にする
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    for (const Case& c :
         std::vector<Case>{{{bsrShort(4)}, "BSR.s"}, {{kBsrWordOp, 0x0010u}, "BSR.w"}})
    {
        INFO(std::string(c.what));
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        CHECK(plan.cyclesTaken == 18u);
        CHECK(plan.cyclesNotTaken == 18u);
    }
    // 手前に MOVEQ (4) が付けば 22。**足し込まれていること**を問う。
    {
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan({moveq(1, 3), bsrShort(6)}, plan, code));
        CHECK(plan.cyclesTaken == 22u);
    }
}

TEST_CASE("BSR のスタックが窓の外なら 1 bit も動かさずに降りる")
{
    // **all-or-nothing (G3 / G7)。** A7 - 4 が窓の外なら、A7 も pc も
    // ゲスト RAM も世代配列も動かない。
    //
    // 落ちる変異:
    //   - emitBsr の s32i (A7 の更新) をガードより前へ移す
    //   - emitTouch をガードより前へ移す
    //   - 範囲ガードそのものを消す
    struct Case
    {
        u32 sp;
        const char* what;
    };
    const std::vector<Case> cases = {
        // 窓の外 (上)。
        {static_cast<u32>(x68k::kMainRamSize) + 0x100u, "窓の外"},
        // 窓の先頭 (A7 - 4 が下へ回り込む)。
        {0u, "A7 = 0 で A7-4 が回り込む"},
        // **奇数。** write32 はアドレスエラーへ入る。
        {kStackAddr + 1u, "A7-4 が奇数"},
        // **窓の末尾ちょうど。** A7-4 == limit なので 4 バイトぶん外へ出る。
        //
        // ここが op.size を埋めているかどうかを問う唯一の点。範囲ガードは
        // 「a <= limit - size」を作るので、size が 0 のままだと
        // 「a <= limit」になり、**窓の末尾 4 バイトが素通りする**
        // (RTS が size を埋めている理由と同じ)。
        {static_cast<u32>(x68k::kMainRamSize) + 4u, "A7-4 が窓の末尾ちょうど"},
        // A7-4 が窓の末尾から 2 バイト手前。**4 バイト書くと 2 バイトはみ出す。**
        {static_cast<u32>(x68k::kMainRamSize) + 2u, "A7-4 が窓の末尾 -2"},
    };

    // **.s と .w の両方で回す。** planBranch は 2 つの経路で別々に size を
    // 埋めるので、片方だけ試すともう片方の埋め忘れが素通りする
    // (実際に .w だけ生き残る変異があった)。
    const std::vector<std::vector<u16>> forms = {{bsrShort(4)}, {kBsrWordOp, 0x0010u}};

    for (const Case& c : cases)
        for (const std::vector<u16>& words : forms)
        {
            INFO(std::string(c.what));
            INFO(std::string(words.size() == 1u ? "BSR.s" : "BSR.w"));
            clearSeeds();
            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);

            M68kState s = makeState(5);
            s.a[7] = c.sp;
            resetExecRam(words);
            resetExecGen(0xFFFFFFFFu, 0);
            const std::vector<u8> before = execRam();

            const NativeOutcome native = runEmitted(e, s);
            REQUIRE(native.ok);

            CHECK(native.guardExit);
            CHECK_FALSE(native.branchTaken);
            CHECK_FALSE(native.dynamicBranch);
            // 0 命令ぶんしか進んでいない (BSR はブロックの唯一の命令)。
            CHECK(native.ranOps == 0u);
            // **アーキテクチャ状態は 1 bit も動いていない** (G3)。
            // pc / ir / irc だけは島が「その命令の直前の命令境界」を書く (G7)。
            M68kState want = s;
            want.pc = kEntry + 4u;
            want.ir = words[0];
            want.irc = code.get16(kEntry + 2u);
            compareStates(want, native.state, /*skipPrefetch=*/false, c.what);
            // **A7 は動いていない。** 名指しで固定する (compareStates が
            // 全レジスタを見るが、ここが G3 の本体)。
            CHECK(native.state.a[7] == c.sp);
            compareMemory(before, std::vector<std::uint16_t>(kGenPages, 0), c.what);
        }
    clearSeeds();
}

TEST_CASE("BSR が自ページへ積むなら書く前に脱出する")
{
    // G13: 積む先が plan.page に掛かると、焼いた定数 (出口の ir / irc) が
    // 実行中に古くなる。**書く前に降りる。**
    //
    // 落ちる変異:
    //   - emitBsr から emitSelfPageGuard を消す
    //   - byteOffset 3 のほう (`.l` の後端) だけを消す
    clearSeeds();
    const u32 selfPageBase = kEntry & ~0x3FFu;
    const std::vector<u16> words{bsrShort(4)};

    struct Case
    {
        u32 sp;
        const char* what;
    };
    const std::vector<Case> cases = {
        // A7 - 4 がちょうど自ページの中。
        {selfPageBase + 0x104u, "自ページの中へ積む"},
        // A7 - 4 が手前のページだが、a + 3 が自ページの先頭へ食い込む。
        // **byteOffset 3 のガードでしか捕まらない。**
        {selfPageBase + 2u, "a+3 が自ページへ食い込む"},
        // A7 - 4 が自ページの末尾で、a + 3 は**次のページ**。
        // **byteOffset 0 のガードでしか捕まらない。** 上のケースと対にして
        // 置かないと、2 本のうち片方を消す変異がもう片方に拾われて通る。
        {selfPageBase + 0x402u, "a が自ページの末尾 / a+3 は次ページ"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        M68kState s = makeState(6);
        s.a[7] = c.sp;
        resetExecRam(words);
        resetExecGen(0xFFFFFFFFu, 0);
        const std::vector<u8> before = execRam();

        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);

        CHECK(native.guardExit);
        CHECK(native.selfPageExit);
        CHECK(native.ranOps == 0u);
        M68kState want = s;
        want.pc = kEntry + 4u;
        want.ir = words[0];
        want.irc = code.get16(kEntry + 2u);
        compareStates(want, native.state, /*skipPrefetch=*/false, c.what);
        CHECK(native.state.a[7] == c.sp);
        compareMemory(before, std::vector<std::uint16_t>(kGenPages, 0), c.what);
    }
    clearSeeds();
}

TEST_CASE("BSR は書きの窓と世代配列が無いと発行しない")
{
    // **BSR は -(A7) へ write32 する。** eaMode を持たず
    // isMemoryWriteKind にも入らないので、canEmitWritesFor は見ていない。
    // canEmitBsrFor が別に見ないと、世代配列の外へ s16i する形で焼ける。
    //
    // 落ちる変異:
    //   - measure() から canEmitBsrFor の呼び出しを消す
    //   - canEmitBsrFor の canEmitWritesIn を消す
    clearSeeds();
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan({bsrShort(4)}, plan, code));
    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);

    CHECK(jit::requiredSize(plan, ir, irc, fakeEnv()) > 0u);

    // 世代配列が無い。
    jit::EmitEnv noGen = fakeEnv();
    noGen.genBaseAddr = 0;
    CHECK(jit::requiredSize(plan, ir, irc, noGen) == 0u);

    // ページ数が足りない。
    jit::EmitEnv shortGen = fakeEnv();
    shortGen.genPageCount = kGenPages - 1;
    CHECK(jit::requiredSize(plan, ir, irc, shortGen) == 0u);

    // 窓が無い。
    jit::EmitEnv noWindow = fakeEnv();
    noWindow.ramBaseAddr = 0;
    CHECK(jit::requiredSize(plan, ir, irc, noWindow) == 0u);

    // 窓が 4 バイトの書きを一度も許さない。
    jit::EmitEnv tiny = fakeEnv();
    tiny.ramLimit = 2;
    CHECK(jit::requiredSize(plan, ir, irc, tiny) == 0u);

    // **窓が読めなくても焼ける。** BSR は書き経路だけで、
    // 書き経路は fastRamReadable_ を見ない (m68k.cpp:331-334)。
    jit::EmitEnv unreadable = fakeEnv();
    unreadable.ramReadable = false;
    CHECK(jit::requiredSize(plan, ir, irc, unreadable) > 0u);

    // **メールボックスが無くても焼ける。** BSR は飛び先を静的に持つ。
    //
    // 落ちる変異: canEmitBsrFor で canEmitDynamicBranchIn も問う
    jit::EmitEnv noMailbox = fakeEnv();
    noMailbox.mailboxAddr = 0;
    CHECK(jit::requiredSize(plan, ir, irc, noMailbox) > 0u);
}

TEST_CASE("BSR は必ずブロック末尾になる")
{
    // **I6 / G23。** BSR の後ろに命令があっても積まない。
    // 積むと、飛んだ後に実行されないはずの命令が実行される。
    //
    // 落ちる変異: plan() の静的分岐の break を消す
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{bsrShort(4), moveq(0, 1)}, "BSR.s の後ろ"},
        {{kBsrWordOp, 0x0010u, moveq(0, 1)}, "BSR.w の後ろ"},
        {{moveq(0, 1), bsrShort(4), moveq(1, 2)}, "MOVEQ + BSR + MOVEQ"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        REQUIRE(plan.count >= 1u);
        // 末尾が BSR で、そこで切れていること。
        CHECK(plan.ops[plan.count - 1u].kind == PlanKind::kBsr);
        CHECK(plan.end == BlockEnd::kBranch);
        // **述語そのものも問う。** plan() は hasStaticBranchTarget で
        // 切っているので、isBlockTerminator に kBsr が入っていなくても
        // 上の CHECK は通ってしまう。契約 (「末尾にしか置けない形」) を
        // 名指しで固定しないと、後から誰かが isBlockTerminator を根拠に
        // 判断を書いたときに黙って外れる。
        //
        // 落ちる変異: isBlockTerminator から kBsr を外す
        CHECK(x68k::isBlockTerminator(PlanKind::kBsr));
        // 静的な飛び先を持つ側でもあること。**両方に入るのは Bcc と BSR だけ。**
        CHECK(x68k::hasStaticBranchTarget(PlanKind::kBsr));
        CHECK_FALSE(x68k::hasStaticBranchTarget(PlanKind::kJsr));
        CHECK_FALSE(x68k::hasStaticBranchTarget(PlanKind::kRts));
    }
}

TEST_CASE("BSR は CCR を 1 bit も変えない")
{
    // **groupBranch の BSR 経路は setLogicFlags も testCondition も通らない**
    // (m68k_ops_move.cpp:119-127)。
    for (u16 sr : {u16(0x2700u), u16(0x271Fu), u16(0xA715u), u16(0x0000u)})
    {
        clearSeeds();
        M68kState s = makeState(1);
        s.sr = sr;
        s.a[7] = kStackAddr;
        checkEquivalence({bsrShort(4)}, s, "BSR は CCR を変えない");
    }
    clearSeeds();
}

TEST_CASE("BSR の世代の動きがインタプリタと一致する")
{
    // **write32 は page(a) と page(a+3) の 2 回 touch する** (m68k.cpp:377-378)。
    // 同一ページでも 2 回。畳むと飽和のタイミングが JIT ON/OFF で割れる。
    //
    // 落ちる変異: emitTouch の 2 回目 (byteOffset 3) を消す
    clearSeeds();
    const u32 page = (kStackAddr - 4u) >> 10;
    for (const std::uint16_t before :
         {std::uint16_t(0u), std::uint16_t(5u), std::uint16_t(0xFFFDu), std::uint16_t(0xFFFFu)})
    {
        M68kState s = makeState(3);
        s.a[7] = kStackAddr;
        checkEquivalence({bsrShort(4)}, s, "BSR の世代", fakeEnv(), page, before);
    }

    // ページ境界を跨いで積む形。**2 ページぶん上がること。**
    {
        const u32 crossing = ((kStackAddr >> 10) << 10) + 2u;  // A7-4 = page 先頭 - 2
        M68kState s = makeState(3);
        s.a[7] = crossing;
        checkEquivalence({bsrShort(4)}, s, "BSR がページ境界を跨ぐ");
    }
    clearSeeds();
}

TEST_CASE("BSR を含むブロックが staging に収まる")
{
    // **kMaxBlockBytes (2560) に収まること。** 収まらないと translate が
    // 黙って諦める (素通りする) 形になるので、気づきにくい。
    //
    // 最悪ケース: 一番長い形を 3 つ並べたうえで BSR で終端する。
    // BSR は「ガード 3 本 + touch 2 組 + store 4 本 + 島」を持つ。
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{bsrShort(4)}, "BSR.s 単体"},
        {{kBsrWordOp, 0x0010u}, "BSR.w 単体"},
        {{moveDnToMem(0x2u, 0, kModeInd, 1), moveDnToMem(0x2u, 1, kModeInd, 2),
          moveDnToMem(0x2u, 2, kModeInd, 3), bsrShort(4)},
         "MOVE.l Dn,(An) x3 + BSR.s"},
        {{clrMem(2u, kModeInd, 1), clrMem(2u, kModeInd, 2), clrMem(2u, kModeInd, 3), bsrShort(4)},
         "CLR.l (An) x3 + BSR.s"},
        // **MOVEM の書きが全 Tier で一番長い。** 3 つ並べて BSR で締める。
        {{movemToPreDec(1), 0x000Fu, movemToPreDec(2), 0x000Fu, movemToPreDec(3), 0x000Fu,
          bsrShort(4)},
         "MOVEM.l <4regs>,-(An) x3 + BSR.s"},
    };

    std::size_t worst = 0;
    const char* worstWhat = "";
    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        // 末尾が BSR であること。**そうでないと最悪ケースを測っていない。**
        REQUIRE(plan.ops[plan.count - 1u].kind == PlanKind::kBsr);
        const u16 ir = code.get16(plan.fallThroughPc);
        const u16 irc = code.get16(plan.fallThroughPc + 2);
        const std::size_t need = jit::requiredSize(plan, ir, irc, fakeEnv());
        // **0 なら発行できていない。** 諦めた形を「収まった」と読まない。
        REQUIRE(need > 0u);
        CHECK(need <= x68k::jit::kMaxBlockBytes);
        if (need > worst)
        {
            worst = need;
            worstWhat = c.what;
        }
    }
    INFO("worst = ", worst, " (", std::string(worstWhat), ")");
    // 余裕が 20% 以上あること。命令を 1 つ足しただけで超えるのは危うい。
    CHECK(worst * 5u <= x68k::jit::kMaxBlockBytes * 4u);
}

TEST_CASE("負のキャッシュの kAnyGen は世代を問わず一致する")
{
    // G18 の止血。自ページ脱出した番地は、そのあと step() が書いて
    // 世代が動くので、(pc, gen) 一致の記憶では素通りしてしまう。
    // **世代を鍵から外した印**が要る。
    std::vector<jit::NegEntry> entries(16);
    jit::NegativeCache neg;
    neg.setStorage(entries.data(), static_cast<std::uint32_t>(entries.size()));
    REQUIRE(neg.isReady());

    constexpr std::uint32_t kPc = 0x2000u;

    // ふつうの記憶は世代が動けば外れる。
    neg.insert(kPc, 5);
    CHECK(neg.contains(kPc, 5));
    CHECK_FALSE(neg.contains(kPc, 6));

    // kAnyGen は世代を問わない。
    neg.insert(kPc, jit::NegativeCache::kAnyGen);
    for (std::uint32_t gen = 0; gen < 0x10000u; gen += 0x1111u)
    {
        INFO("gen=", gen);
        CHECK(neg.contains(kPc, static_cast<std::uint16_t>(gen)));
    }
    // 別の番地には効かない。
    CHECK_FALSE(neg.contains(kPc + 2u, 5));

    // **番兵の値は CodeGenMap::kAlwaysStale と同じ。** 流用してよいのは
    // rememberFailure がその世代を決して格納しないから。片方だけ動かすと、
    // 「飽和したページの失敗」が「世代不問」に化ける。
    CHECK(jit::NegativeCache::kAnyGen == x68k::CodeGenMap::kAlwaysStale);

    // clear() で消える (写像が変わったとき呼び出し側がやる)。
    neg.clear();
    CHECK_FALSE(neg.contains(kPc, 5));
}

TEST_CASE("生成コードの世代定数が CodeGenMap と一致する")
{
    // block_emitter.cpp は kAlwaysStale を**数値で書き直している**
    // (kStateDOffset と同じ流儀)。core/ 側が動いたら生成コードは黙って
    // 飽和しなくなるので、ここで縛る。
    //
    // 生成コードの飽和判定は「cur - 0xFFFF == 0 なら据え置き」。
    // その 0xFFFF がバイト列のリテラルとして在ることを問う。
    clearSeeds();
    const std::vector<u16> words{moveDnToMem(0x2u, 1, kModeInd, 2)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));
    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    // リテラルプールは [0, entryOffset) にリトルエンディアンで並んでいる。
    bool sawStale = false;
    for (std::size_t i = 0; i + 4 <= e.info.entryOffset; i += 4)
    {
        std::uint32_t v = 0;
        std::memcpy(&v, e.buffer.data() + i, 4);
        if (v == x68k::CodeGenMap::kAlwaysStale)
        {
            sawStale = true;
        }
    }
    INFO("飽和値 (kAlwaysStale) のリテラルが無い");
    CHECK(sawStale);

    // ページ粒度も一致すること。1KB を 2KB にする変異は、
    // 自ページ判定と touch の宛先を同時にずらす。
    CHECK(x68k::CodeGenMap::kPageShift == 10);
}

TEST_CASE("decodeBlockReturn が符号化を全数で解く")
{
    // 符号化と復号は 1 対でしか意味がないので、境界を全部問う。
    //
    // **サイクルの上限が 23bit になった** (bit23 を kSelfPageExitFlag へ
    // 譲ったため)。1 ブロックの最大は 58 (kMaxOps = 4 の分岐 12 + 非分岐
    // 4 x 3) なので無損失だが、kCycleMask の上端は明示的に問う。
    for (std::uint32_t cycles : {0u, 1u, 58u, jit::kCycleMask})
    {
        {
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles);
            CHECK(r.cycles == cycles);
            CHECK_FALSE(r.branchTaken);
            CHECK_FALSE(r.guardExit);
            CHECK_FALSE(r.selfPageExit);
            CHECK_FALSE(r.dynamicBranch);
            CHECK(r.ranOps == 0);
        }
        {
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles | jit::kBranchTakenFlag);
            CHECK(r.cycles == cycles);
            CHECK(r.branchTaken);
            CHECK_FALSE(r.guardExit);
            CHECK_FALSE(r.selfPageExit);
            CHECK_FALSE(r.dynamicBranch);
        }
        {
            // 動的分岐 (Tier D)。**bit31 とは別のビット**で、
            // runner は「メールボックスを読む」という別の動作をする。
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles | jit::kDynamicBranchFlag);
            CHECK(r.cycles == cycles);
            CHECK(r.dynamicBranch);
            CHECK_FALSE(r.branchTaken);
            CHECK_FALSE(r.guardExit);
            CHECK_FALSE(r.selfPageExit);
            CHECK(r.ranOps == 0);
        }
        for (std::uint32_t k = 0; k < x68k::kMaxOps; ++k)
        {
            const std::uint32_t ret = cycles | jit::kGuardExitFlag | (k << jit::kGuardCountShift);
            const jit::BlockReturn r = jit::decodeBlockReturn(ret);
            CHECK(r.cycles == cycles);
            CHECK(r.guardExit);
            CHECK_FALSE(r.branchTaken);
            CHECK_FALSE(r.selfPageExit);
            CHECK_FALSE(r.dynamicBranch);
            CHECK(r.ranOps == k);
            // 自ページ脱出 (G18): bit30 に**加えて** bit23 が立つ。
            const jit::BlockReturn sp = jit::decodeBlockReturn(ret | jit::kSelfPageExitFlag);
            CHECK(sp.cycles == cycles);
            CHECK(sp.guardExit);
            CHECK(sp.selfPageExit);
            CHECK_FALSE(sp.branchTaken);
            CHECK_FALSE(sp.dynamicBranch);
            CHECK(sp.ranOps == k);
            // **ガード脱出と動的分岐は排他** (G21)。両方立った戻り値を
            // 渡されても、復号側は「ガード脱出」だけを言う。
            //
            // 落ちる変異: r.dynamicBranch から !r.guardExit を外す
            const jit::BlockReturn both = jit::decodeBlockReturn(ret | jit::kDynamicBranchFlag);
            CHECK(both.guardExit);
            CHECK_FALSE(both.dynamicBranch);
            // **k はここで壊れてはいけない。** kGuardCountMask が
            // kDynamicBranchFlag のビットまで含むと、動的分岐の印が
            // 「k に 32 を足した」ものとして読める。
            //
            // 落ちる変異: kGuardCountMask を 0x3F へ戻す
            CHECK(both.ranOps == k);
        }
    }

    // --- ビットの重なりを名指しで問う ---
    //
    // **どれか 2 つが重なると、片方の意味がもう片方に化ける。**
    // 位置を動かす変更は、必ずここを通ってから入る。
    CHECK((jit::kDynamicBranchFlag & jit::kCycleMask) == 0u);
    CHECK((jit::kDynamicBranchFlag & jit::kBranchTakenFlag) == 0u);
    CHECK((jit::kDynamicBranchFlag & jit::kGuardExitFlag) == 0u);
    CHECK((jit::kDynamicBranchFlag & jit::kSelfPageExitFlag) == 0u);
    // **k の欄と重ならないこと。** ここが重なると M9 の変異が通る。
    CHECK((jit::kDynamicBranchFlag & (jit::kGuardCountMask << jit::kGuardCountShift)) == 0u);
    // k の欄が kMaxOps を表しきれること (0..kMaxOps が入る)。
    CHECK(jit::kGuardCountMask >= x68k::kMaxOps);

    // **kSelfPageExitFlag はサイクルの外にある。** kCycleMask と重なると、
    // 自ページ脱出のたびにサイクルが 8,388,608 増えて時間が飛ぶ。
    CHECK((jit::kSelfPageExitFlag & jit::kCycleMask) == 0u);
    // ガード脱出でなければ bit23 は見ない (符号化の契約)。
    CHECK_FALSE(jit::decodeBlockReturn(jit::kSelfPageExitFlag).selfPageExit);
}

TEST_CASE("nextBranch が飛び先の出どころを取り違えない")
{
    // 保証すること: ブロックの戻り値とスロットの状態から決まる「次の飛び先」は、
    // 静的分岐ならスロットの branchTarget、動的分岐ならメールボックス、
    // それ以外なら「飛ばない」であること。
    //
    // **出どころを取り違えても型は通る** (どちらも u32) ので、テストが
    // 名指しで問う以外に守る手が無い。動的分岐スロットの branchTarget は
    // 設計上つねに 0 なので、取り違えると branchTo(0) — リセットベクタ相当へ
    // 飛ぶ。runner 本体は runBlock (ESP32 のアセンブリ) に依存してホストで
    // 走らせられないので、判断の部分だけを純関数として問う。
    constexpr std::uint32_t kStaticTarget = 0x00FC0100u;
    constexpr std::uint32_t kMailbox = 0x00001234u;

    for (std::uint32_t cycles : {0u, 12u, 58u})
    {
        SUBCASE("静的分岐が成立したらスロットの branchTarget へ飛ぶ")
        {
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles | jit::kBranchTakenFlag);
            const jit::BranchDecision d = jit::nextBranch(r, kStaticTarget, kMailbox);
            CHECK(d.shouldBranch());
            // 落ちる変異 (M10 の裏): 出どころをメールボックスにする
            CHECK(d.source == jit::BranchSource::kStaticTarget);
            // 落ちる変異 (M13): staticTarget + 2 を返す
            CHECK(d.target == kStaticTarget);
            CHECK(d.target != kMailbox);
        }

        SUBCASE("動的分岐ならメールボックスへ飛ぶ (branchTarget ではない)")
        {
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles | jit::kDynamicBranchFlag);
            const jit::BranchDecision d = jit::nextBranch(r, kStaticTarget, kMailbox);
            // 落ちる変異 (M12): dynamicBranch を一切ディスパッチしない
            CHECK(d.shouldBranch());
            // 落ちる変異 (M10): 出どころを staticTarget にする
            CHECK(d.source == jit::BranchSource::kMailbox);
            CHECK(d.target == kMailbox);
            CHECK(d.target != kStaticTarget);
        }

        SUBCASE("動的分岐スロットの branchTarget は 0 なので取り違えは 0 番地行きになる")
        {
            // 実機で起きる形をそのまま置く。動的分岐のスロットは
            // branchTarget を埋めない (翻訳時に飛び先が決まらない)。
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles | jit::kDynamicBranchFlag);
            const jit::BranchDecision d = jit::nextBranch(r, 0u, kMailbox);
            CHECK(d.shouldBranch());
            CHECK(d.source == jit::BranchSource::kMailbox);
            // **ここが 0 になったら実機はリセットベクタへ飛ぶ。**
            CHECK(d.target == kMailbox);
            CHECK(d.target != 0u);
        }

        SUBCASE("分岐しないブロックはどこへも飛ばない")
        {
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles);
            const jit::BranchDecision d = jit::nextBranch(r, kStaticTarget, kMailbox);
            CHECK_FALSE(d.shouldBranch());
            CHECK(d.source == jit::BranchSource::kNone);
        }

        SUBCASE("ガード脱出はどこへも飛ばない")
        {
            // 途中で降りているので、分岐の判断はそもそも走っていない。
            for (std::uint32_t k = 0; k <= x68k::kMaxOps; ++k)
            {
                const std::uint32_t base =
                    cycles | jit::kGuardExitFlag | (k << jit::kGuardCountShift);
                for (std::uint32_t extra : {0u, jit::kSelfPageExitFlag})
                {
                    const jit::BlockReturn r = jit::decodeBlockReturn(base | extra);
                    const jit::BranchDecision d = jit::nextBranch(r, kStaticTarget, kMailbox);
                    CHECK_FALSE(d.shouldBranch());
                    CHECK(d.source == jit::BranchSource::kNone);
                }
            }
        }
    }

    // --- 排他性: 出どころは戻り値の全空間で高々 1 つ ---
    //
    // 4 つのフラグの全組み合わせを回し、「静的とメールボックスが同時に
    // 選ばれることはない」「ガード脱出が立っていたら必ず飛ばない」を問う。
    // decodeBlockReturn が排他を保っているので nextBranch は素直に書けるが、
    // **どちらか片方だけ緩めた変異**をここで殺す。
    for (std::uint32_t bits = 0; bits < 16u; ++bits)
    {
        std::uint32_t ret = 42u;
        if ((bits & 1u) != 0)
        {
            ret |= jit::kBranchTakenFlag;
        }
        if ((bits & 2u) != 0)
        {
            ret |= jit::kGuardExitFlag;
        }
        if ((bits & 4u) != 0)
        {
            ret |= jit::kDynamicBranchFlag;
        }
        if ((bits & 8u) != 0)
        {
            ret |= jit::kSelfPageExitFlag;
        }
        const jit::BlockReturn r = jit::decodeBlockReturn(ret);
        const jit::BranchDecision d = jit::nextBranch(r, kStaticTarget, kMailbox);

        // ガード脱出が立っていたら、他に何が立っていても飛ばない。
        if (r.guardExit)
        {
            CHECK(d.source == jit::BranchSource::kNone);
        }
        // 飛ぶなら、target は必ずその出どころの値と一致する。
        // **どちらの出どころでも他方の値にはならない。**
        if (d.source == jit::BranchSource::kStaticTarget)
        {
            CHECK(d.target == kStaticTarget);
            CHECK(d.target != kMailbox);
            CHECK(r.branchTaken);
        }
        if (d.source == jit::BranchSource::kMailbox)
        {
            CHECK(d.target == kMailbox);
            CHECK(d.target != kStaticTarget);
            CHECK(r.dynamicBranch);
            // **静的が立っていたら動的は選ばれない。** 生成コードは
            // bit31 と bit29 を同時に立てないので実機では起きないが、
            // 起きても静的が勝つ (メールボックスは書かれていない)。
            CHECK_FALSE(r.branchTaken);
        }
        // 飛ばないと決めたなら、分岐フラグはどちらも立っていない
        // (ガード脱出でない限り)。**ディスパッチ漏れをここで殺す。**
        if (!d.shouldBranch() && !r.guardExit)
        {
            CHECK_FALSE(r.branchTaken);
            CHECK_FALSE(r.dynamicBranch);
        }
    }

    // constexpr で解けること。**実行時の分岐に化けていないこと**を型で問う。
    static_assert(
        jit::nextBranch(jit::decodeBlockReturn(jit::kBranchTakenFlag), 0x1000u, 0x2000u).target ==
        0x1000u);
    static_assert(
        jit::nextBranch(jit::decodeBlockReturn(jit::kDynamicBranchFlag), 0x1000u, 0x2000u).target ==
        0x2000u);
    static_assert(!jit::nextBranch(jit::decodeBlockReturn(0u), 0x1000u, 0x2000u).shouldBranch());
}

TEST_CASE("ガードより前に状態を書く命令が 1 つも無い")
{
    // G3 の機械検査。**バイト列を走査して確かめる。**
    //
    // 同値テストは「ガードが不成立になった場合」にしか commit の順序を
    // 問えないが、こちらは発行されたコードそのものを見るので、
    // 不成立を作れない形でも順序を固定できる。
    //
    // 生成コードが状態を書くのは kState (a3) 基底の s32i / s16i だけ。
    // 先頭から最初の bnez (ガードの分岐) までの間に、それが 1 つも
    // 現れないことを問う。
    for (u32 group : {0x1u, 0x2u, 0x3u})
    {
        for (u32 mode : {kModeInd, kModePostInc, kModePreDec})
        {
            const std::vector<u16> words{moveMemToDn(group, 1, mode, 2)};
            FlatCode code;
            BlockPlan plan{};
            REQUIRE(buildPlan(words, plan, code));
            const EmitResult e = emit(plan, code);
            REQUIRE(e.ok);

            // 最初の bnez を探す。BRI12: op0=0x6 / t=0x5。
            // **命令の切れ目に沿って歩く。** バイトごとに見ると、3 バイト
            // 命令の途中が別の正当な命令に化けて偽の検出になる。
            std::size_t guardAt = e.buffer.size();
            for (std::size_t i = e.info.entryOffset; i + 2 <= e.buffer.size();
                 i += xtensaInsnLength(e.buffer[i]))
            {
                const bool isBnez =
                    (e.buffer[i] & 0x0Fu) == 0x6u && ((e.buffer[i] >> 4) & 0x0Fu) == 0x5u;
                if (isBnez)
                {
                    guardAt = i;
                    break;
                }
            }
            INFO("group=", group, " mode=", mode);
            REQUIRE(guardAt < e.buffer.size());

            // そこまでに kState 基底のストアが無いこと。
            //
            // s32i at, a3, off = RRI8 op0=2 / r=6 / s=3
            // s16i at, a3, off = RRI8 op0=2 / r=5 / s=3
            // s32i.n at, a3, off = RRRN op0=9 / s=3
            bool wrote = false;
            for (std::size_t i = e.info.entryOffset; i < guardAt;
                 i += xtensaInsnLength(e.buffer[i]))
            {
                const std::uint32_t op0 = e.buffer[i] & 0x0Fu;
                const std::uint32_t s = e.buffer[i + 1] & 0x0Fu;
                const std::uint32_t r = (e.buffer[i + 1] >> 4) & 0x0Fu;
                const bool isWideStore = op0 == 0x2u && s == 3u && (r == 6u || r == 5u);
                const bool isNarrowStore = op0 == 0x9u && s == 3u;
                if (isWideStore || isNarrowStore)
                {
                    wrote = true;
                    break;
                }
            }
            CHECK_FALSE(wrote);
        }
    }
}

TEST_CASE("動的分岐が最後のガードより前に状態もメモリも書かない")
{
    // **G3 の Tier D 版。** 同値テストは「ガードが不成立になった場合」に
    // しか順序を問えないが、こちらは発行されたコードそのものを走査するので、
    // 不成立を作りにくい形でも順序を固定できる。
    //
    // 動的分岐が守るべきものは 3 つあり、**どれも最後のガードより後**:
    //   a[7] への s32i         RTS は +4、JSR は -4
    //   ゲスト RAM への s8i    JSR が積む戻り先
    //   世代配列への s16i      JSR の touch
    //
    // 落ちる変異:
    //   RTS の戻り先の整列ガードを A7 の更新より後ろへ動かす
    //   JSR の飛び先の整列ガードを積んだ後ろへ動かす
    //   JSR の自ページガードを touch より後ろへ動かす
    struct Case
    {
        std::vector<u16> words;
        // ガードの本数。**この数だけ分岐を数えてから最後のものを取る。**
        // 途中の分岐で切ると、後ろのガードより前の commit を見逃す。
        std::size_t guardCount;
        const char* what;
    };
    const std::vector<Case> cases = {
        // RTS: 範囲 (bnez) + 戻り先の整列 (bnez) の 2 本。
        {{kRtsOp}, 2u, "RTS"},
        // JSR: 飛び先の整列 (bnez) + 範囲 (bnez) + 自ページ x2 (beqz) の 4 本。
        {{jsr(kModeInd, 3)}, 4u, "JSR (An)"},
        {{jsr(kModeDisp, 4), 0x0100u}, 4u, "JSR (d16,An)"},
        {{jsr(kModeAbsL, 1), 0x0006u, 0x0000u}, 4u, "JSR (xxx).L"},
    };

    for (const Case& c : cases)
    {
        INFO(std::string(c.what));
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        REQUIRE(plan.end == BlockEnd::kDynamicBranch);
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        // ガードの分岐を順に数え、**最後の 1 本**の位置を取る。
        //
        // BRI12: op0 = 0x6。bnez は t = 0x5、beqz は t = 0x1。
        // 出口の島も分岐を持たないので (retN で戻るだけ)、本体の分岐は
        // ガードだけ。**数が合わないなら形が変わっている**ので REQUIRE で問う。
        std::size_t lastGuardAt = e.buffer.size();
        std::size_t seen = 0;
        for (std::size_t i = e.info.entryOffset; i + 2 <= e.buffer.size();
             i += xtensaInsnLength(e.buffer[i]))
        {
            const bool isBri12 = (e.buffer[i] & 0x0Fu) == 0x6u;
            if (!isBri12)
            {
                continue;
            }
            const std::uint32_t t = (e.buffer[i] >> 4) & 0x0Fu;
            const bool isGuardBranch = t == 0x5u || t == 0x1u;
            if (!isGuardBranch)
            {
                continue;
            }
            ++seen;
            lastGuardAt = i;
            if (seen == c.guardCount)
            {
                break;
            }
        }
        INFO("guards seen = ", seen);
        REQUIRE(seen == c.guardCount);
        REQUIRE(lastGuardAt < e.buffer.size());

        // そこまでに commit が 1 つも無いこと。
        //
        // s32i  at, as, off = RRI8 op0=2 / r=6   (a[7] / メールボックス)
        // s16i  at, as, off = RRI8 op0=2 / r=5   (世代配列)
        // s8i   at, as, off = RRI8 op0=2 / r=4   (ゲスト RAM)
        // s32i.n at, as, off = RRRN op0=9        (a[7] の短縮形)
        //
        // **基底レジスタを問わない。** Tier B/C の走査は kState (a3) 基底だけを
        // 見ていたが、動的分岐はゲスト RAM と世代配列にも書くので、
        // 基底で絞ると「窓へ書く命令」を見逃す。
        const char* found = nullptr;
        for (std::size_t i = e.info.entryOffset; i < lastGuardAt;
             i += xtensaInsnLength(e.buffer[i]))
        {
            const std::uint32_t op0 = e.buffer[i] & 0x0Fu;
            const std::uint32_t r = (e.buffer[i + 1] >> 4) & 0x0Fu;
            if (op0 == 0x2u && r == 6u)
            {
                found = "s32i";
                break;
            }
            if (op0 == 0x2u && r == 5u)
            {
                found = "s16i";
                break;
            }
            if (op0 == 0x2u && r == 4u)
            {
                found = "s8i";
                break;
            }
            if (op0 == 0x9u)
            {
                found = "s32i.n";
                break;
            }
        }
        INFO("store before last guard: ", std::string(found == nullptr ? "none" : found));
        CHECK(found == nullptr);
    }
}

TEST_CASE("ガード脱出の irc が実メモリの mem16(opPc + 2) と一致する")
{
    // I11: 出口の irc は導出値なので、**実メモリの語と突き合わせる**。
    //
    // 導出は 3 通り (次命令語 / 第 1 拡張ワード / long の上位語) に分かれる。
    // どれか 1 つを間違えても、その形の命令が脱出したときにしか現れない。
    // ここで全ての EA 形について、脱出後の irc を参照実行の irc と比べる。
    const u32 outside = static_cast<u32>(x68k::kMainRamSize) + 0x1000u;

    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const std::vector<Case> cases{
        // 長さ 2: opPc + 2 は次の命令語 (ここでは後続の NOP)
        {{moveMemToDn(0x2u, 1, kModeInd, 4)}, "(An) 単独"},
        {{moveMemToDn(0x2u, 1, kModePostInc, 4)}, "(An)+ 単独"},
        // 長さ 2 の**後ろに別の命令が続く**形。
        //
        // ここが要点: 脱出する命令がブロックの最後だと、irc は
        // fallThroughIr でも ops[k+1].op でも同じ語になってしまい、
        // 導出の分岐を区別できない。**後続の命令語が NOP 埋めと
        // 違う値**になるように MOVEQ を置いて、取り違えを見えるようにする。
        {{moveMemToDn(0x2u, 1, kModeInd, 4), moveq(5, 0x42)}, "(An) の後ろに MOVEQ"},
        {{moveq(0, 3), moveMemToDn(0x2u, 1, kModeInd, 4), moveq(6, 0x21)},
         "2 命令目が (An)、その後ろにも命令"},
        // 長さ 4: opPc + 2 は第 1 拡張ワード
        {{moveMemToDn(0x2u, 1, kModeDisp, 4), 0x1234u}, "(d16,An)"},
        {{moveMemToDn(0x2u, 1, kModeDisp, 4), 0x1234u, moveq(7, 0x33)}, "(d16,An) の後ろに MOVEQ"},
    };

    for (const Case& c : cases)
    {
        M68kState s = makeState(11);
        s.a[4] = outside;
        // checkEquivalence が pc / ir / irc をまとめて参照側と比べる。
        // **脱出したことも確かめる** (脱出しないと irc の導出を通らない)。
        checkEquivalence(c.words, s, c.what);

        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        std::fill(execRam().begin(), execRam().end(), 0);
        const NativeOutcome native = runEmitted(e, s);
        REQUIRE(native.ok);
        INFO(std::string(c.what));
        CHECK(native.guardExit);
    }
}

TEST_CASE("0 進捗のガード脱出はブロックの成功にしない")
{
    // G10: k == 0 は「状態を 1 bit も変えずに降りた」なので、
    // 呼び出し側は kDeferToStep を返さなければならない。
    //
    // **これが無いと Machine::run が used == 0 を halted と誤読する**か、
    // 同じブロックを 0 サイクルで回し続ける。runner 本体は runBlock
    // (ESP32 のアセンブリ) に依存してホストで走らせられないので、
    // 判断の部分だけを純関数として問う。
    for (std::uint32_t cycles : {0u, 4u, 12u})
    {
        const jit::BlockReturn zero =
            jit::decodeBlockReturn(cycles | jit::kGuardExitFlag | (0u << jit::kGuardCountShift));
        CHECK(zero.guardExit);
        CHECK_FALSE(jit::guardExitMadeProgress(zero));

        for (std::uint32_t k = 1; k < x68k::kMaxOps; ++k)
        {
            const jit::BlockReturn some =
                jit::decodeBlockReturn(cycles | jit::kGuardExitFlag | (k << jit::kGuardCountShift));
            CHECK(some.guardExit);
            CHECK(jit::guardExitMadeProgress(some));
        }
    }
}

TEST_CASE("kMaxLiterals を超える計画は発行しない")
{
    // リテラルが溢れたら false を返し、**中途半端なコードを渡さない**こと。
    // 現行の発行器では kMaxOps = 4 の範囲で溢れないので、capacity を
    // 削ることで「書けない」側を起こす。
    const std::vector<u16> words{moveq(0, 1), moveq(1, 2), moveq(2, 3), moveq(3, 4)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));

    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);
    const std::size_t need = jit::requiredSize(plan, ir, irc, fakeEnv());
    REQUIRE(need > 0);

    std::vector<std::uint8_t> buf(need, 0xCC);
    jit::EmittedBlock info{};
    // 1 バイト足りないと必ず断ること。
    CHECK_FALSE(jit::emitBlock(plan, ir, irc, fakeEnv(), buf.data(), need - 1, info));
    CHECK(jit::emitBlock(plan, ir, irc, fakeEnv(), buf.data(), need, info));
}

TEST_CASE("空の計画は発行しない")
{
    BlockPlan plan{};
    plan.count = 0;
    CHECK(jit::requiredSize(plan, 0, 0, fakeEnv()) == 0);
    std::uint8_t buf[64];
    jit::EmittedBlock info{};
    CHECK_FALSE(jit::emitBlock(plan, 0, 0, fakeEnv(), buf, sizeof(buf), info));

    // count が壊れている計画も断る (§4(d) のゴミ検査と同じ趣旨)。
    plan.count = x68k::kMaxOps + 1;
    CHECK(jit::requiredSize(plan, 0, 0, fakeEnv()) == 0);
    CHECK_FALSE(jit::emitBlock(plan, 0, 0, fakeEnv(), buf, sizeof(buf), info));
}

TEST_CASE("ADD.b / SUB.b / CMP.b のフラグを全数で突き合わせる")
{
    // **V と C を分岐なしのビット演算で出す部分は、式を読んで納得しても
    // 根拠にならない。** byte なら被演算子の組み合わせが 65,536 通りしかないので、
    // 全部を alu::add / alu::sub と突き合わせる。
    //
    // 落ちる変異:
    //   - ADD の C の式から (d | s) & ~r の項を落とす
    //   - SUB の V を (d ^ s) & (r ^ s) にする (r ^ d が正しい)
    //   - 切り出し (extui) を省いて上位バイトの桁を拾わせる
    //   - X を CMP でも書く / ADD で書かない
    struct Variant
    {
        u32 group;
        PlanAluOp op;
        bool writesX;
        bool writesValue;
        const char* what;
    };
    static const Variant kVariants[] = {
        {0xDu, PlanAluOp::kAdd, true, true, "ADD.b"},
        {0x9u, PlanAluOp::kSub, true, true, "SUB.b"},
        {0xBu, PlanAluOp::kCmp, false, false, "CMP.b"},
    };

    for (const Variant& v : kVariants)
    {
        // 発行は 1 回だけ。65,536 通りは初期状態を差し替えて回す。
        const std::vector<u16> words{aluReg(v.group, 2, 0u, 4)};
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        u32 padAt = kEntry + static_cast<u32>(words.size()) * 2u;
        for (u32 i = 0; i < 8; ++i)
        {
            code.put16(padAt + i * 2, static_cast<u16>(0x4E71u));
        }
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        std::size_t mismatches = 0;
        u32 firstD = 0;
        u32 firstS = 0;
        for (u32 d = 0; d < 256 && mismatches == 0; ++d)
        {
            for (u32 s = 0; s < 256; ++s)
            {
                M68kState st = makeState(1);
                // 上位バイトに 1 を敷き詰める。**切り出しを忘れたら必ず落ちる。**
                st.d[2] = 0xAABBCC00u | d;
                st.d[4] = 0x11223300u | s;
                st.sr = static_cast<u16>(0x2700u | 0x10u);  // X を立てておく

                const NativeOutcome got = runEmitted(e, st);
                if (!got.ok)
                {
                    mismatches = 1;
                    firstD = d;
                    firstS = s;
                    break;
                }

                // 参照は alu の関数そのもの。インタプリタを 65,536 回立てると
                // 遅すぎるので、ここでは applyFlags と同じ組み立てを再現する。
                const x68k::alu::Result r = v.op == PlanAluOp::kAdd
                                                ? x68k::alu::add(d, s, x68k::alu::kByte)
                                                : x68k::alu::sub(d, s, x68k::alu::kByte);
                u16 wantSr = static_cast<u16>(st.sr & 0xFFF0u);
                if (r.n)
                {
                    wantSr = static_cast<u16>(wantSr | 0x08u);
                }
                if (r.z)
                {
                    wantSr = static_cast<u16>(wantSr | 0x04u);
                }
                if (r.v)
                {
                    wantSr = static_cast<u16>(wantSr | 0x02u);
                }
                if (r.c)
                {
                    wantSr = static_cast<u16>(wantSr | 0x01u);
                }
                if (v.writesX)
                {
                    wantSr = static_cast<u16>(wantSr & 0xFFEFu);
                    if (r.c)
                    {
                        wantSr = static_cast<u16>(wantSr | 0x10u);
                    }
                }
                const u32 wantD2 =
                    v.writesValue ? ((st.d[2] & 0xFFFFFF00u) | (r.value & 0xFFu)) : st.d[2];

                if (got.state.sr != wantSr || got.state.d[2] != wantD2)
                {
                    ++mismatches;
                    firstD = d;
                    firstS = s;
                    break;
                }
            }
        }
        INFO(std::string(v.what), " d=", firstD, " s=", firstS);
        CHECK(mismatches == 0);
    }
}

TEST_CASE("ADD.w / SUB.w の桁上がりが上位バイトへ漏れない")
{
    // word でも同じ形で問う。全数 (2^32) は回せないので、C と V が
    // 切り替わる境界だけを選ぶ。**切り出しを忘れたときにだけ落ちる値**を含む。
    static constexpr u32 kEdges[] = {
        0x0000u, 0x0001u, 0x7FFFu, 0x8000u, 0x8001u, 0xFFFEu, 0xFFFFu, 0x00FFu, 0x0100u,
    };
    for (u32 group : {0xDu, 0x9u, 0xBu})
    {
        const std::vector<u16> words{aluReg(group, 2, 1u, 4)};
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        u32 padAt = kEntry + 2u;
        for (u32 i = 0; i < 8; ++i)
        {
            code.put16(padAt + i * 2, static_cast<u16>(0x4E71u));
        }
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);

        for (u32 d : kEdges)
        {
            for (u32 s : kEdges)
            {
                M68kState st = makeState(2);
                st.d[2] = 0xDEAD0000u | d;
                st.d[4] = 0xBEEF0000u | s;
                st.sr = 0x2700u;
                checkEquivalence(words, st, "ALU.w の境界");
            }
        }
    }
}

TEST_CASE("kMaxOps いっぱいのブロックがリテラルを使い切らない")
{
    // 1 命令あたりのリテラル消費が増えると、**正しさは保ったまま被覆率だけが
    // 落ちる** (発行を諦めるブロックが増える)。落ちても既存のテストは全部通るので、
    // ここで明示的に問う。
    //
    // 落ちる変異: Emitter::literalIndex の共有をやめる (同じ定数を毎回積む)。
    struct Case
    {
        std::vector<u16> words;
        const char* what;
    };
    const std::vector<Case> cases = {
        // 最も定数を食う形: ADD.b x4 (サイズマスク + CCR マスク + 上位保存マスク)。
        {{aluReg(0xDu, 0, 0, 1), aluReg(0xDu, 2, 0, 3), aluReg(0xDu, 4, 0, 5),
          aluReg(0xDu, 6, 0, 7)},
         "ADD.b x4"},
        {{aluReg(0x9u, 0, 1, 1), aluReg(0xBu, 2, 1, 3), aluReg(0xCu, 4, 1, 5),
          aluReg(0x8u, 6, 1, 7)},
         "SUB/CMP/AND/OR .w x4"},
        {{moveReg(0x1u, 0, 1), moveReg(0x3u, 2, 3), moveReg(0x1u, 4, 5), moveReg(0x3u, 6, 7)},
         "MOVE.b/.w x4"},
    };

    for (const Case& c : cases)
    {
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(c.words, plan, code));
        REQUIRE(plan.count == x68k::kMaxOps);
        INFO(std::string(c.what));
        CHECK(jit::requiredSize(plan, code.get16(plan.fallThroughPc),
                                code.get16(plan.fallThroughPc + 2), fakeEnv()) > 0);

        // 実際に走らせても一致すること。
        u32 padAt = kEntry + static_cast<u32>(c.words.size()) * 2u;
        for (u32 i = 0; i < 8; ++i)
        {
            code.put16(padAt + i * 2, static_cast<u16>(0x4E71u));
        }
        checkEquivalence(c.words, makeState(21), c.what);
    }
}

TEST_CASE("リテラルの実使用量に余裕があること")
{
    // requiredSize の内訳から、リテラル領域が何語になったかを逆算する。
    // **kMaxLiterals にどれだけ近いか**を明示的に見ておかないと、
    // 「まだ入る」のか「たまたま 1 語だけ空いていた」のか分からない。
    //
    // 落ちる変異: Emitter::literalIndex の共有をやめる (同じ定数を毎回積む)。
    const std::vector<std::vector<u16>> worst = {
        {aluReg(0xDu, 0, 0, 1), aluReg(0xDu, 2, 0, 3), aluReg(0xDu, 4, 0, 5),
         aluReg(0xDu, 6, 0, 7)},
        {aluReg(0x9u, 0, 1, 1), aluReg(0xBu, 2, 1, 3), aluReg(0xCu, 4, 1, 5),
         aluReg(0x8u, 6, 1, 7)},
        {moveReg(0x1u, 0, 1), moveReg(0x3u, 2, 3), moveReg(0x1u, 4, 5), moveReg(0x3u, 6, 7)},
    };
    for (const std::vector<u16>& words : worst)
    {
        FlatCode code;
        BlockPlan plan{};
        REQUIRE(buildPlan(words, plan, code));
        const EmitResult e = emit(plan, code);
        REQUIRE(e.ok);
        // entryOffset がリテラル領域の長さそのもの。
        const std::size_t literalWords = e.info.entryOffset / 4u;
        INFO("literal words = ", literalWords);
        // 半分以下に収まっていること。ここを超えたら kMaxLiterals を
        // 増やすか、定数の作り方を見直す。
        CHECK(literalWords <= x68k::jit::kMaxLiterals / 2u);
        CHECK(e.info.entryOffset % 4u == 0u);
    }
}

TEST_CASE("ExecMemory が 32bit 整列で切り出し、容量を超えたら断る")
{
    // 実機では MALLOC_CAP_EXEC、ホストでは整列付きの new から取る。
    // **整列と容量の判断は両方で同じコードを回す。**
    //
    // 整列を外すと l32r のリテラルが 4 バイト境界を跨ぎ、変位の計算
    // ((pc + 3) & ~3 が基準) と実際の語の位置がずれる。「別の正当な定数」を
    // 読むので、演算結果が静かに間違う形でしか出てこない。
    x68k::jit::ExecMemory mem;
    REQUIRE(mem.acquire(256));
    CHECK(mem.isReady());
    CHECK(mem.capacity() == 256);
    CHECK(mem.used() == 0);

    // 中途半端な長さを続けて切り出しても、必ず 4 の倍数の位置から始まること。
    std::uint8_t* prev = nullptr;
    for (std::size_t n : {3u, 1u, 7u, 13u, 2u})
    {
        std::uint8_t* p = mem.allocate(n);
        REQUIRE(p != nullptr);
        INFO("size ", n);
        CHECK((reinterpret_cast<std::uintptr_t>(p) % 4u) == 0u);
        CHECK(p != prev);
        prev = p;
    }
    CHECK(mem.used() <= mem.capacity());

    // 容量を超えたら nullptr。**部分的に切り出さない。**
    const std::size_t left = mem.capacity() - mem.used();
    CHECK(mem.allocate(left + 1) == nullptr);
    CHECK(mem.allocate(left) != nullptr);
    CHECK(mem.allocate(4) == nullptr);

    // 作り直せること。
    mem.reset();
    CHECK(mem.used() == 0);
    CHECK(mem.allocate(256) != nullptr);

    // 二重に確保しない。
    CHECK_FALSE(mem.acquire(64));

    // 確保していない ExecMemory は必ず断る (JIT を無効にしたまま動く経路)。
    x68k::jit::ExecMemory empty;
    CHECK_FALSE(empty.isReady());
    CHECK(empty.allocate(4) == nullptr);
    CHECK_FALSE(empty.acquire(0));
}

TEST_CASE("発行したコードを ExecMemory へ置ける")
{
    // 発行 → 実行可能メモリへ切り出し → 書き込み、までが繋がること。
    // **実際に呼ぶのは実機だけ**なので、ここで問えるのは「置けること」まで。
    x68k::jit::ExecMemory mem;
    REQUIRE(mem.acquire(4096));

    const std::vector<u16> words{moveq(0, 7), aluReg(0xDu, 1, 2, 0), bcc(0x6u, 6)};
    FlatCode code;
    BlockPlan plan{};
    REQUIRE(buildPlan(words, plan, code));

    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);
    const std::size_t need = jit::requiredSize(plan, ir, irc, fakeEnv());
    REQUIRE(need > 0);

    std::uint8_t* slot = mem.allocate(need);
    REQUIRE(slot != nullptr);
    jit::EmittedBlock info{};
    REQUIRE(jit::emitBlock(plan, ir, irc, fakeEnv(), slot, need, info));
    CHECK(info.totalSize == need);
    CHECK(info.endsWithBranch);
    CHECK(info.branchTarget == plan.branchTarget);
    // エントリポイントは 4 バイト整列の領域からの相対で、リテラル領域の直後。
    CHECK(info.entryOffset % 4u == 0u);
    CHECK(info.entryOffset < info.totalSize);

    // 同じ内容がホストのバッファへ出したものと一致すること
    // (切り出し先が変わっても発行結果が変わらない = 位置独立)。
    const EmitResult host = emit(plan, code);
    REQUIRE(host.ok);
    CHECK(host.buffer.size() == need);
    CHECK(std::memcmp(slot, host.buffer.data(), need) == 0);
}

// ============================================================================
// BlockRunner の勘定と回復 — 「動いているが効いていない」を検出する
// ============================================================================
//
// ## ここで問うのは「正しさ」ではない
//
// このファイルの他のテストは全部「生成コードがインタプリタと同じ状態を作るか」
// を問うている。**それが 694 件あって、以下の 3 つの欠陥を 1 件も検出できなかった。**
//
//   1. コード領域が満杯になると二度と回復せず、翻訳器が永久に停止していた
//      (実機 45 秒で「諦めた」14,815,821 回のうち 99.997% がこれ)
//   2. スロット索引が下位ビットだけで畳んでいて、1KB 周期の番地が全部衝突
//   3. 鍵外れの理由の帰属順序が誤っていて、衝突 151 万件が「飽和」に化けていた
//
// **3 つとも正しさを 1 ビットも壊さない。** 諦めた分はインタプリタが実行するので
// 状態は常に正しい。統計の帰属が狂っても状態は正しい。だから状態を比べる
// テストでは永遠に見えない (MEMORY.md「保守的なフォールバックはテストの盲点になる」)。
//
// 見えるようにするには問いを変えるしかない。以下の 3 種類を使う。
//
//   **勘定の保存** — 諦めた回数が理由別の内訳と一致するか (T1)。
//     破れていれば「どのカウンタにも乗らない早期 return がある」ことを意味する。
//     欠陥 1 と 3 はどちらもこの形で、**この 1 本だけで 45 秒ログの時点で
//     機械的に暴けたはず**だった。
//   **生存性** — 悪い状態に入った後、有限回以内に抜け出せるか (T2/T3)。
//     「正しく諦め続ける」は正しさのテストを全部通る。抜け出すことは別に問う。
//   **分散** — 索引が実際に散っているか (T4)。
//
// ## ホストで実行できないものを、どうやってホストで検査するか
//
// **runBlock はホストで std::abort() する** (exec_memory.cpp)。吐くのは Xtensa の
// 機械語なので、Mac の CPU に食わせるわけにいかない。つまり
// **翻訳が成功した瞬間に run() はホストで落ちる。**
//
// そこで、以下のテストは全部「翻訳が成功しない」状態で run() を回す。
// 窓を読めなくしておく (setFastRamReadable(false)) と peekCodeWord が false を
// 返し、BlockPlanner::plan がそこで諦める。翻訳は必ず失敗し、run() は
// runBlock へ届く前に kDeferToStep で戻る。
//
// **それでも欠陥 1/2/3 の経路は全部通る。** 満杯の判定も、スロット索引も、
// 鍵外れの帰属も、翻訳の成否より手前にあるからである。

namespace runner_accounting
{

// 諦めた回数の内訳。**この 3 つの和が deferUnsupported と一致しなければ
// ならない** (T1)。
//
// run() が nullptr を受け取って deferUnsupported を数える経路は 1 つしかなく、
// その手前で translate() が nullptr を返す理由は
//   満杯 (fullDeferred) / 負の記憶 (negativeHit) / 翻訳失敗 (translateFail)
// の 3 つで尽きている。**尽きていなければ、どのカウンタにも乗らない
// 早期 return があるということ。**
std::uint64_t deferBreakdown(const x68k::NativeStats& s)
{
    return s.negativeHit + s.translateFail + s.fullDeferred;
}

// **勘定が閉じているか。** 全シナリオの終端で必ず呼ぶ。
void checkDeferAccounting(const x68k::NativeStats& s, const char* what)
{
    INFO(std::string(what));
    INFO("deferUnsupported=", s.deferUnsupported, " negativeHit=", s.negativeHit,
         " translateFail=", s.translateFail, " fullDeferred=", s.fullDeferred);
    CHECK(s.deferUnsupported == deferBreakdown(s));
}

// BlockRunner を回すための最小の台。
//
// **Machine を丸ごと立てる。** インタプリタとまったく同じ CPU に対して
// NativeExec として刺さる形でしか、run() の入口の事前条件
// (pc == 命令語 + 4、mustDeferToStep、mappingEpoch) を本物にできない。
struct Harness
{
    static constexpr std::uint32_t kSlots = 512;
    // 世代を直接叩いて飽和を作るためのページ数。実機と同じ 1KB ページ。
    static constexpr std::uint32_t kPages = kGenPages;

    x68k::Machine machine;
    std::vector<u8> ram;
    std::vector<std::uint16_t> gen;
    std::vector<x68k::jit::BlockSlot> slots;
    std::vector<x68k::jit::NegEntry> negEntries;
    x68k::jit::ExecMemory code;
    x68k::jit::BlockRunner runner;

    // codeBytes を小さくすると、少ない翻訳で満杯にできる。
    explicit Harness(std::size_t codeBytes = 65536)
        : ram(x68k::kMainRamSize, 0), gen(kPages, 0), slots(kSlots), negEntries(256)
    {
        x68k::MemoryMap map{};
        map.mainRam = ram.data();
        machine.setMemory(map);
        machine.reset();
        machine.cpu().codeGenMap().setStorage(gen.data(), static_cast<u32>(gen.size()));

        // **翻訳できる命令列を置く。** MOVEQ は窓もメモリも要らない
        // (レジスタだけで閉じる) ので、ホストの 64bit ポインタで窓が
        // 焼けない環境でも計画と発行が通る。満杯の判定へ届かせるには
        // ここまで通す必要がある。
        for (u32 a = kEntry - 0x400; a < kEntry + 0x4000; a += 2)
        {
            ram[a] = 0x70;      // MOVEQ #0,D0
            ram[a + 1] = 0x00;  //
        }

        // **窓を読める状態にする。** Machine::reset の直後は ROM 写像中
        // ($000000 に IPL-ROM が見える) なので fastRamReadable が false で、
        // peekCodeWord が必ず失敗する。その状態だと翻訳は計画の手前で
        // 諦めてしまい、満杯の判定 (allocate) まで届かない。
        cpu().setFastRamReadable(true);

        REQUIRE(code.acquire(codeBytes));
        runner.setStorage(slots.data(), kSlots, &code);
        runner.setNegativeStorage(negEntries.data(), static_cast<std::uint32_t>(negEntries.size()));
        REQUIRE(runner.isReady());
    }

    x68k::M68k& cpu()
    {
        return machine.cpu();
    }

    const x68k::NativeStats& stats() const
    {
        return runner.stats();
    }

    // **窓を読めなくする。** これで peekCodeWord が必ず false を返し、
    // BlockPlanner::plan が諦める = 翻訳が成功しない = runBlock へ届かない。
    // 満杯の判定・スロット索引・鍵外れの帰属はどれも翻訳の手前にあるので、
    // この状態でも問いたい経路は全部通る。
    void makeTranslationImpossible()
    {
        cpu().setFastRamReadable(false);
    }

    // 負の記憶を外す。
    //
    // **多数の番地を訪ねるテストでだけ使う。** 表 (256 件) が溢れると
    // 追い出しが起き、一度覚えた番地でも再び翻訳を試みる。窓を外して
    // あれば翻訳は失敗するので正しさには影響しないが、
    // 「毎回 translateFail で落ちる」ことが保証されると筋が読みやすい。
    void disableNegativeCache()
    {
        runner.setNegativeStorage(nullptr, 0);
    }

    // entryPc から 1 回だけ run() を呼ぶ。
    //
    // **入口の事前条件を本物のインタプリタに作らせる** (pc == 命令語 + 4)。
    // 手で pc を組むと、契約を取り違えたときにテストが先に壊れる。
    x68k::NativeResult runAt(u32 entryPc)
    {
        cpu().refillPrefetchForTest(entryPc);
        return x68k::jit::BlockRunner::runThunk(&runner, cpu());
    }

    // **実装が pc をどのスロットへ写すかを、実装に訊く。**
    //
    // slotIndex() は private なので呼べない。テスト側で式を再現すると
    // **同じ式を 2 度書くだけ**になり、式を変える変異を素通りさせる。
    // 実装の観測可能な副作用から突き止める。
    //
    // 手順: 全スロットへ「この pc とは違う番地の先客」を置いてから 1 回
    // 訪ねる。実装が選んだ 1 つだけがタグ外れとして読まれ、**そのスロットの
    // 先客だけが translate 後に書き換わらずに残る**…のではなく、
    // より単純に: 先客の entryPc をスロットごとに違う値にしておけば、
    // 訪ねた後に keyMissTag が 1 増えることは分かっても「どれか」は分からない。
    //
    // そこで**先客を 1 スロットにだけ置く**方式を使う。候補 i に先客を置いて
    // 訪ね、タグ外れが増えたら当たり (増えなければ実装は別のスロットを見て
    // いて、そこは空きなので keyMissCold が増える)。
    //
    // **翻訳は必ず失敗させる。** 鍵が外れると translate へ落ちるので、
    // 成功するとホストでは runBlock が abort する。窓を読めなくするだけでは
    // 負のキャッシュが溢れた後に再び翻訳を試みてしまうため、
    // **命令語を奇数番地にして peekCodeWord を確実に失敗させる**のではなく、
    // 窓を外したうえで負のキャッシュを無効化 (容量 0) して、
    // 毎回 translateFail で落ちる形にする。
    std::uint32_t runnerSlotIndex(u32 pc)
    {
        REQUIRE_FALSE(cpu().codeWindowForJit().ramReadable);
        static std::uint8_t probeCode[4] = {0, 0, 0, 0};

        std::uint32_t found = kSlots;
        for (std::uint32_t i = 0; i < kSlots; ++i)
        {
            for (std::uint32_t j = 0; j < kSlots; ++j)
            {
                slots[j] = x68k::jit::BlockSlot{};
            }
            slots[i].entryPc = pc ^ 0x2u;  // pc とは必ず違う値
            slots[i].mappingEpoch = cpu().codeGenMap().mappingEpoch();
            slots[i].page = pc >> x68k::CodeGenMap::kPageShift;
            slots[i].pageGen = cpu().codeGenMap().generation(pc);
            slots[i].count = 1;
            slots[i].code = probeCode;

            const std::uint64_t before = stats().keyMissTag;
            runAt(pc);
            if (stats().keyMissTag == before + 1)
            {
                found = i;
                break;
            }
        }
        for (std::uint32_t j = 0; j < kSlots; ++j)
        {
            slots[j] = x68k::jit::BlockSlot{};
        }
        REQUIRE(found < kSlots);
        return found;
    }

    // ページを直接 65,536 回叩いて飽和させる。
    //
    // **テスト対象 (BlockRunner) を経由しない。** run() を回して飽和させようと
    // すると回数が現実的でなくなるうえ、作りたい状態と測りたい振る舞いが
    // 混ざる。世代配列は外から与えている実体なので、直接叩けば済む。
    void saturatePage(u32 addr)
    {
        x68k::CodeGenMap& map = cpu().codeGenMap();
        while (map.generation(addr) != x68k::CodeGenMap::kAlwaysStale)
        {
            map.touch(addr);
        }
        REQUIRE(map.generation(addr) == x68k::CodeGenMap::kAlwaysStale);
    }
};

// 索引の再現。**実装と同じ式をここに書く**ので、これ自体は実装の正しさの
// 根拠にならない。使うのは「同じスロットに写る別の番地」を作るためだけで、
// 分散そのものは T4 が実装の観測可能な振る舞い (keyMissTag) で問う。
std::uint32_t foldedIndex(u32 pc, std::uint32_t slotCount)
{
    return ((pc >> 1) ^ (pc >> 10)) & (slotCount - 1);
}

}  // namespace runner_accounting

// --- T1: 勘定の保存 ---------------------------------------------------------

TEST_CASE("諦めた回数は理由別カウンタの合計と常に一致する")
{
    // **保存則: deferUnsupported == negativeHit + translateFail + fullDeferred**
    //
    // これが破れることは「どのカウンタにも乗らずに諦めた経路がある」ことと
    // 同値である。欠陥 1 (満杯で諦めた 1481 万回がどこにも乗らず「未対応命令」
    // に見えていた) は、まさにこの破れだった。
    //
    // **単一の状態ではなく、諦め方の異なる 3 つの状態を全部通す。** 1 つの
    // 経路だけで確かめると、他の経路に無勘定の return が増えても気づけない。
    using namespace runner_accounting;

    SUBCASE("翻訳失敗で諦め続けても収支が閉じる")
    {
        Harness h;
        h.makeTranslationImpossible();
        for (int i = 0; i < 64; ++i)
        {
            const u32 pc = 0x2000u + static_cast<u32>(i) * 4u;
            CHECK(h.runAt(pc).exit == x68k::NativeExit::kDeferToStep);
        }
        // 諦めていること自体を先に確かめる (0 == 0 で通る空虚な保存を避ける)。
        CHECK(h.stats().deferUnsupported > 0);
        checkDeferAccounting(h.stats(), "翻訳失敗のみ");
    }

    SUBCASE("負の記憶が効いた後も収支が閉じる")
    {
        // 同じ番地を叩き直すと、2 回目以降は負の記憶で translate を省く。
        // **省いた分も内訳に乗らなければ収支が破れる。**
        Harness h;
        h.makeTranslationImpossible();
        for (int i = 0; i < 32; ++i)
        {
            CHECK(h.runAt(0x2000u).exit == x68k::NativeExit::kDeferToStep);
        }
        CHECK(h.stats().negativeHit > 0);
        checkDeferAccounting(h.stats(), "負の記憶あり");
    }

    SUBCASE("満杯で諦めた分も内訳に乗る")
    {
        // **欠陥 1 の直接の回帰テスト。** 満杯の早期 return が数えられて
        // いなければ、ここで deferUnsupported だけが増えて収支が破れる。
        //
        // 満杯にする手は「ExecMemory を外から使い切る」。code_ は外が
        // 与えている実体なので、テストが直接 allocate して空にできる。
        // こうすると translate() は計画も発行も通ったうえで
        // code_->allocate() に断られ、そこで codeFull_ を立てる。
        // **成功した翻訳は 1 本も無いので runBlock へは行かない。**
        Harness h;
        REQUIRE(h.code.allocate(h.code.capacity()) != nullptr);
        REQUIRE(h.code.allocate(4) == nullptr);

        for (int i = 0; i < 64; ++i)
        {
            const u32 pc = kEntry + static_cast<u32>(i) * 2u;
            CHECK(h.runAt(pc).exit == x68k::NativeExit::kDeferToStep);
        }
        // 満杯の経路を実際に通ったこと。通っていなければ以下の保存則は
        // 満杯について何も言っていない。
        CHECK(h.stats().fullDeferred > 0);
        checkDeferAccounting(h.stats(), "コード領域が満杯");
    }
}

// --- T2: 満杯からの回復 (生存性) --------------------------------------------

TEST_CASE("コード領域が満杯になっても有限回で翻訳器が回復する")
{
    // **これは「正しさ」ではなく「生存性」のテストである。**
    //
    // 満杯のまま永久に諦め続けても、諦めた命令はインタプリタが実行するので
    // **状態は 1 ビットも狂わない**。だから状態を比べるテストをいくら足しても
    // この欠陥は永遠に見えない (MEMORY.md「保守的なフォールバックはテストの
    // 盲点になる」)。実機ではこれが 45 秒間ずっと続いていた。
    //
    // 問うべきは「悪い状態に入った後、有限回以内に出られるか」である。
    using namespace runner_accounting;

    Harness h;
    // 外から使い切って満杯にする。以後 translate() は入口で諦める。
    REQUIRE(h.code.allocate(h.code.capacity()) != nullptr);

    // 翻訳可能な pc を叩き続ける。**同じ pc でよい** — 実機で起きていたのも
    // 「常駐ルーチンを回し続けているのに翻訳が再開しない」という形だった。
    //
    // 上限は閾値の定数倍。回復経路が無ければここで尽きる。
    // **実装の定数を読む。** 写すと、値を変えたときにテストの前提だけが
    // 古くなって「回復しない」と嘘の失敗をする (実際に踏んだ)。
    constexpr std::uint32_t kThreshold = x68k::jit::BlockRunner::kCapacityResetThreshold;
    constexpr std::uint32_t kBudget = kThreshold * 4;

    bool recovered = false;
    std::uint32_t calls = 0;
    for (; calls < kBudget; ++calls)
    {
        h.runAt(kEntry);
        if (h.stats().capacityReset > 0)
        {
            recovered = true;
            break;
        }
    }

    INFO("calls=", calls, " fullDeferred=", h.stats().fullDeferred,
         " capacityReset=", h.stats().capacityReset);
    // **有限回以内に回復したこと。** 落ちるなら回復経路が無い。
    CHECK(recovered);
    // 閾値の定数倍以内であること (回復はするが遅すぎる、を許さない)。
    CHECK(calls <= kBudget);

    // 回復したなら、実行可能メモリは巻き戻っていて再び切り出せる。
    // **「reset を呼んだ」ではなく「また翻訳できる状態になった」を問う。**
    CHECK(h.code.used() < h.code.capacity());
    CHECK(h.code.allocate(4) != nullptr);

    checkDeferAccounting(h.stats(), "満杯からの回復");
}

// --- T3: 反スラッシング (逆向きの上限) --------------------------------------

TEST_CASE("捨て直しは閾値ぶん諦めるまで起きない")
{
    // **T2 の逆向きの上限。** 「回復する」だけを問うと、満杯を見るたびに
    // reset する実装 (閾値 1) でも通ってしまう。それはキャッシュを永久に
    // 冷たいままにするスラッシングで、満杯で凍るのと同じくらい遅い。
    // 生存性のテストには必ず「やりすぎない」側の上限を対にして置く。
    using namespace runner_accounting;

    constexpr std::uint32_t kThreshold = x68k::jit::BlockRunner::kCapacityResetThreshold;

    SUBCASE("満杯でも飽和でもない負荷では 1 回も捨てない")
    {
        Harness h;  // 既定の 64KB。埋まらない。
        // **翻訳を成功させない。** 成功するとホストでは runBlock (Xtensa の
        // 機械語) を呼んで abort する。ここで問うのは翻訳の成否より手前の
        // 判断なので、失敗させたままでも問える。
        h.makeTranslationImpossible();

        for (int i = 0; i < 256; ++i)
        {
            h.runAt(kEntry + static_cast<u32>(i % 8) * 2u);
        }

        INFO("fullDeferred=", h.stats().fullDeferred, " capacityReset=", h.stats().capacityReset);
        CHECK(h.stats().fullDeferred == 0);  // 前提の確認
        CHECK(h.stats().capacityReset == 0);
        CHECK(h.stats().generationReset == 0);

        checkDeferAccounting(h.stats(), "スラッシングなし");
    }

    SUBCASE("満杯でも閾値に届くまでは捨てない")
    {
        // **これが M (閾値を小さくする) を殺す本体。**
        //
        // 満杯にしたうえで、閾値の手前まで叩く。捨てる費用は常駐ブロックの
        // 再翻訳まるごとなので、要求が積もる前に捨ててはいけない。
        // **窓は読めるままにする。** 読めなくすると計画の段階で諦めて
        // しまい、満杯の判定 (allocate) まで届かない = codeFull_ が立たない。
        // 満杯にしておけば翻訳は必ず入口で諦めるので、成功して runBlock へ
        // 行く心配はない。
        Harness h;
        REQUIRE(h.code.allocate(h.code.capacity()) != nullptr);

        // 1 回目で allocate に断られて codeFull_ が立つ。この 1 回は
        // translateFail に数えられ、fullDeferred には乗らない。
        h.runAt(kEntry);
        REQUIRE(h.stats().translateFail == 1u);
        REQUIRE(h.stats().fullDeferred == 0u);

        // **1 回ごとに「まだ捨てていない」ことを確かめながら進む。**
        //
        // Why not 閾値の手前までまとめて回してから 1 回だけ見るか:
        // 閾値を小さくする変異では、途中で捨てた瞬間にコード領域が空くので
        // **次の翻訳が成功して runBlock (ホストでは abort) へ落ちる**。
        // それだと落ち方が SIGABRT になり、「何を破ったのか」が読めない。
        // 毎回見れば、捨てた最初の 1 回をアサーションとして捕まえられる。
        for (std::uint32_t i = 0; i < kThreshold - 1u; ++i)
        {
            h.runAt(kEntry);
            if (h.stats().capacityReset != 0u)
            {
                INFO("捨て直しが早すぎる: ", i + 2u, " 回目 (閾値 ", kThreshold, ")");
                CHECK(h.stats().capacityReset == 0u);
                return;
            }
        }

        INFO("fullDeferred=", h.stats().fullDeferred, " capacityReset=", h.stats().capacityReset);
        // 満杯の経路を確かに通っていること (通っていなければ以下は空虚)。
        CHECK(h.stats().fullDeferred == kThreshold - 1u);
        // **閾値に 1 回足りないので、まだ捨てていないこと。**
        CHECK(h.stats().capacityReset == 0);
        // 実行可能メモリも巻き戻っていないこと。
        CHECK(h.code.allocate(4) == nullptr);

        // もう 1 回叩けば閾値に届き、そこで初めて捨てる。
        h.runAt(kEntry);
        CHECK(h.stats().capacityReset == 1);

        checkDeferAccounting(h.stats(), "閾値の境界");
    }
}

// --- T4: 索引の分散 ---------------------------------------------------------

TEST_CASE("1KB 周期に並ぶ番地がスロット全体へ散る")
{
    // **欠陥 2 の回帰テスト。** 索引が (pc >> 1) & mask だったとき、
    // 2 * slotCount バイト周期 (512 スロットなら 1KB) の番地は**全部同じ
    // スロット**に落ちた。2MB に散った常駐部とサブルーチン群に対して
    // 系統的に当たる形で、実機では鍵外れの 21.5% がこれだった。
    //
    // **正しさは壊れない** (タグ照合が必ず外すので別番地のコードは走らない)。
    // 壊れるのは速度だけなので、状態を比べるテストには映らない。
    //
    // ## ここで何を「保証」と呼ぶか
    //
    // 「N 個の pc が N 個の異なるスロットに写る」は**成り立たない**。
    // 512 スロット (マスク 0x1FF = 索引ビット 0-8) では、pc のビット 10 が
    // (pc >> 1) のビット 9 へ行ってマスクの外へ落ち、(pc >> 11) にも乗らない。
    // **ビット 10 は索引から完全に消える**ので、1KB だけ離れた 2 番地は
    // 必ず衝突する (下の SUBCASE がその事実を固定している)。
    //
    // したがって保証するのは「衝突が畳み込みの周期で系統的に起きないこと」
    // — 具体的には **2KB 周期では全部散り、1KB 周期でも半数まで散る**こと。
    // 直す前は 16 個が 1 個へ潰れていたので、これでも桁違いに違う。
    using namespace runner_accounting;

    constexpr std::uint32_t kSlots = Harness::kSlots;
    constexpr int kCount = 16;

    SUBCASE("2KB 周期なら全部が別のスロットへ行く")
    {
        // ビット 10 を跨がない周期。ここは完全に散らなければならない。
        constexpr u32 kStride = 4u * kSlots;  // 2048
        std::vector<std::uint32_t> seen;
        for (int i = 0; i < kCount; ++i)
        {
            seen.push_back(foldedIndex(kEntry + static_cast<u32>(i) * kStride, kSlots));
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        INFO("distinct = ", seen.size(), " / ", kCount);
        CHECK(seen.size() == static_cast<std::size_t>(kCount));
    }

    SUBCASE("1KB 周期でも半数以上へ散る")
    {
        // **直す前はここが 1 だった。** 畳み込みを外すと 16 個が 1 個へ潰れる。
        constexpr u32 kStride = 2u * kSlots;  // 1024
        std::vector<std::uint32_t> seen;
        for (int i = 0; i < kCount; ++i)
        {
            seen.push_back(foldedIndex(kEntry + static_cast<u32>(i) * kStride, kSlots));
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        INFO("distinct = ", seen.size(), " / ", kCount);
        // **全数散ること。** 半数で妥協すると、ビット 10 が索引から
        // 落ちている状態 (16 → 8 スロット) を緑のまま通してしまう。
        CHECK(seen.size() == static_cast<std::size_t>(kCount));
    }

    SUBCASE("ちょうど 1KB 離れた番地が別のスロットへ写る")
    {
        // **直そうとした当の周期が残っていないこと。**
        //
        // 畳み込みが (pc >> 11) だと、512 スロット (マスク 0x1FF) では
        // pc のビット 10 が索引から完全に消え、1KB 離れた 2 番地が必ず
        // 衝突したままになる。「散らした」つもりで元の周期が残る形なので、
        // ビット 10 だけが違う組を名指しで問う。
        for (u32 pc = kEntry; pc < kEntry + 0x8000u; pc += 2u)
        {
            REQUIRE(foldedIndex(pc, kSlots) != foldedIndex(pc ^ 0x400u, kSlots));
        }
    }

    SUBCASE("実装の振る舞いでも 2KB 周期の番地が共存できる")
    {
        // 索引式の再現 (上の 3 つ) は**同じ式を 2 度書いただけ**なので、
        // 実装を問うたことにならない。畳み込みを外す変異はテスト側の
        // foldedIndex を変えないため、上の 3 つは素通りする。
        // **実装の観測可能な振る舞いで裏を取る。**
        //
        // 手順: 2KB 周期の番地を 1 つずつスロットへ「居座らせ」てから、
        // もう一度全部を訪ねる。散っていれば全員が自分のスロットに残って
        // いるのでタグ外れは 1 件も出ない。畳み込みを外すと全員が同じ
        // スロットへ落ち、最後の 1 つ以外は追い出されてタグ外れになる。
        Harness h;
        // 翻訳は成功させない (ホストでは runBlock が abort する)。スロット索引と
        // 鍵外れの帰属はどちらも翻訳の手前なので、失敗させたままで問える。
        h.makeTranslationImpossible();
        // 索引の探索は同じ pc を何百回も訪ねるので、表が溢れて追い出しが
        // 起きないよう外しておく。
        h.disableNegativeCache();
        constexpr u32 kStride = 4u * kSlots;

        // **実装に「この pc はどのスロットか」を訊いて回る。**
        //
        // Why not スロットへ先客を置いて訪ね直す形にしないか: 鍵が合うと
        // それは「当たり」なので run() が生成コードを呼ぶ。ホストでは
        // runBlock が abort するため、**当たりを作ってはいけない**。
        // 索引だけを訊き出して、衝突の有無はテスト側で数える。
        std::vector<std::uint32_t> where;
        for (int i = 0; i < kCount; ++i)
        {
            where.push_back(h.runnerSlotIndex(kEntry + static_cast<u32>(i) * kStride));
        }

        std::vector<std::uint32_t> uniq = where;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());

        INFO("distinct slots = ", uniq.size(), " / ", kCount);
        // **N 個の pc が N 個の異なるスロットへ写ること。**
        // 畳み込みを外すと 2KB 周期は全部スロット 0 へ落ち、ここが 1 になる。
        CHECK(uniq.size() == static_cast<std::size_t>(kCount));

        checkDeferAccounting(h.stats(), "索引の分散");
    }
}

// --- T5: 帰属の排他性 -------------------------------------------------------

TEST_CASE("スロット衝突は居座りブロックのページが飽和していてもタグに数える")
{
    // **欠陥 3 の回帰テスト。**
    //
    // 鍵照合は世代を `slot->page` (= そのスロットの**先客**のページ) から
    // 引く。空きスロットなら 0 で、ページ 0 は例外ベクタ表と Human68k の
    // ワーク領域なので起動後すぐ 65,536 回書かれて飽和する。
    //
    // 飽和すると `nowGen == kAlwaysStale` が**どのスロットのどの取りこぼしでも**
    // 成立する。帰属の if-else が kAlwaysStale をタグより先に見ていたため、
    // 本来「スロット衝突」であるものが**まとめて「飽和」に化けた**。
    // 実機ではこれで衝突 151 万件が誤帰属され、対策の方向を丸ごと見誤った。
    //
    // **判定 (hit) の順序は変えていない。** kAlwaysStale を世代一致より先に
    // 見るのは正しさの根拠がある (§5.5)。変えたのは帰属の順序だけで、
    // このテストは「正しさの順序」と「説明の順序」が別物であることを固定する。
    using namespace runner_accounting;

    Harness h;
    h.makeTranslationImpossible();

    // 同じスロットへ写る 2 つの番地を用意する。
    //
    // Why not 「ビット 10 だけ違う番地」にしないか: それが必ず衝突するのは
    // **索引の欠陥そのもの**で、直した今は衝突しない。衝突する組は
    // 索引を実際に引いて探す。こうしておけば索引式が変わっても
    // このテストの前提は壊れない (衝突は必ずどこかに存在する — 番地の
    // 数がスロット数より多いので鳩の巣原理)。
    constexpr std::uint32_t kSlots = Harness::kSlots;
    const u32 pc1 = kEntry;
    u32 pc2 = 0;
    for (u32 cand = kEntry + 2u; cand < kEntry + 0x40000u; cand += 2u)
    {
        if (foldedIndex(cand, kSlots) == foldedIndex(pc1, kSlots))
        {
            pc2 = cand;
            break;
        }
    }
    REQUIRE(pc2 != 0u);
    REQUIRE(foldedIndex(pc1, kSlots) == foldedIndex(pc2, kSlots));
    REQUIRE(pc1 != pc2);

    // pc1 をスロットへ居座らせる。**翻訳は成功しないので、居座らせるには
    // スロットを直接書く** (テスト対象を経由せず状態を作る)。
    x68k::jit::BlockSlot& slot = h.slots[foldedIndex(pc1, kSlots)];
    slot.entryPc = pc1;
    slot.mappingEpoch = h.cpu().codeGenMap().mappingEpoch();
    slot.page = pc1 >> x68k::CodeGenMap::kPageShift;
    slot.pageGen = h.cpu().codeGenMap().generation(pc1);
    slot.count = 1;
    // code != nullptr でないと帰属の if-else へ入らない (空きは keyMissCold)。
    // **実行はされない** (タグが外れるので translate へ落ちる)。
    static const std::uint8_t kDummyCode[4] = {0, 0, 0, 0};
    slot.code = kDummyCode;

    // **先客のページを飽和させる。** 65,536 回の touch を直接叩く
    // (テスト対象を経由せず状態を作る)。
    h.saturatePage(pc1);
    REQUIRE(h.cpu().codeGenMap().generation(pc1) == x68k::CodeGenMap::kAlwaysStale);

    const std::uint64_t tagBefore = h.stats().keyMissTag;
    const std::uint64_t staleBefore = h.stats().keyMissStale;

    // pc2 を訪ねる。先客は pc1 なのでタグが外れる。同時に先客のページは
    // 飽和しているので kAlwaysStale も成立する。**どちらに数えるか。**
    h.runAt(pc2);

    INFO("keyMissTag ", tagBefore, " -> ", h.stats().keyMissTag);
    INFO("keyMissStale ", staleBefore, " -> ", h.stats().keyMissStale);
    // **タグに数えること。** これが実体 (別の番地がスロットを取り合っている)。
    CHECK(h.stats().keyMissTag == tagBefore + 1);
    // **飽和は 1 も動かないこと。** 動くなら衝突が飽和に化けている。
    CHECK(h.stats().keyMissStale == staleBefore);

    checkDeferAccounting(h.stats(), "帰属の排他性");
}

// --- T6: コールドミスが数えられている ---------------------------------------

TEST_CASE("空きスロットを引いたらコールドミスとして数える")
{
    // **欠陥 3 の片割れ。** 鍵外れの帰属は `slot->code != nullptr` の中でしか
    // 数えていなかったので、**空きスロットの取りこぼしはどのカウンタにも
    // 乗らなかった**。起動直後は全部が空きなので、これは取りこぼしの
    // 最大成分でありながら統計から完全に消えていた。
    //
    // 「見えないものは直せない」形の欠陥で、正しさは 1 ビットも壊れない。
    using namespace runner_accounting;

    Harness h;
    h.makeTranslationImpossible();

    // 全部が空きスロットの状態から、別々のスロットへ写る番地を訪ねる。
    constexpr std::uint32_t kSlots = Harness::kSlots;
    constexpr int kCount = 8;
    constexpr u32 kStride = 4u * kSlots;  // 2KB: 完全に散る周期

    for (int i = 0; i < kCount; ++i)
    {
        h.runAt(kEntry + static_cast<u32>(i) * kStride);
    }

    INFO("keyMissCold=", h.stats().keyMissCold, " keyMissTag=", h.stats().keyMissTag,
         " keyMissStale=", h.stats().keyMissStale, " keyMissGen=", h.stats().keyMissGen,
         " keyMissEpoch=", h.stats().keyMissEpoch);
    // **1 回ずつ数えられていること。**
    CHECK(h.stats().keyMissCold == static_cast<std::uint64_t>(kCount));
    // 空きスロットは他のどの理由にも数えられないこと (排他)。
    CHECK(h.stats().keyMissTag == 0);
    CHECK(h.stats().keyMissStale == 0);
    CHECK(h.stats().keyMissGen == 0);
    CHECK(h.stats().keyMissEpoch == 0);

    checkDeferAccounting(h.stats(), "コールドミス");
}
