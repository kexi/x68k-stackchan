// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ゲスト RAM のページ世代。デコード済みブロックが古くなったことを知るための土台。
//
// ## なぜ「世代」なのか (pull 型)
//
// ブロックキャッシュは「この番地の命令列はこう」という前提を持つ。ゲストが
// その番地を書き換えたら前提が崩れる。崩れたことをどう知るかで 2 通りある。
//
//   push 型: 書き込み側が「該当するブロックを消す」
//   pull 型: ブロック側が「実行する前に前提が成り立つか確かめる」
//
// **push 型は採らない。** 一度それで失敗している。命令フェッチの窓を
// ポインタでキャッシュしたとき、窓が変わる 5 つの経路 (setFastRam /
// setFastRamReadable / setFastRom / loadStateForTest / アドレスエラー) の
// 全てで捨てる必要があり、**1 つ漏らしても既存テストが検出できなかった**
// (docs/knowledge/event-driven-implementation.md)。失敗の形が
// 「古い前提のまま静かに走り続ける」なので、症状が原因から遠い。
//
// pull 型なら「消し忘れたブロックを実行する」状態が **原理的に存在しない**。
// 古いブロックは入口の検査で自分から落ちる。フックを漏らした場合の失敗は
// 「世代が上がらない」であり、経路ごとに変異テストが書ける
// (書き換えてから実行し、新しい命令として動くか)。**検出できるかどうかが
// 決定的に違う。**
//
// ## 精度について
//
// 1 ページ 1KB で数える。コードとデータが同じページに同居すると、データを
// 書いただけで世代が上がる (偽共有)。そのため世代が動いたブロックは
// **命令語のバイト列を実メモリと照合**してから捨てる。同じなら世代を
// 控え直すだけで、翻訳はやり直さない。
//
// Why not ページを細かくしないか: ページ数だけ配列が要る。2MB / 1KB で
// 2048 エントリ = 4KB で済む。256B にすると 16KB になり、内部 SRAM の
// 逼迫した本プロジェクトでは割に合わない。偽共有が実測で問題になったら
// そのとき細かくする (再翻訳の回数を数える口を用意してある)。

#ifndef X68K_CORE_CPU_CODE_GEN_MAP_H
#define X68K_CORE_CPU_CODE_GEN_MAP_H

#include <cstdint>

#include "m68k_types.h"

namespace x68k
{

// ゲスト RAM の書き換えを世代で追う。
//
// ストレージは外から与える。core/ は ESP32 非依存なので、確保の仕方
// (PSRAM か内部 SRAM か) を知らない。setFastRam と同じ「教わる」形。
class CodeGenMap
{
public:
    // 1 ページの大きさ。2MB を 2048 ページで覆う。
    static constexpr u32 kPageShift = 10;  // 1KB
    static constexpr u32 kPageSize = 1u << kPageShift;

    // 「常に古いものとして扱う」を表す世代。
    //
    // 世代は 16bit なので、書き込みを数え続けると 65,536 回で一周して
    // 元の値に戻る。戻った瞬間、書き換えられたページが「変わっていない」
    // ことになり、**古いコードを静かに実行し続ける**。
    //
    // 一周させないために、上限に達したらそこで止める。以後そのページは
    // 世代の比較では判定できないので、呼び出し側はバイト照合へ落とす。
    // 遅くなるだけで、正しさは保たれる。
    //
    // 元に戻せるのはキャッシュを全部捨てたときだけ (resetGenerations)。
    static constexpr std::uint16_t kAlwaysStale = 0xFFFFu;

    // 世代の配列を教わる。length はページ数。
    void setStorage(std::uint16_t* generations, u32 pageCount)
    {
        gen_ = generations;
        pageCount_ = generations != nullptr ? pageCount : 0;
    }

    [[nodiscard]] bool isReady() const
    {
        return gen_ != nullptr;
    }

