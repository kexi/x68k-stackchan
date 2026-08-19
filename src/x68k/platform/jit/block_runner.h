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
#include "jit/negative_cache.h"

namespace x68k::jit
{

// キャッシュの 1 スロット。
//
// **BlockPlan をまるごと持たない。** 実行時に要るのは鍵と生成コードの
// 位置とサイクル数だけで、ops[] (20 バイト x 4 = 80 バイト) は翻訳の
// 途中でしか使わない。落とすと 1 スロット 120 → 40 バイトになり、
// 同じ内部 SRAM で **3 倍のスロット**が置ける。
//
// タグ外れが実行の 21% あったので、ここが効く。
struct BlockSlot
{
    // --- 鍵 (§5.5 の順で照合する) ---
    std::uint32_t entryPc = 0;       // 0 は空きスロットの番兵
    std::uint32_t mappingEpoch = 0;  // 写像の世代
    std::uint32_t page = 0;          // 先頭バイトのページ番号
    std::uint16_t pageGen = 0;       // そのページの世代
    std::uint8_t count = 0;          // 命令数 (統計とゴミ検査に使う)
    // 生成コードのエントリポイント。nullptr なら未翻訳。
    const std::uint8_t* code = nullptr;
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

    // 「翻訳できない」を覚える表を教わる。**成功ブロックとは別に持つ**
    // (negative_cache.h の冒頭に理由がある)。教わらなくても動く。
    void setNegativeStorage(NegEntry* entries, std::uint32_t count)
    {
        neg_.setStorage(entries, count);
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
    void rememberFailure(std::uint32_t entryPc, std::uint16_t gen);
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
    // 最悪ケースに余裕を見て取る。
    //
    // **Tier B (読みガード) で 512 → 1024 へ広げた。** ガード付き 1 命令は
    // EA 計算 + ガード + commit + バイト 4 本の合成 + 本体で、Tier A の
    // 2-3 倍になる。加えて脱出用の出口の島が命令ごとに 1 つ増える。
    // 足りなければ translate が諦めるだけなので正しさは損なわれないが、
    // 諦めた分は素通りするので気づきにくい。
    //
    // **Tier C (書きガード) で 1024 → 1536 へ広げた。** 実測の最悪
    // (CLR.l (An) x 4) が 949 バイトで、1024 に対して余裕が 7% しか無かった。
    // 書き 1 命令は「ガード 3 本 + touch 2 組 + バイト 4 本の store + 島 2 つ」
    // で読み形の 1.5 倍近くあり、命令を 1 つ足しただけで超える。
    // 超えたら黙って諦める (素通りする) 形なので、先に広げておく。
    static constexpr std::size_t kStagingBytes = 1536;
    alignas(4) std::uint8_t staging_[kStagingBytes]{};

    BlockSlot* slots_ = nullptr;
    std::uint32_t slotCount_ = 0;
    ExecMemory* code_ = nullptr;
    // 飽和したページに当たった回数。閾値を超えたら世代を捨てて数え直す。
    //
    // Why 即座に捨てないか: 1 回の飽和で全部の世代を捨てると、正常な
    // ブロックまで再翻訳になる。まとまった数が溜まってからにする。
    static constexpr std::uint32_t kSaturationResetThreshold = 64;
    std::uint32_t saturatedSeen_ = 0;
    NegativeCache neg_{};
    // 直近に見た写像の世代。変わったら負の記憶を全部捨てる。
    std::uint32_t seenEpoch_ = 0;
    NativeStats stats_{};
    // 実行可能メモリを使い切ったら、それ以上翻訳しない。
    bool codeFull_ = false;
};

}  // namespace x68k::jit

#endif  // X68K_PLATFORM_JIT_BLOCK_RUNNER_H
