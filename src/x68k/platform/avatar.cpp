// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 【仮の実装】placeholder の顔を描く。本物の Avatar ではない。
// 何が入っていないかは avatar.h の冒頭を見よ。

#include "avatar.h"

#include <M5Unified.h>

namespace x68k_platform
{
namespace
{

// 目と口の配置。320x240 の中央に置く。
//
// 数字に根拠は無い。placeholder なので「顔に見える」ことだけが条件。
// 本物へ差し替えるときは丸ごと消える。
constexpr int kEyeY = 90;
constexpr int kEyeLeftX = 110;
constexpr int kEyeRightX = 210;
constexpr int kEyeRadius = 18;
constexpr int kMouthY = 160;
constexpr int kMouthWidth = 80;
constexpr int kMouthHeight = 10;

}  // namespace

void Avatar::setSpriteBuffer(std::uint16_t* buffer)
{
    sprite_ = buffer;
}

void Avatar::setExpression(FaceExpression expression)
{
    expression_ = expression;
}

void Avatar::draw()
{
    // 全面を塗ってから目と口を置く。
    //
    // Why not 差分だけ描かないか: 直前まで X68000 の画面が出ている。
    // 320x240 の全面が別の内容なので、消さずに顔を描くと文字の上に
    // 目が浮く。placeholder の段階で部分更新を考える意味は無い。
    M5.Display.fillScreen(TFT_BLACK);

    M5.Display.fillCircle(kEyeLeftX, kEyeY, kEyeRadius, TFT_WHITE);
    M5.Display.fillCircle(kEyeRightX, kEyeY, kEyeRadius, TFT_WHITE);
    M5.Display.fillRect(160 - kMouthWidth / 2, kMouthY, kMouthWidth, kMouthHeight, TFT_WHITE);

    // 仮であることを画面にも出す。
    //
    // Why: これを見た人が「顔が実装された」と誤解しないため。
    // 本物へ差し替えるときに消す。
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 224);
    M5.Display.print("FACE (placeholder)");
}

}  // namespace x68k_platform
