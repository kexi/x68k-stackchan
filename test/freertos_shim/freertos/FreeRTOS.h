// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ホストテスト用の FreeRTOS の代役。実機の <freertos/FreeRTOS.h> の代わりに
// これをインクルードさせる。
//
// なぜ要るか: MouseQueue (src/x68k/platform/key_queue.*) の「SCC に断られた
// レポートを差し戻して送り直す」という判断は platform 層にしか無く、
// core/ のテストでは触れない。この判断が抜けるとゲストのボタンが
// 押しっぱなしになる (SX-Window のドラッグが終わらない) ため、実機に焼かずに
// 確かめる手段が要る。
//
// Why not MouseQueue から FreeRTOS への依存を抜いて素の std::mutex にしないか:
// 実機で MouseQueue を触るのは表示コアとエミュレーションコアの 2 つで、
// FreeRTOS のミューテックスは優先度継承を持つ。std::mutex に置き換えると
// ESP32 側の実挙動が変わる。テストの都合で実機の同期プリミティブを
// 変えるのは順序が逆なので、テスト側に代役を置く。
//
// Why not 本物の FreeRTOS をホストへ移植 (POSIX port) しないか: ここで要るのは
// 「排他が取れること」だけで、スケジューラもタイマも要らない。テストは
// 単一スレッドで drain() を呼ぶので、取得と解放を数えるだけで足りる。
// 移植を持ち込むと、テストの失敗がテスト基盤の問題か実装の問題か
// 切り分けられなくなる。

#ifndef X68K_TEST_FREERTOS_SHIM_FREERTOS_H
#define X68K_TEST_FREERTOS_SHIM_FREERTOS_H

#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned int;
using TickType_t = std::uint32_t;

constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = pdTRUE;

constexpr TickType_t portMAX_DELAY = 0xFFFFFFFFu;

#endif  // X68K_TEST_FREERTOS_SHIM_FREERTOS_H
