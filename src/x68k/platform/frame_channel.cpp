// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "frame_channel.h"

namespace x68k_platform
{

FrameChannel::~FrameChannel()
{
    // 所有タスク停止後だけ破棄する。使用中のmutexを時間待ちで解放しない。
    const bool isInitialized = mutex_ != nullptr;
    if (isInitialized)
    {
        vSemaphoreDelete(mutex_);
    }
}

bool FrameChannel::begin(x68k::u16* bufferA, x68k::u16* bufferB, x68k::u16* bufferC)
{
    const bool hasNull = bufferA == nullptr || bufferB == nullptr || bufferC == nullptr;
    const bool hasDuplicate = bufferA == bufferB || bufferB == bufferC || bufferA == bufferC;
    const bool alreadyInitialized = mutex_ != nullptr;
    if (hasNull || hasDuplicate || alreadyInitialized)
    {
        return false;
    }

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr)
    {
        return false;
    }

    buffers_[0] = bufferA;
    buffers_[1] = bufferB;
    buffers_[2] = bufferC;
    for (int i = 0; i < kBuffers; ++i)
    {
        slots_[i] = Slot::Free;
        readySeq_[i] = 0;
    }
    nextSeq_ = 1;
    writingIndex_ = -1;
    return true;
}

x68k::u16* FrameChannel::writeBuffer()
{
    if (mutex_ == nullptr)
    {
        return nullptr;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    x68k::u16* const result = writingIndex_ >= 0 ? buffers_[writingIndex_] : nullptr;
    xSemaphoreGive(mutex_);
    return result;
}

x68k::u16* FrameChannel::tryWriteBuffer()
{
    const bool isInitialized = mutex_ != nullptr;
    if (!isInitialized)
    {
        return nullptr;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);

    // すでに書きかけがあるならそれを返す。合成の途中で別の枚へ移ると、
    // 半分だけ描かれた絵が 2 枚できる。
    if (writingIndex_ >= 0)
    {
        x68k::u16* const inProgress = buffers_[writingIndex_];
        xSemaphoreGive(mutex_);
        return inProgress;
    }

    // 空きを 1 枚借りる。転送中でも公開待ちでもない枚だけが対象。
    x68k::u16* result = nullptr;
    for (int i = 0; i < kBuffers; ++i)
    {
        if (slots_[i] != Slot::Free)
        {
            continue;
        }
        slots_[i] = Slot::Writing;
        writingIndex_ = i;
        result = buffers_[i];
        break;
    }

    xSemaphoreGive(mutex_);
    return result;
}

bool FrameChannel::publish()
{
    if (mutex_ == nullptr)
    {
        return false;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // 借りていないのに公開しようとしたら何もしない。
    if (writingIndex_ < 0)
    {
        xSemaphoreGive(mutex_);
        return false;
    }

    slots_[writingIndex_] = Slot::Ready;
    readySeq_[writingIndex_] = nextSeq_++;
    writingIndex_ = -1;

    xSemaphoreGive(mutex_);
    return true;
}

x68k::u16* FrameChannel::take()
{
    if (mutex_ == nullptr)
    {
        return nullptr;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // 転送中の枚があるうちは次を渡さない。Core0 は 1 枚ずつしか送れない。
    for (int i = 0; i < kBuffers; ++i)
    {
        if (slots_[i] == Slot::InTransfer)
        {
            xSemaphoreGive(mutex_);
            return nullptr;
        }
    }

    // Ready のうち一番古いものを渡す。新しい方を先に出すと、次に古い方が
    // 出たときに画面が巻き戻って見える。
    int oldest = -1;
    for (int i = 0; i < kBuffers; ++i)
    {
        if (slots_[i] != Slot::Ready)
        {
            continue;
        }
        if (oldest < 0 || readySeq_[i] < readySeq_[oldest])
        {
            oldest = i;
        }
    }

    x68k::u16* result = nullptr;
    if (oldest >= 0)
    {
        slots_[oldest] = Slot::InTransfer;
        result = buffers_[oldest];
    }

    xSemaphoreGive(mutex_);
    return result;
}

void FrameChannel::done()
{
    if (mutex_ == nullptr)
    {
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < kBuffers; ++i)
    {
        if (slots_[i] == Slot::InTransfer)
        {
            slots_[i] = Slot::Free;
        }
    }
    xSemaphoreGive(mutex_);
}

}  // namespace x68k_platform
