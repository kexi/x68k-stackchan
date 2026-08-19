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

    XtensaRun run(std::size_t entry, std::uint32_t arg)
    {
        for (int i = 0; i < 16; ++i)
        {
            a_[i] = 0xDEADBEEFu;
        }
        a_[2] = arg;
        pc_ = entry;

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

    // 生成コードは a2 (= M68kState の先頭) からの相対でしか触らない契約。
    //
    // **範囲を外れたらその場で失敗にする。** ここを素通しにすると、
    // プロローグを落とすような変異が「ホストのメモリを踏んで落ちる」形で
    // 現れ、テストが緑にも赤にもならず固まる。
    [[nodiscard]] bool inRange(std::uint32_t addr, std::size_t len) const
    {
        return static_cast<std::size_t>(addr) + len <= memSize_;
    }

    [[nodiscard]] std::uint32_t load32(std::uint32_t addr)
    {
        if (!inRange(addr, 4))
        {
            outOfRange_ = true;
            return 0;
        }
        std::uint32_t v = 0;
        std::memcpy(&v, mem_ + addr, 4);
        return v;
    }
    void store32(std::uint32_t addr, std::uint32_t v)
    {
        if (!inRange(addr, 4))
        {
            outOfRange_ = true;
            return;
        }
        std::memcpy(mem_ + addr, &v, 4);
    }
    [[nodiscard]] std::uint32_t load16(std::uint32_t addr)
    {
        if (!inRange(addr, 2))
        {
            outOfRange_ = true;
            return 0;
        }
        std::uint16_t v = 0;
        std::memcpy(&v, mem_ + addr, 2);
        return v;
    }
    void store16(std::uint32_t addr, std::uint32_t v)
    {
        if (!inRange(addr, 2))
        {
            outOfRange_ = true;
            return;
        }
        const std::uint16_t h = static_cast<std::uint16_t>(v & 0xFFFFu);
        std::memcpy(mem_ + addr, &h, 2);
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
                a_[t] = v;
                pc_ += 3;
                return nullptr;
            }
            case 0x2u:  // RRI8 群
                switch (r)
                {
                    case 0x1u:  // l16ui at, as, imm8*2
                        a_[t] = load16(a_[s] + imm8 * 2u);
                        pc_ += 3;
                        return nullptr;
                    case 0x2u:  // l32i at, as, imm8*4
                        a_[t] = load32(a_[s] + imm8 * 4u);
                        pc_ += 3;
                        return nullptr;
                    case 0x5u:  // s16i
                        store16(a_[s] + imm8 * 2u, a_[t]);
                        pc_ += 3;
                        return nullptr;
                    case 0x6u:  // s32i
                        store32(a_[s] + imm8 * 4u, a_[t]);
                        pc_ += 3;
                        return nullptr;
                    case 0xAu:  // movi at, imm12 (s は imm[11:8])
                    {
                        const std::uint32_t raw = (s << 8) | imm8;
                        // 12bit 符号付き。
                        const std::int32_t v = (raw & 0x800u) != 0
                                                   ? static_cast<std::int32_t>(raw | 0xFFFFF000u)
                                                   : static_cast<std::int32_t>(raw);
                        a_[t] = static_cast<std::uint32_t>(v);
                        pc_ += 3;
                        return nullptr;
                    }
                    case 0xCu:  // addi at, as, imm8 (符号付き)
                        a_[t] =
                            a_[s] + static_cast<std::uint32_t>(static_cast<std::int32_t>(
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
                    a_[r] = (a_[t] >> sh) & m;
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
                    a_[r] = a_[s] | a_[t];
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x3u && op1 == 0x0u)
                {
                    a_[r] = a_[s] ^ a_[t];
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0xCu && op1 == 0x0u)
                {
                    a_[r] = a_[s] - a_[t];
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x6u && op1 == 0x0u && s == 0u)
                {
                    a_[r] = 0u - a_[t];
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x8u && op1 == 0x3u)
                {
                    if (a_[t] == 0u)
                    {
                        a_[r] = a_[s];
                    }
                    pc_ += 3;
                    return nullptr;
                }
                if (op2 == 0x9u && op1 == 0x3u)
                {
                    if (a_[t] != 0u)
                    {
                        a_[r] = a_[s];
                    }
                    pc_ += 3;
                    return nullptr;
                }
                if (op1 == 0x1u && (op2 == 0x0u || op2 == 0x1u))
                {
                    // slli ar, as, n。符号化されているのは 32 - n。
                    const std::uint32_t sa = t | (op2 << 4);
                    const std::uint32_t n = 32u - sa;
                    a_[r] = n >= 32u ? 0u : (a_[s] << n);
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
                    const bool take = sub == 0x1u ? (a_[s] == 0u) : (a_[s] != 0u);
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
                a_[r] = a_[s] + a_[t];
                pc_ += 2;
                return nullptr;
            case 0x8u:  // l32i.n at, as, r*4
                a_[t] = load32(a_[s] + r * 4u);
                pc_ += 2;
                return nullptr;
            case 0x9u:  // s32i.n
                store32(a_[s] + r * 4u, a_[t]);
                pc_ += 2;
                return nullptr;
            case 0xCu:  // movi.n as, imm7
            {
                const std::uint32_t raw = (t << 4) | r;
                const std::int32_t v = (raw & 0x60u) == 0x60u
                                           ? static_cast<std::int32_t>(raw | 0xFFFFFF80u)
                                           : static_cast<std::int32_t>(raw);
                a_[s] = static_cast<std::uint32_t>(v);
                pc_ += 2;
                return nullptr;
            }
            case 0xDu:  // mov.n at, as (r == 0)
                if (r != 0u)
                {
                    return "未知の RRRN 命令 (0xD)";
                }
                a_[t] = a_[s];
                pc_ += 2;
                return nullptr;
            default:
                return "未知の命令";
        }
    }

    void and_op(std::uint32_t r, std::uint32_t s, std::uint32_t t)
    {
        a_[r] = a_[s] & a_[t];
    }

    const std::uint8_t* code_;
    std::size_t size_;
    std::uint8_t* mem_;
    std::size_t memSize_ = 0;
    bool outOfRange_ = false;
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

EmitResult emit(const BlockPlan& plan, const FlatCode& code)
{
    EmitResult r{};
    const u16 ir = code.get16(plan.fallThroughPc);
    const u16 irc = code.get16(plan.fallThroughPc + 2);
    const std::size_t need = jit::requiredSize(plan, ir, irc);
    if (need == 0)
    {
        return r;
    }
    r.buffer.assign(need, 0xCC);
    r.ok = jit::emitBlock(plan, ir, irc, r.buffer.data(), r.buffer.size(), r.info);
    return r;
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
};

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
    const XtensaRun r = cpu.run(e.info.entryOffset, 0);
    if (!r.ok)
    {
        out.failure = r.failure;
        return out;
    }
    out.ok = true;
    out.state = mem.store();
    out.branchTaken = (r.ret & jit::kBranchTakenFlag) != 0;
    out.cycles = r.ret & jit::kCycleMask;
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

void ramPoke16(u32 a, u16 v)
{
    execRam()[a] = static_cast<u8>(v >> 8);
    execRam()[a + 1] = static_cast<u8>(v & 0xFFu);
}

// words を kEntry へ置き、initial から count 命令だけ回した結果を返す。
M68kState runReference(const std::vector<u16>& words, const M68kState& initial, u32 count,
                       u32& cyclesOut)
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

// 命令列と初期状態を 1 組ぶん検査する。
void checkEquivalence(const std::vector<u16>& words, const M68kState& initial, const char* what)
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

    const EmitResult e = emit(plan, code);
    REQUIRE(e.ok);

    const NativeOutcome native = runEmitted(e, initial);
    INFO(std::string(native.failure == nullptr ? "ok" : native.failure));
    REQUIRE(native.ok);

    u32 refCycles = 0;
    const M68kState want = runReference(words, initial, plan.count, refCycles);

    const bool branchTaken = native.branchTaken;
    compareStates(want, native.state, branchTaken, what);

    // サイクル数。**ここがずれると rasterNumber の 317 サイクル粒度が
    // 数万命令後に必ず割れる。**
    INFO("cycles");
    CHECK(native.cycles == refCycles);

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
                            code.get16(plan.fallThroughPc + 2)) == a.info.totalSize);
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
    const std::size_t need = jit::requiredSize(plan, ir, irc);
    REQUIRE(need > 0);

    std::vector<std::uint8_t> buf(need, 0xCC);
    jit::EmittedBlock info{};
    // 1 バイト足りないと必ず断ること。
    CHECK_FALSE(jit::emitBlock(plan, ir, irc, buf.data(), need - 1, info));
    CHECK(jit::emitBlock(plan, ir, irc, buf.data(), need, info));
}

TEST_CASE("空の計画は発行しない")
{
    BlockPlan plan{};
    plan.count = 0;
    CHECK(jit::requiredSize(plan, 0, 0) == 0);
    std::uint8_t buf[64];
    jit::EmittedBlock info{};
    CHECK_FALSE(jit::emitBlock(plan, 0, 0, buf, sizeof(buf), info));

    // count が壊れている計画も断る (§4(d) のゴミ検査と同じ趣旨)。
    plan.count = x68k::kMaxOps + 1;
    CHECK(jit::requiredSize(plan, 0, 0) == 0);
    CHECK_FALSE(jit::emitBlock(plan, 0, 0, buf, sizeof(buf), info));
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
                                code.get16(plan.fallThroughPc + 2)) > 0);

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
    const std::size_t need = jit::requiredSize(plan, ir, irc);
    REQUIRE(need > 0);

    std::uint8_t* slot = mem.allocate(need);
    REQUIRE(slot != nullptr);
    jit::EmittedBlock info{};
    REQUIRE(jit::emitBlock(plan, ir, irc, slot, need, info));
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
