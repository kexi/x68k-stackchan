// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "key_queue.h"

#include "io/ascii_keymap.h"

namespace x68k_platform
{
namespace
{

// 溜められる文字数。タイプ入力なら数文字ぶんあれば足りるが、
// シリアルから一行まとめて流し込む使い方をするので余裕を持たせる。
constexpr UBaseType_t kQueueLength = 64;

}  // namespace

bool KeyQueue::begin()
{
    queue_ = xQueueCreate(kQueueLength, sizeof(char));
    return queue_ != nullptr;
}

void KeyQueue::push(char c)
{
    if (queue_ == nullptr)
    {
        return;
    }
    // 待たない。溢れたら捨てる。
    //
    // Why not 空くまで待つか: 呼ぶのは表示コアのループなので、
    // ここで待つと画面更新が止まる。キーを 1 つ取りこぼす方が軽い。
    xQueueSend(queue_, &c, 0);
}

void KeyQueue::drain(x68k::Machine& machine)
{
    if (queue_ == nullptr)
    {
        return;
    }

    if (stepsLeft_ > 0)
    {
        --stepsLeft_;
        return;
    }

    // 押下だけ送ってあるなら、先に解放を送る。
    const bool hasPendingRelease = pendingRelease_ != 0;
    if (hasPendingRelease)
    {
        machine.pressKey(static_cast<x68k::u8>(pendingRelease_ | 0x80u));
        pendingRelease_ = 0;
        stepsLeft_ = kStepsPerEvent;
        return;
    }

    char c = 0;
    if (xQueueReceive(queue_, &c, 0) != pdTRUE)
    {
        return;
    }

    const x68k::u8 code = x68k::asciiToScanCode(c);
    const bool isTypable = code != 0;
    if (!isTypable)
    {
        return;
    }

    machine.pressKey(code);
    pendingRelease_ = code;
    stepsLeft_ = kStepsPerEvent;
}

}  // namespace x68k_platform
