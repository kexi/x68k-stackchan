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

// --- マウス ----------------------------------------------------------------

bool MouseQueue::begin()
{
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ != nullptr;
}

void MouseQueue::push(int dx, int dy, bool leftButton, bool rightButton)
{
    if (mutex_ == nullptr)
    {
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    // 足し込む。エミュレーションコアが引き取るまでに何度も呼ばれるので、
    // 上書きすると指の動きの大半が捨てられる。
    pendingDx_ += dx;
    pendingDy_ += dy;
    // ボタンは最後の状態が正しい。押した/離したは足し合わせられない。
    leftButton_ = leftButton;
    rightButton_ = rightButton;
    xSemaphoreGive(mutex_);
}

void MouseQueue::drain(x68k::Machine& machine)
{
    if (mutex_ == nullptr)
    {
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const int dx = pendingDx_;
    const int dy = pendingDy_;
    const bool left = leftButton_;
    const bool right = rightButton_;
    pendingDx_ = 0;
    pendingDy_ = 0;
    xSemaphoreGive(mutex_);

    // 動きもボタンの変化も無ければ送らない。
    //
    // Why not 毎回送らないか: 1 レポートごとに SCC が受信割り込みを上げる。
    // 触っていない間も送り続けると、IOCS のマウス割り込みハンドラが
    // スライスごとに走って 68000 の時間を食う。実効 3MHz ではこれが効く。
    const bool hasMotion = dx != 0 || dy != 0;
    const bool hasButtonChange = left != sentLeftButton_ || right != sentRightButton_;
    if (!hasMotion && !hasButtonChange)
    {
        return;
    }

    sentLeftButton_ = left;
    sentRightButton_ = right;

    // 飽和は Scc::moveMouse が持つ (レポートは 1 バイト符号付き)。
    machine.moveMouse(dx, dy, left, right);
}

}  // namespace x68k_platform