    // 書き込みを記録する。**分岐を持たない**。
    //
    // Why 条件を付けないか: 「翻訳済みのページだけ数える」ようにすると、
    // 稀にしか通らない経路で条件を書き忘れる余地が生まれる。無条件なら
    // その失敗形が存在しない。コストは load + add + store の 3 命令で、
    // 1 命令あたり平均 0.5 回の書き込みに対し 88 CPU サイクルの ~2%。
    //
    // 範囲外 (VRAM / I/O) は数えない。そこにコードは置かれない。
    void touch(u32 addr)
    {
        const u32 page = addr >> kPageShift;
        if (page < pageCount_)
        {
            // **飽和させる。** 単純な ++ だと 65,536 回の書き込みで
            // 元の値へ戻り、書き換えられたページが「変わっていない」と
            // 判定される。1KB ページに 65,536 回は珍しくない
            // (スタックやワーク領域なら数秒で到達する)。
            //
            // 失敗の形が「古いコードを静かに実行し続ける」なので、
            // 症状が原因から遠く、再現も難しい。ここで潰しておく。
            //
            // Why not 分岐を足さないか: 足している。ただし比較 1 本で、
            // ホットパスの他の判定 (page < pageCount_) と同じ形。
            // 飽和値に達したページは kAlwaysStale を返し続け、
            // 呼び出し側は必ずバイト照合へ落ちる (遅くなるが正しい)。
            const std::uint16_t current = gen_[page];
            gen_[page] =
                current == kAlwaysStale ? kAlwaysStale : static_cast<std::uint16_t>(current + 1);
        }
    }

    // ページの現在の世代。
    //
    // 範囲外は kAlwaysStale を返す。0 を返すと「まだ一度も書かれていない
    // ページ」と区別できず、範囲外に置いたブロックが有効と見なされる。
    [[nodiscard]] std::uint16_t generation(u32 addr) const
    {
        const u32 page = addr >> kPageShift;
        return page < pageCount_ ? gen_[page] : kAlwaysStale;
    }

    // 全部の世代を進める。実体が差し替わったときに使う
    // (reset / setFastRam / loadStateForTest)。個別に消して回るより、
    // まとめて 1 つ進める方が漏れようがない。
    void touchAll()
    {
        for (u32 i = 0; i < pageCount_; ++i)
        {
            const std::uint16_t current = gen_[i];
            gen_[i] =
                current == kAlwaysStale ? kAlwaysStale : static_cast<std::uint16_t>(current + 1);
        }
    }

    // 全部の世代を捨てて 0 から数え直す。
    //
    // 飽和した (kAlwaysStale になった) ページを元へ戻せる唯一の口。
    // **キャッシュを全部捨てた後にしか呼んではいけない。** 世代が 0 に
    // 戻るので、控えを持ったままだと「変わっていない」と誤判定する。
    void resetGenerations()
    {
        for (u32 i = 0; i < pageCount_; ++i)
        {
            gen_[i] = 0;
        }
    }

    // メモリの写像そのものが変わった回数。
    //
    // **ページの世代では表現できない変化がある。** setFastRam /
    // setFastRamReadable / setFastRom は「どの番地に何が見えるか」を
    // 変えるが、ゲスト RAM の中身は 1 バイトも書き換わらない。
    // ページ世代は書き込みしか数えないので、この変化を取りこぼす。
    //
    // 取りこぼすと何が起きるか: RESET 前後で ROM/RAM の見え方が入れ替わる
    // X68000 で、**古い写像を前提に翻訳したコードをそのまま実行する**。
    //
    // ブロックの鍵にこの値を含めれば、写像が変わった瞬間に全ブロックが
    // 自分から無効になる (pull 型のまま扱える)。
    [[nodiscard]] u32 mappingEpoch() const
    {
        return mappingEpoch_;
    }

    // 写像が変わったことを知らせる。
    void bumpMappingEpoch()
    {
        ++mappingEpoch_;
    }

private:
    std::uint16_t* gen_ = nullptr;
    u32 pageCount_ = 0;
    u32 mappingEpoch_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_CPU_CODE_GEN_MAP_H
