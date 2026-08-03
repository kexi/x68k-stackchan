// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// CoreS3 の LCD へテキスト画面を出す。
//
// Human68k の標準コンソールは 768x512 だが CoreS3 の LCD は 320x240 しかない。
// そのまま縮小すると 8x16 のフォントが 3.3x7.5 ドットになって読めないので、
// 等倍で一部を切り出し、カーソルを追うようにスクロールさせる。
//
// 転送量について:
//   SPI 接続の ILI9342C に全画面 (150KB) を毎フレーム送ると間に合わない。
//   テキスト VRAM への書き込みで立てたダーティ行を見て、変化した帯だけを送る。
//   プロンプトが点滅しているだけの状態なら転送は数 KB で済む。

#ifndef X68K_PLATFORM_DISPLAY_LCD_H
#define X68K_PLATFORM_DISPLAY_LCD_H

#include <cstdint>

#include "machine.h"

namespace x68k_platform
{

class DisplayLcd
{
public:
    // CoreS3 の LCD の大きさ。
    static constexpr x68k::u32 kScreenWidth = 320;
    static constexpr x68k::u32 kScreenHeight = 240;

    // 変換バッファを受け取る。実体の確保は呼び出し側の責務
    // (PSRAM の断片化を避けるため起動直後に一括確保したい)。
    // buffer は kScreenWidth * kScreenHeight 個の u16 が要る。
    void begin(x68k::u16* buffer);

    // 表示位置を設定する。768x512 の中のどこを映すか。
    void setViewport(x68k::u32 x, x68k::u32 y);

    [[nodiscard]] x68k::u32 viewportX() const
    {
        return viewX_;
    }
    [[nodiscard]] x68k::u32 viewportY() const
    {
        return viewY_;
    }

    // 変化した部分だけを LCD へ送る。
    // 送るものが無ければ何もしない。
    void present(x68k::Machine& machine, const x68k::u8* textVram);

    // 次のフレームで全体を送り直す。表示位置を変えた後などに呼ぶ。
    void invalidateAll()
    {
        forceFullRedraw_ = true;
    }

    // 起動時のメッセージを出す。ROM が見つからないときなどに使う。
    static void showMessage(const char* line1, const char* line2 = nullptr);

private:
    x68k::u16* buffer_ = nullptr;
    x68k::u32 viewX_ = 0;
    x68k::u32 viewY_ = 0;
    bool forceFullRedraw_ = true;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_DISPLAY_LCD_H
