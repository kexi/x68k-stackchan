// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ホストテスト用の FreeRTOS キュー/ミューテックスの代役。FreeRTOS.h の
// 冒頭に置いた理由がそのまま当てはまる。
//
// 実機では xQueueCreate が失敗しうる (ヒープ不足) が、ここでは new が
// 投げない限り成功する。begin() の失敗経路は実機固有なのでテストしない。

#ifndef X68K_TEST_FREERTOS_SHIM_QUEUE_H
#define X68K_TEST_FREERTOS_SHIM_QUEUE_H

#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>

#include "FreeRTOS.h"

// 固定長の要素を FIFO で溜める。実機の xQueue と同じく、満杯なら送信は
// 失敗し、空なら受信は失敗する (待ち時間 0 の場合)。
struct X68kTestQueue
{
    std::size_t itemSize = 0;
    std::size_t capacity = 0;
    std::deque<std::vector<unsigned char>> items;
};

// 再帰でないミューテックス。テストは単一スレッドなので、実際に待つことは
// 無い。取り違え (二重取得や取らずに解放) を検出できるよう状態だけ持つ。
struct X68kTestMutex
{
    bool held = false;
};

using QueueHandle_t = X68kTestQueue*;
using SemaphoreHandle_t = X68kTestMutex*;

inline QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize)
{
    auto* queue = new X68kTestQueue();
    queue->itemSize = itemSize;
    queue->capacity = length;
    return queue;
}

inline void vQueueDelete(QueueHandle_t queue)
{
    delete queue;
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t /*ticksToWait*/)
{
    if (queue == nullptr || queue->items.size() >= queue->capacity)
    {
        return pdFALSE;
    }
    const auto* bytes = static_cast<const unsigned char*>(item);
    queue->items.emplace_back(bytes, bytes + queue->itemSize);
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t /*ticksToWait*/)
{
    if (queue == nullptr || queue->items.empty())
    {
        return pdFALSE;
    }
    std::memcpy(item, queue->items.front().data(), queue->itemSize);
    queue->items.pop_front();
    return pdTRUE;
}

inline SemaphoreHandle_t xSemaphoreCreateMutex()
{
    return new X68kTestMutex();
}

inline void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    delete mutex;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t /*ticksToWait*/)
{
    if (mutex == nullptr || mutex->held)
    {
        return pdFALSE;
    }
    mutex->held = true;
    return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    if (mutex == nullptr || !mutex->held)
    {
        return pdFALSE;
    }
    mutex->held = false;
    return pdTRUE;
}

#endif  // X68K_TEST_FREERTOS_SHIM_QUEUE_H
