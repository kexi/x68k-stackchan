// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 「この番地は翻訳できない」を覚える表。**成功ブロックとは別に持つ。**
//
// ## なぜ別表なのか
//
// 最初は成功ブロックのスロットへ相乗りさせた。動いたが、実測で
// 翻訳失敗が 8,819,504 → 4,914,288 までしか減らなかった (予測は 2 万未満)。
// 畳み先が同じ失敗番地同士が互いを追い出していたため。
//
// **相乗りにはもっと悪い失敗もある。** 負の記憶が成功ブロックを追い出すと、
// 追い出された番地は再翻訳のたびに実行可能メモリを新しく食う (解放しない)。
// 負と正が 1 組で往復するだけで 16KB を数秒で使い切り、codeFull_ で
// JIT 全体が止まる。別表ならこれが原理的に起きない。
//
// ## 無効化 (pull 型)
//
// ページ世代を一緒に覚える。ゲストがそのページへ 1 バイトでも書けば
// 世代が動き、記憶は自分から外れて再翻訳を試みる。
//
// 写像 (mappingEpoch) が変わったときは呼び出し側が clear() する。
// 世代はページ単位だが写像は全体に効くので、個別に持つより一括で捨てる方が
// 安く、かつ漏れようがない。

#ifndef X68K_PLATFORM_JIT_NEGATIVE_CACHE_H
#define X68K_PLATFORM_JIT_NEGATIVE_CACHE_H

#include <cstdint>

namespace x68k::jit
{

// 記憶 1 件。8 バイト。
struct NegEntry
{
    // 翻訳できなかった番地。**0 は空きの番兵。**
    std::uint32_t pc;
    // 失敗を観測した時点の、その番地のページ世代。
    std::uint16_t pageGen;
    std::uint16_t pad;
};

class NegativeCache
{
public:
    // 表を教わる。**count は 2 の冪でなければならない** (マスクで畳むため)。
    void setStorage(NegEntry* entries, std::uint32_t count)
    {
        const bool powerOfTwo = count != 0 && (count & (count - 1)) == 0;
        entries_ = powerOfTwo ? entries : nullptr;
        mask_ = entries_ != nullptr ? count - 1 : 0;
    }

    [[nodiscard]] bool isReady() const
    {
        return entries_ != nullptr;
    }

    // 「今の世代でも翻訳できない」と覚えているか。
    //
    // gen は呼び出し側が引いて渡す。CodeGenMap に依存させないことで、
    // この表だけをホストで全数テストできる (命令長デコーダと同じ流儀)。
    [[nodiscard]] bool contains(std::uint32_t pc, std::uint16_t gen) const
    {
        if (entries_ == nullptr || pc == 0)
        {
            return false;
        }
        const NegEntry& e = entries_[index(pc)];
        return e.pc == pc && e.pageGen == gen;
    }

    // 失敗を覚える。
    void insert(std::uint32_t pc, std::uint16_t gen)
    {
        if (entries_ == nullptr || pc == 0)
        {
            return;
        }
        NegEntry& e = entries_[index(pc)];
        e.pc = pc;
        e.pageGen = gen;
    }

    void clear()
    {
        if (entries_ == nullptr)
        {
            return;
        }
        for (std::uint32_t i = 0; i <= mask_; ++i)
        {
            entries_[i] = NegEntry{};
        }
    }

private:
    // 下位ビットだけで畳むと、**2*count バイト周期の番地が全部衝突する**。
    // ページ番号のビット (>> 11 = 2KB 単位) を混ぜて散らす。
    [[nodiscard]] std::uint32_t index(std::uint32_t pc) const
    {
        return ((pc >> 1) ^ (pc >> 11)) & mask_;
    }

    NegEntry* entries_ = nullptr;
    std::uint32_t mask_ = 0;
};

}  // namespace x68k::jit

#endif  // X68K_PLATFORM_JIT_NEGATIVE_CACHE_H
