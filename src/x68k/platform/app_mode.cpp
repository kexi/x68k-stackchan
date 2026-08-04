// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "app_mode.h"

namespace x68k_platform
{
namespace
{

// 要求を「行き先」へ解釈する。
//
// Toggle だけが今のモードを見る。ここで解釈を閉じておけば、呼ぶ側が
// 現在値を読んでから要求を出すまでの隙間が生まれない (app_mode.h の
// ModeRequest のコメントを見よ)。
AppMode resolveTarget(ModeRequest request, AppMode current)
{
    switch (request)
    {
        case ModeRequest::ToFace:
            return AppMode::Face;
        case ModeRequest::ToX68k:
            return AppMode::X68k;
        case ModeRequest::Toggle:
            return current == AppMode::Face ? AppMode::X68k : AppMode::Face;
    }
    // ここには来ない。列挙の外の値が来たら今のモードを保つ。
    //
    // Why not assert で落とさないか: 実機に assert は無い (NDEBUG)。
    // 落ちない代わりに「切り替わらない」で済ませる方が、顔も画面も
    // 出ない状態より扱いやすい。
    return current;
}

}  // namespace

ModeTransition AppModeMachine::request(ModeRequest request)
{
    const AppMode target = resolveTarget(request, mode_);

    ModeTransition transition;
    transition.from = mode_;
    transition.to = target;
    transition.changed = target != mode_;

    if (!transition.changed)
    {
        // 同じモードへの要求。画面もマウスも触らない。
        //
        // Why not shouldRedraw を立てないか: 立てると、シリアルから
        // 'f' を連打しただけで毎回全画面を作り直すことになる。
        // 320x240 の変換と SPI 転送はスライス数回ぶんの重さがある。
        return transition;
    }

    mode_ = target;

    // どちらの向きでも描き直す。顔と X68000 は同じ 320x240 を奪い合う
    // ので、切り替えた直後は相手の描いた内容が残っている。
    transition.shouldRedraw = true;

    // 顔へ入るときだけボタンを離す。
    //
    // Why not X68K へ戻るときにも離さないか: 戻る時点でゲストのボタンは
    // 既に離れている (顔へ入るときに離した)。もう一度離すと、SCC へ
    // 変化の無いレポートを積んで割り込みを無駄に上げるだけになる。
    // MouseQueue は変化が無ければ送らないので実害は出ないが、
    // 「なぜここで離すのか」を説明できない呼び出しを残さない。
    transition.shouldReleaseMouseButtons = target == AppMode::Face;

    return transition;
}

bool AppModeMachine::shouldRunEmulation() const
{
    if (mode_ == AppMode::X68k)
    {
        return true;
    }
    // 顔モード。Paused のときだけ止める。
    return policy_ != EmulationPolicy::Paused;
}

std::uint32_t AppModeMachine::sliceCycles(std::uint32_t base) const
{
    if (mode_ == AppMode::X68k)
    {
        return base;
    }

    switch (policy_)
    {
        case EmulationPolicy::KeepRunning:
            return base;
        case EmulationPolicy::Throttled:
            // 0 サイクルの run() は何も進めないまま vTaskDelay へ落ちる。
            // base が kThrottleDivisor 未満のときに 0 を返すと、
            // Throttled のつもりが Paused と区別できなくなる。
            // 最低 1 サイクルは進める。
            return base / kThrottleDivisor > 0 ? base / kThrottleDivisor : 1;
        case EmulationPolicy::Paused:
            return 0;
    }
    return base;
}

}  // namespace x68k_platform
