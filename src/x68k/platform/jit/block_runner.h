// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ブロックキャッシュと実行器。core/ が教わる NativeExec の実体。
//
// ## 何をするか
//
// 現在の PC からブロックを 1 本探し、あれば生成コードを呼ぶ。無ければ
// 翻訳して置く。翻訳できなければインタプリタへ落とす。
//
// ## 鍵の照合 (設計 §5.5)
//
// **順序が意味を持つ。** kAlwaysStale の検査を世代の一致より先に置く。
// 逆順だと CodeGenMap 未配線時に 0xFFFF == 0xFFFF が成立して「常に有効」に
// なり、自己書き換えを一切検出しないブロックキャッシュができる。
//
// ## 割り込みの受理点 (設計 §5.4)
//
// 入口で M68k::mustDeferToStep() を見る。これが無いと、割り込みが立った後も
// ネイティブが成功し続ける限り step() が呼ばれず、次に reachSlow へ落ちる
// までの最大 80,000 サイクル受理が遅れる。

#ifndef X68K_PLATFORM_JIT_BLOCK_RUNNER_H
#define X68K_PLATFORM_JIT_BLOCK_RUNNER_H

#include <cstddef>
#include <cstdint>

#include "cpu/block_plan.h"
#include "cpu/m68k.h"
#include "cpu/native_exec.h"
#include "jit/exec_memory.h"

namespace x68k::jit
{

// キャッシュの 1 スロット。
struct BlockSlot
{
    // 計画そのもの。鍵もここに入っている。
    BlockPlan plan;
    // 生成コードのエントリポイント。nullptr なら未翻訳。
    const std::uint8_t* code = nullptr;
    // **翻訳できないことを覚えた番地。** 0 なら覚えていない。
    //
    // Why 要るか: 翻訳に失敗しても何も残さないと、同じ番地に来るたび
    // BlockPlanner::plan() を呼び直す。実機で**翻訳失敗がブロック実行の
    // 4.4 倍**発生していた (8,819,504 回 / 1,999,173 本)。ブロックに
    // ならない命令のたびに翻訳の実費を払っており、これが最大の損だった。
    //
    // Why 別のフィールドにするか: 成功した計画と同じ場所へ書くと、
    // 「翻訳できた」と「翻訳できないと分かっている」が区別できない。
    // 分けておけば、同じスロットに成功ブロックと失敗番地が同居できる
    // (畳み先が同じでも互いを追い出さない)。
    std::uint32_t failedPc = 0;
    // 失敗を覚えた時点の世代と写像。**これが変わったら覚え直す。**
    //
    // ゲストが RAM を書き換えれば「翻訳できない」も変わりうる。
    // 成功したブロックと同じ pull 型の検査を、失敗側にも適用する。
    std::uint16_t failedGen = 0;
    std::uint32_t failedEpoch = 0;
    // 分岐で終端したか (戻り値の bit31 を見てよいか)。
    bool endsWithBranch = false;
    // 分岐成立時の飛び先。
    std::uint32_t branchTarget = 0;
};

// ブロックキャッシュ。
//
// **内部 SRAM に置く。** ブロックの入口で毎回引くので、PSRAM への
// 散らばったアクセスは避ける (実機でエミュレーションを止めた実績がある)。
class BlockRunner
{
public:
    // スロットと実行可能メモリを教わる。どちらも外が確保する
    // (core/ と同じ「外から教わる」流儀)。
    void setStorage(BlockSlot* slots, std::uint32_t slotCount, ExecMemory* code)
    {
        slots_ = slots;
        slotCount_ = slots != nullptr ? slotCount : 0;
        code_ = code;
    }

    [[nodiscard]] bool isReady() const
    {
        return slots_ != nullptr && code_ != nullptr && code_->isReady();
    }

    // NativeExec に渡す関数。
    static NativeResult runThunk(void* context, M68k& cpu);
    static const NativeStats* statsThunk(void* context);

    [[nodiscard]] NativeExec exec()
    {
        return NativeExec{&BlockRunner::runThunk, &BlockRunner::statsThunk, this};
    }

    [[nodiscard]] const NativeStats& stats() const
    {
        return stats_;
    }

    // 全部捨てる。実行可能メモリも巻き戻す。
    void reset();

private:
    NativeResult run(M68k& cpu);
    static void rememberFailure(BlockSlot& slot, std::uint32_t entryPc, std::uint16_t gen,
                                std::uint32_t epoch);
    // 翻訳して置く。置けたらスロットを返す。
    BlockSlot* translate(M68k& cpu, std::uint32_t entryPc);
    [[nodiscard]] std::uint32_t slotIndex(std::uint32_t pc) const
    {
        // PC は必ず偶数なので 1 bit 落としてから畳む。
        return (pc >> 1) & (slotCount_ - 1);
    }

    // 発行はここへ書き、確定するときに実行可能メモリへ 32bit 単位で写す。
    //
    // **IRAM へ直接バイト書き込みをしてはいけない。** ESP32-S3 の IRAM は
    // 32bit 単位でしか読み書きできず、バイト書き込みは LoadStoreError で
    // 落ちる。実際に踏んだ (emit16 の out[0] = ... で panic)。
    // MALLOC_CAP_32BIT を付けても「32bit でアクセスすれば使える」だけで、
    // バイト書き込みが許されるようにはならない。
    //
    // 1 ブロックの上限は requiredSize が返す値なので、kMaxOps = 4 の
    // 最悪ケースに余裕を見て 512 バイト取る。
    static constexpr std::size_t kStagingBytes = 512;
    alignas(4) std::uint8_t staging_[kStagingBytes]{};

    BlockSlot* slots_ = nullptr;
    std::uint32_t slotCount_ = 0;
    ExecMemory* code_ = nullptr;
    NativeStats stats_{};
    // 実行可能メモリを使い切ったら、それ以上翻訳しない。
    bool codeFull_ = false;
};

}  // namespace x68k::jit

#endif  // X68K_PLATFORM_JIT_BLOCK_RUNNER_H
