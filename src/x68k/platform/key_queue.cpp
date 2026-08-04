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

    // 飽和は Scc::moveMouse が持つ (レポートは 1 バイト符号付き)。
    const bool accepted = machine.moveMouse(dx, dy, left, right);
    if (accepted)
    {
        sentLeftButton_ = left;
        sentRightButton_ = right;
        return;
    }

    // 断られたら、送ろうとしたぶんを丸ごと差し戻す。次の drain() で送り直す。
    //
    // SCC は FIFO に 3 バイトの空きが無いとレポートを丸ごと捨てる。CPU が
    // 割り込みを止めている間 (レベル 5 のマスク) に入力が続くとこれが起きる。
    //
    // Why not 送る前に sentLeftButton_ を更新してしまわないか: ボタンは
    // 「押されている/いない」という状態であって、変化そのものは 1 度しか
    // 流れない。送ったことにして捨てられると、次の drain() では left ==
    // sentLeftButton_ になって「変化なし」と判断され、解放が二度と届かない。
    // ゲストから見るとボタンが押されたままになり、SX-Window のドラッグが
    // 終わらない。受理されたときだけ状態を進めれば、解放は必ず届く。
    //
    // Why not 移動量だけ捨てて済ませないか: dx/dy は相対量なので、捨てた
    // ぶんの距離は永久に失われる。ボタンと違って後から辻褄が合う値が
    // 来ることは無く、指を滑らせた距離とカーソルの移動量がずれ続ける。
    // 足し戻せば「1 スライスぶん遅れる」だけで距離は保たれる。
    //
    // Why not 差し戻さずリトライ用の別のフィールドを持たないか: push() が
    // 足し込む先と同じ変数へ戻せば、差し戻した後に届いた動きも自然に
    // 合流する。2 つ持つと「どちらを先に送るか」という順序の問題が生まれ、
    // レポートの中身が時系列と食い違う余地ができる。
    xSemaphoreTake(mutex_, portMAX_DELAY);
    pendingDx_ += dx;
    pendingDy_ += dy;
    xSemaphoreGive(mutex_);
}

}  // namespace x68k_platform
