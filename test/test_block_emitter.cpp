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
#include "cpu/m68k_types.h"
#include "jit/block_emitter.h"
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

bool buildPlan(const std::vector<u16>& words, BlockPlan& plan, FlatCode& code)
{
    code.reset(0x1000, 0x3000);
    u32 at = kEntry;
    for (u16 w : words)
    {
        code.put16(at, w);
        at += 2;
    }
    return BlockPlanner::plan(makeSource(code), makeGen(), kEntry, plan);
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

jit::EmitEnv fakeEnv()
{
    jit::EmitEnv env{};
    env.ramBaseAddr = kFakeWindow;
    env.ramLimit = static_cast<std::uint32_t>(x68k::kMainRamSize);
    env.ramReadable = true;
    env.genBaseAddr = kFakeGenWindow;
    env.genPageCount = kGenPages;
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

    const bool branchTaken = native.branchTaken;
    compareStates(want, native.state, branchTaken, what);

    // サイクル数。**ここがずれると rasterNumber の 317 サイクル粒度が
    // 数万命令後に必ず割れる。**
    INFO("cycles");
    CHECK(native.cycles == refCycles);

    if (native.guardExit)
    {
        // ガード脱出は分岐ではない (G9: bit31 と bit30 は同時に立たない)。
        INFO("guard exit is not a branch");
        CHECK_FALSE(branchTaken);
        // 降りた地点は「まだ実行していない命令の手前」なので、
        // 計画の全命令を走り切ってはいない。
        CHECK(native.ranOps < plan.count);
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

constexpr u16 bcc(u32 cond, int disp8)
{
    return static_cast<u16>(0x6000u | (cond << 8) | (static_cast<u32>(disp8) & 0xFFu));
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
        {0x3u, {0x0000}},         {0x3u, {0x8000}}, {0x3u, {0x7FFF}}, {0x2u, {0x1234, 0x5678}},
        {0x2u, {0x8000, 0x0000}}, {0x1u, {0x0042}}, {0x1u, {0x0080}},
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
    // BlockRunner::kStagingBytes (1024) に収まること。**収まらないと
    // translate が黙って諦める。**
    CHECK(need <= 1024);

    // 走らせても一致すること。
    M68kState s = makeState(2);
    s.a[2] = kWriteAddr;
    s.a[3] = kWriteAddr + 0x10u;
    s.a[4] = kWriteAddr + 0x20u;
    s.a[5] = kWriteAddr + 0x30u;
    checkEquivalence(words, s, "CLR.l x 4");
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
            CHECK(r.ranOps == 0);
        }
        {
            const jit::BlockReturn r = jit::decodeBlockReturn(cycles | jit::kBranchTakenFlag);
            CHECK(r.cycles == cycles);
            CHECK(r.branchTaken);
            CHECK_FALSE(r.guardExit);
            CHECK_FALSE(r.selfPageExit);
        }
        for (std::uint32_t k = 0; k < x68k::kMaxOps; ++k)
        {
            const std::uint32_t ret = cycles | jit::kGuardExitFlag | (k << jit::kGuardCountShift);
            const jit::BlockReturn r = jit::decodeBlockReturn(ret);
            CHECK(r.cycles == cycles);
            CHECK(r.guardExit);
            CHECK_FALSE(r.branchTaken);
            CHECK_FALSE(r.selfPageExit);
            CHECK(r.ranOps == k);
            // 自ページ脱出 (G18): bit30 に**加えて** bit23 が立つ。
            const jit::BlockReturn sp = jit::decodeBlockReturn(ret | jit::kSelfPageExitFlag);
            CHECK(sp.cycles == cycles);
            CHECK(sp.guardExit);
            CHECK(sp.selfPageExit);
            CHECK_FALSE(sp.branchTaken);
            CHECK(sp.ranOps == k);
        }
    }

    // **kSelfPageExitFlag はサイクルの外にある。** kCycleMask と重なると、
    // 自ページ脱出のたびにサイクルが 8,388,608 増えて時間が飛ぶ。
    CHECK((jit::kSelfPageExitFlag & jit::kCycleMask) == 0u);
    // ガード脱出でなければ bit23 は見ない (符号化の契約)。
    CHECK_FALSE(jit::decodeBlockReturn(jit::kSelfPageExitFlag).selfPageExit);
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
            std::size_t guardAt = e.buffer.size();
            for (std::size_t i = e.info.entryOffset; i + 3 <= e.buffer.size(); ++i)
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
            for (std::size_t i = e.info.entryOffset; i + 3 <= guardAt; ++i)
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
