// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// CoreS3 の LCD へテキスト画面を出す。
//
// Human68k の標準コンソールは 768x512 だが CoreS3 の LCD は 320x240 しかない。
// そのまま縮小すると 8x16 のフォントが 3.3x7.5 ドットになって読めないので、
// 等倍で一部を切り出し、カーソルを追うようにスクロールさせる。
//
// 役割を 2 つに分けてある:
//   renderTo()  — VRAM を RGB565 へ変換する。Machine を読むので
//                 エミュレーションコア (Core1) から呼ぶ。
//   pushFrame() — できあがった RGB565 を LCD へ送る。Machine を触らないので
//                 表示コア (Core0) から呼べる。
//
// テキストだけを出す経路とグラフィックを重ねる経路を持つ。どちらを使うかは
// $E82600 の表示許可を見て毎フレーム決める (setGraphicVram を参照)。
//
// なぜ分けるか: Machine の状態を両コアから触るとデータ競合になる。
// 変換を Core1 に寄せ、Core0 へは完成したフレームだけを渡す。

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

    // LCD を初期化する。表示コアから 1 度だけ呼ぶ。
    void begin();

    // 表示位置を設定する。768x512 の中のどこを映すか。
    void setViewport(x68k::u32 x, x68k::u32 y);

    // 拡大率の範囲。
    //
    // 呼ぶ側が「これ以上は変わらない」を判断できるよう公開する。
    // setZoom 側だけがクランプを持つと、呼ぶ側は上限を超えた値を
    // 持ち続けてしまい、戻すときに打った回数どおりに動かない。
    static constexpr x68k::u32 kMinZoom = 1;
    static constexpr x68k::u32 kMaxZoom = 4;

    // 拡大率。1 = 等倍 (40 桁 x 15 行)、2 = 2 倍 (20 桁 x 7 行)。
    //
    // 等倍は 1 文字が 8x16 ドットのまま 2 インチの画面に出るので、
    // 桁数は稼げるが実際には読み取れない。2 倍にすると 16x32 になり、
    // 見える範囲は狭まるが文字として判別できる。
    void setZoom(x68k::u32 zoom);

    [[nodiscard]] x68k::u32 zoom() const
    {
        return zoom_;
    }

    [[nodiscard]] x68k::u32 viewportX() const
    {
        return viewX_;
    }
    [[nodiscard]] x68k::u32 viewportY() const
    {
        return viewY_;
    }

    // グラフィック VRAM を与える。nullptr ならテキストだけを出す。
    //
    // Why not renderTo の引数で毎回渡さないか: グラフィック面を出すかどうかは
    // ゲストが $E82600 を書き換えるたびに変わる。引数にすると呼ぶ側が
    // 「今は出す/出さない」を判断することになり、表示の決定が main.cpp へ
    // 漏れる。確保できた領域を一度預けておけば、判断はこのクラスに閉じる。
    void setGraphicVram(const x68k::u8* graphicVram)
    {
        graphicVram_ = graphicVram;
    }

    // --- エミュレーションコア (Core1) から呼ぶ ---

    // VRAM を RGB565 へ変換して out へ書く。
    //
    // グラフィック面が表示許可されていればテキストと合成し、そうでなければ
    // テキストだけを描く。
    //
    // 変化が無く、全体の描き直しも要らなければ false を返して何もしない。
    // その場合は転送も要らない。
    bool renderTo(x68k::Machine& machine, const x68k::u8* textVram, x68k::u16* out);

    // 次のフレームで全体を描き直させる。表示位置を変えた後などに呼ぶ。
    void invalidateAll()
    {
        forceFullRedraw_ = true;
    }

    // --- 表示コア (Core0) から呼ぶ ---

    // できあがった RGB565 を LCD へ送る。
    void pushFrame(const x68k::u16* frame);

    // 起動時のメッセージを出す。ROM が見つからないときなどに使う。
    static void showMessage(const char* line1, const char* line2 = nullptr);

private:
    // 1 枚ぶんの変換。合成するかどうかはここで決める。
    //
    // 拡大表示はこれを左上の縮小サイズで呼んでから引き伸ばすので、
    // テキスト単独と合成の分岐を 1 か所に持てるよう切り出してある。
    void renderPlanes(x68k::Machine& machine, const x68k::u8* textVram, x68k::u32 srcWidth,
                      x68k::u32 srcHeight, x68k::u16* out);

    // 拡大表示。全画面を作り直す。
    void renderZoomed(x68k::Machine& machine, const x68k::u8* textVram, x68k::u16* out);

    // グラフィック面を合成すべきか。$E82600 の表示許可と実体の有無で決まる。
    [[nodiscard]] bool shouldComposite(x68k::Machine& machine) const;

    const x68k::u8* graphicVram_ = nullptr;
    x68k::u32 viewX_ = 0;
    x68k::u32 viewY_ = 0;
    x68k::u32 zoom_ = 1;
    bool forceFullRedraw_ = true;

    // 前フレームで合成したか。グラフィック面の表示許可が切り替わった
    // フレームを検出して描き直させる。
    //
    // Why 要るか: SystemBus のダーティ追跡はテキスト VRAM にしか印を付けない
    // (bus.cpp の markTextDirty はテキスト領域からしか呼ばれない)。
    // $E82600 を書いてグラフィック面を出しただけではテキストが汚れないので、
    // ダーティだけを見ていると画面が切り替わらない。
    bool wasComposited_ = false;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_DISPLAY_LCD_H
