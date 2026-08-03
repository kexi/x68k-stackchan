// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 打たれたキーをエミュレーションコアへ渡す。
//
// なぜ要るか (2 つある):
//
// 1. データ競合。Machine::pressKey() は MFP のキー受信レジスタを書く。
//    エミュレーションコアは同じレジスタを割り込み処理で読むので、
//    表示コアから直接呼ぶと競合する。
//
// 2. 取りこぼし。MFP の受信レジスタは 1 バイトしか保持しない。押下と
//    解放を続けざまに書くと、CPU が読み出す前に上書きされて入力が
//    丸ごと消える。実効 3〜4MHz では 1 バイト読むのに数フレームかかる。
//
// 打たれた文字を溜めておき、エミュレーションコアが自分のペースで
// 1 イベントずつ MFP へ渡す。押下と解放の間隔もそこで空ける。
//
// Why not FreeRTOS の queue をそのまま使うか: 使っている。この型は
// 「ASCII を積むと押下と解放に分けて送られる」という約束を持たせる薄い
// 包みで、間隔の調整もここに閉じ込めてある。呼ぶ側が押下・解放の対を
// 意識せずに済む。

#ifndef X68K_PLATFORM_KEY_QUEUE_H
#define X68K_PLATFORM_KEY_QUEUE_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "machine.h"

namespace x68k_platform
{

class KeyQueue
{
public:
    // 押下と解放の間に空けるフレーム数。
    //
    // drain() は表示の周期 (約 60Hz) ではなくエミュレーションの
    // スライスごとに呼ばれる。実効 3〜4MHz だと 1 スライス 20000
    // サイクルが約 5ms なので、4 スライスで 20ms 程度になる。
    // Human68k が受信レジスタを読むには十分な間隔。
    static constexpr int kStepsPerEvent = 4;

    bool begin();

    // --- どのコアからでも呼べる ---

    // 打たれた文字を積む。溢れたら捨てる (押しっぱなしで詰まるより、
    // 取りこぼす方が扱いやすい)。
    void push(char c);

    // --- エミュレーションコアから呼ぶ ---

    // 溜まったキーを 1 イベントずつ MFP へ流す。スライスごとに呼ぶ。
    void drain(x68k::Machine& machine);

private:
    QueueHandle_t queue_ = nullptr;

    // 押下を送った後、解放を送るまでの待ち。
    int stepsLeft_ = 0;
    x68k::u8 pendingRelease_ = 0;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_KEY_QUEUE_H
