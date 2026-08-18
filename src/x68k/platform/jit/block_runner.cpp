// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "jit/block_runner.h"

#include "cpu/block_planner.h"
#include "cpu/code_gen_map.h"
#include "jit/block_emitter.h"

namespace x68k::jit
{
namespace
{

// PlanSource / PlanGenSource が M68k を触るための箱。
struct PlanCtx
{
    M68k* cpu;
};

bool readCodeWord(void* ctx, u32 addr, u16& out)
{
    // **窓の中からだけ読む。** バスを叩くと翻訳しただけで MFP の
    // 割り込み要因レジスタが動く。
    return static_cast<PlanCtx*>(ctx)->cpu->peekCodeWord(addr, out);
}

std::uint16_t pageGeneration(void* ctx, u32 addr)
{
    return static_cast<PlanCtx*>(ctx)->cpu->codeGenMap().generation(addr);
}

u32 mappingEpochOf(void* ctx)
{
    return static_cast<PlanCtx*>(ctx)->cpu->codeGenMap().mappingEpoch();
}

}  // namespace

void BlockRunner::rememberFailure(std::uint32_t entryPc, std::uint16_t gen)
{
    // **飽和したページは覚えない。** kAlwaysStale は「常に古い」なので、
    // 覚えても次回必ず外れる。覚えた分だけ表を無駄にする。
    if (gen == CodeGenMap::kAlwaysStale)
    {
        return;
    }
    neg_.insert(entryPc, gen);
}

void BlockRunner::reset()
{
    for (std::uint32_t i = 0; i < slotCount_; ++i)
    {
        slots_[i] = BlockSlot{};
    }
    if (code_ != nullptr)
    {
        code_->reset();
    }
    neg_.clear();
    codeFull_ = false;
}

BlockSlot* BlockRunner::translate(M68k& cpu, std::uint32_t entryPc)
{
    if (codeFull_)
    {
        return nullptr;
    }

    // **一度失敗した番地なら、もう試さない。**
    //
    // 世代が当時のままなら同じ命令列が同じ結果になる。動いていれば
    // 覚え直す (ゲストが書き換えた可能性がある)。写像の変化は run() が
    // 一括で捨てている。
    CodeGenMap& map = cpu.codeGenMap();
    BlockSlot& here = slots_[slotIndex(entryPc)];
    const std::uint16_t nowGen = map.generation(entryPc);
    if (neg_.contains(entryPc, nowGen))
    {
        ++stats_.negativeHit;
        return nullptr;
    }

    PlanCtx ctx{&cpu};
    const PlanSource src{&readCodeWord, &ctx};
    const PlanGenSource gen{&pageGeneration, &mappingEpochOf, &ctx};

    BlockPlan plan{};
    if (!BlockPlanner::plan(src, gen, entryPc, plan))
    {
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    // 出口の ir / irc は翻訳時に読める。I4 が fallThroughPc + 2 まで
    // 同一 1KB ページを保証しているので、必ず窓の中にある。
    u16 fallIr = 0;
    u16 fallIrc = 0;
    if (!cpu.peekCodeWord(plan.fallThroughPc, fallIr) ||
        !cpu.peekCodeWord(plan.fallThroughPc + 2, fallIrc))
    {
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    const std::size_t need = requiredSize(plan, fallIr, fallIrc);
    if (need == 0)
    {
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    if (need > kStagingBytes)
    {
        // 発行用の控えに収まらない。段 1 の kMaxOps = 4 では起きないが、
        // 上限を上げたときに黙って壊れないよう明示的に諦める。
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    std::uint8_t* buf = code_->allocate(need);
    if (buf == nullptr)
    {
        // 使い切った。**以後は翻訳を試みない。**
        //
        // Why not 毎回試すか: 溢れてからも allocate を呼び続けると、
        // ブロックに当たらない命令のたびに翻訳のコストを払う。
        // 段 3 で eviction を入れるまでは、埋まったら諦める。
        codeFull_ = true;
        ++stats_.translateFail;
        return nullptr;
    }

    // **通常の RAM へ発行してから写す。** IRAM へバイト書き込みをすると
    // LoadStoreError で落ちる (実機で踏んだ)。
    EmittedBlock emitted{};
    if (!emitBlock(plan, fallIr, fallIrc, staging_, need, emitted))
    {
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    // 32bit 単位で写す。allocate が 4 バイト整列を返すので、
    // 端数は最後の 1 語だけ読み書きすれば足りる。
    const std::size_t words = (need + 3) / 4;
    auto* dst = reinterpret_cast<volatile std::uint32_t*>(buf);
    const auto* words32 = reinterpret_cast<const std::uint32_t*>(staging_);
    for (std::size_t i = 0; i < words; ++i)
    {
        dst[i] = words32[i];
    }

    // **書いたら確定させる。** isync を通さないと命令フェッチが
    // 書く前の中身を拾いうる (1 回目だけ壊れる形で出る)。
    code_->commit();

    BlockSlot& slot = here;
    // **鍵だけを写す。** ops[] は翻訳の途中でしか要らない。
    slot.entryPc = plan.entryPc;
    slot.mappingEpoch = plan.mappingEpoch;
    slot.page = plan.page;
    slot.pageGen = plan.pageGen;
    slot.count = plan.count;
    slot.code = buf + emitted.entryOffset;
    slot.endsWithBranch = emitted.endsWithBranch;
    slot.branchTarget = emitted.branchTarget;
    return &slot;
}

NativeResult BlockRunner::run(M68k& cpu)
{
    // 設計 §5.4: 入口で必ず見る。
    if (cpu.mustDeferToStep())
    {
        ++stats_.deferInterrupt;
        return NativeResult{0, NativeExit::kDeferToStep};
    }

    // **写像が変わったら負の記憶を全部捨てる。**
    //
    // 世代はページ単位だが写像は全体に効く。個別に持つより一括で捨てる方が
    // 安く、かつ漏れようがない。写像が動くのは setFastRam / setFastRom /
    // reset / loadStateForTest だけなので稀。
    const u32 nowEpoch = cpu.codeGenMap().mappingEpoch();
    if (nowEpoch != seenEpoch_)
    {
        neg_.clear();
        seenEpoch_ = nowEpoch;
    }

    // 現在の命令語アドレス。プリフェッチの契約により pc は「命令語 + 4」。
    const M68kState& st = cpu.state();
    const u32 entryPc = st.pc - 4;

    BlockSlot* slot = &slots_[slotIndex(entryPc)];

    // 設計 §5.5 の順で照合する。**順序が意味を持つ。**
    CodeGenMap& map = cpu.codeGenMap();
    const std::uint16_t nowGen = map.generation(slot->page << CodeGenMap::kPageShift);
    const bool hit = slot->code != nullptr &&                     // 翻訳済み
                     slot->entryPc != 0 &&                        // 空きの番兵
                     slot->entryPc == entryPc &&                  // タグ
                     slot->count <= kMaxOps &&                    // ゴミ検査
                     slot->mappingEpoch == map.mappingEpoch() &&  // 写像
                     nowGen != CodeGenMap::kAlwaysStale &&        // **先に見る**
                     nowGen == slot->pageGen;                     // 世代の一致

    if (!hit)
    {
        if (slot->code != nullptr)
        {
            // 鍵が外れた理由を分けて数える。段 3 の判断材料になる。
            if (nowGen == CodeGenMap::kAlwaysStale)
            {
                ++stats_.keyMissStale;
            }
            else if (slot->mappingEpoch != map.mappingEpoch())
            {
                ++stats_.keyMissEpoch;
            }
            else if (slot->entryPc != entryPc)
            {
                ++stats_.keyMissTag;
            }
            else
            {
                ++stats_.keyMissGen;
            }
        }
        slot = translate(cpu, entryPc);
        if (slot == nullptr)
        {
            ++stats_.deferUnsupported;
            return NativeResult{0, NativeExit::kDeferToStep};
        }
    }

    // 生成コードを呼ぶ。**引数の順は (state, code)。**
    const std::uint32_t ret = runBlock(&cpu.state(), slot->code);

    ++stats_.blocksRun;
    stats_.insnsRun += slot->count;

    const bool branchTaken = (ret & kBranchTakenFlag) != 0;
    const u32 cycles = ret & ~kBranchTakenFlag;

    if (branchTaken)
    {
        // 設計 §5.2: プリフェッチを詰め直すのは成立側だけ。
        if (!cpu.branchTo(slot->branchTarget))
        {
            // I7 が翻訳時に奇数を弾いているので通常は起きない。
            // 起きたら branchTo が既にアドレスエラーへ入っている。
            return NativeResult{cycles, NativeExit::kRan};
        }
    }

    return NativeResult{cycles, NativeExit::kRan};
}

NativeResult BlockRunner::runThunk(void* context, M68k& cpu)
{
    return static_cast<BlockRunner*>(context)->run(cpu);
}

const NativeStats* BlockRunner::statsThunk(void* context)
{
    return &static_cast<BlockRunner*>(context)->stats_;
}

}  // namespace x68k::jit
