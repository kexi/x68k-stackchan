// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 顔の表示。
//
// 【重要】これは仮の実装。本物の Avatar ではない。
//
// 今描くのは「目 2 つと口 1 つ」の図形だけで、まばたきも表情の遷移も
// 口パクも無い。stackchan の Avatar (M5Stack-Avatar) が持つ機能は
// 何一つ入っていない。切り替えたときに顔モードだと分かる placeholder。
//
// Why not M5Stack-Avatar をそのまま持ってこないか (2 つある):
//
// 1. Avatar は自前で描画タスクを立てる。M5Stack-Avatar の Avatar::init は
//    FreeRTOS のタスクを起こし、そのタスクが M5.Display を直接叩き続ける。
//    このプロジェクトの表示は既に「エミュレーションコアが作った RGB565 を
//    表示コアが送る」という所有権で組んであり (frame_channel.h)、
//    3 つ目の書き手が同じ LCD を触ると SPI の操作が競合する。
//    載せるには Avatar 側の描画をこちらのループから駆動する形へ
//    直す必要があり、それは切り替え FSM とは別の仕事。
//
// 2. PSRAM の見積もりを先に確かめたい。Avatar はスプライトを PSRAM に
//    持つ。今の残量で足りるかは main.cpp の reserveMemory で先行予約して
//    測るのが確実で、そのためには「どれだけ要るか」を決めた枠が要る。
//    枠 (kSpriteBytes) を先に置いて実機のログで確かめてから、中身を
//    本物に差し替える。
//
// 本物にするときにやること:
//   draw() の中身を差し替える。呼ぶ側 (main.cpp の表示ループ) と
//   FSM の側は変えなくてよい。スプライトが要るなら setSpriteBuffer で
//   受け取る (main.cpp が起動直後に予約したもの)。

#ifndef X68K_PLATFORM_AVATAR_H
#define X68K_PLATFORM_AVATAR_H

#include <cstddef>
#include <cstdint>

namespace x68k_platform
{

// 顔の表情。
//
// 今は draw() が使っていない (仮の実装なので目と口の形は固定)。
// 本物へ差し替えるときに呼ぶ側を変えずに済むよう、口だけ先に置く。
enum class FaceExpression : std::uint8_t
{
    Neutral,
    Happy,
    Sleepy,
};

class Avatar
{
public:
    // 顔に使うスプライトの枠。
    //
    // 320x240 の RGB565 で 1 枚ぶん = 150KB。
    //
    // Why 全画面ぶん取るか: 顔は画面いっぱいに描く。部分更新にすると
    // まばたきのたびに背景との合成が要り、ちらつきを避けるには結局
    // 画面ぶんのバッファが要る。フレームバッファ (frame_channel) と
    // 同じ大きさなので、見積もりも揃う。
    //
    // Why not フレームバッファを使い回さないか: 使い回すと、顔モードの
    // 間 X68000 の変換先が無くなる。KeepRunning を既定にした以上
    // (app_mode.h の EmulationPolicy を見よ)、顔を出している間も
    // エミュレーションは画面を作り続ける。作れないと、戻ったときに
    // 全画面を作り直すまで古い画面が残る。
    static constexpr std::size_t kSpriteBytes = 320 * 240 * 2;

    // 顔に使うバッファを受け取る。nullptr でもよい。
    //
    // 今の仮実装は使わない (M5.Display へ直接描く)。本物にするときの
    // 受け口として先に置いてある。main.cpp が起動直後に予約する。
    void setSpriteBuffer(std::uint16_t* buffer);

    [[nodiscard]] bool hasSpriteBuffer() const
    {
        return sprite_ != nullptr;
    }

    void setExpression(FaceExpression expression);

    [[nodiscard]] FaceExpression expression() const
    {
        return expression_;
    }

    // 顔を描く。表示コアから呼ぶ。
    //
    // 【仮】目と口の図形を置くだけ。M5.Display を直接叩く。
    void draw();

private:
    std::uint16_t* sprite_ = nullptr;
    FaceExpression expression_ = FaceExpression::Neutral;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_AVATAR_H
