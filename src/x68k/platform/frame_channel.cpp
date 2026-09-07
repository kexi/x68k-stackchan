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

bool FrameChannel::begin(x68k::u16* bufferA, x68k::u16* bufferB)
{
    const bool invalidBuffers = bufferA == nullptr || bufferB == nullptr || bufferA == bufferB;
    const bool alreadyInitialized = mutex_ != nullptr;
    if (invalidBuffers || alreadyInitialized)
    {
        return false;
    }

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr)
    {
        return false;
    }

    writeBuffer_ = bufferA;
    frontBuffer_ = bufferB;
    hasNewFrame_ = false;
    isFrontInUse_ = false;
    return true;
}

x68k::u16* FrameChannel::tryWriteBuffer()
{
    const bool isInitialized = mutex_ != nullptr;
    if (!isInitialized)
    {
        return nullptr;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool mayPublish = !isFrontInUse_ && !hasNewFrame_;
    auto* const result = mayPublish ? writeBuffer_ : nullptr;
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

    // Core0 が転送中なら入れ替えない。
    //
    // 入れ替えてしまうと、転送に使っている実体が Core1 の書き込み先に
    // なり、SPI が読んでいる最中に内容が変わる。LCD には新旧が混ざった
    // フレームが出る (この現象は PSRAM を直接 DMA していたときに実際に
    // 見た)。
    //
    // 待たずに捨てる。次のスライスで作り直せばよく、表示が追いつかない
    // ときは古いフレームを飛ばす方が、遅れて溜まるより見た目がよい。
    if (isFrontInUse_)
    {
        xSemaphoreGive(mutex_);
        return false;
    }

    x68k::u16* const justWritten = writeBuffer_;
    writeBuffer_ = frontBuffer_;
    frontBuffer_ = justWritten;
    hasNewFrame_ = true;

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

    x68k::u16* result = nullptr;
    if (hasNewFrame_)
    {
        result = frontBuffer_;
        hasNewFrame_ = false;
        isFrontInUse_ = true;
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
    isFrontInUse_ = false;
    xSemaphoreGive(mutex_);
}

}  // namespace x68k_platform
