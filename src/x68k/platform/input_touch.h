// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// タッチパネルの仮想キーボードとマウス。
//
// 画面を上下に分けて使う。下部 (kKeyboardTop 以降) が仮想キーボード、
// 上部が X68000 の画面で、そこをなぞるとマウスが動く。SX-Window は
// マウスが無いと何も操作できないので、両方を 1 枚のパネルに載せる。
//
// CoreS3 には物理キーボードが無いので、画面下部にキーを並べて打てるようにする。
// Human68k のプロンプトで `dir` と打てれば PoC の目的は達せられるので、
// 英数字と Enter / Space、記号は `.` と `/` だけを並べてある。
// BS は無い (4 行 x 10 列に収める都合。input_touch.cpp の kLayout を見よ)。
//
// スキャンコードについて:
//   X68000 のキーボードは MFP のシリアルへスキャンコードを送る。押下で
//   コードそのもの、離すとコード | $80 が届く。ASCII ではないので変換表が要る。

#ifndef X68K_PLATFORM_INPUT_TOUCH_H
#define X68K_PLATFORM_INPUT_TOUCH_H

#include <cstdint>

#include "key_queue.h"
#include "machine.h"

namespace x68k_platform
{

class TouchKeyboard
{
public:
    // キーボードの表示領域。画面下部に置く。
    static constexpr int kKeyboardTop = 150;
    static constexpr int kKeyRows = 4;
    static constexpr int kKeyCols = 10;

    void begin();

    // タッチを読んで、押されたキーがあれば keys へ、画面側のドラッグは
    // mouse へ積む。毎ループ呼ぶ。
    //
    // Why not Machine へ直接送るか: Machine の状態はエミュレーション
    // コアが所有する。表示コアから触るとデータ競合になるうえ、MFP の
    // 受信レジスタは 1 バイトしか保持しないので、押下と解放を続けて
    // 書くと入力が消える。queue に積んでおけば、エミュレーションコアが
    // 自分のペースで間隔を空けて送れる。マウスも SCC の受信 FIFO を
    // 割り込み処理と共有するので、同じ理由で queue を挟む。
    void poll(KeyQueue& keys, MouseQueue& mouse);

    // キーボードを描画する。表示が上書きされた後に呼び直す。
    void draw();

    // キーボードを表示するか。false のときは poll してもキーを送らない。
    void setVisible(bool visible);

    [[nodiscard]] bool isVisible() const
    {
        return visible_;
    }

    // タッチを X68000 へ流すか。顔モードの間は false にする。
    //
    // Why not visible_ で兼ねるか: 別の関心事。visible_ は「仮想
    // キーボードを描いているか」で、false でも画面全体がマウス領域として
    // 生きる (main.cpp が今そうしている)。顔モードで止めたいのはマウスも
    // 含めた X68000 への経路すべてなので、兼ねると顔を触った指が
    // カーソルを動かす。
    //
    // Why not poll() の呼び出しを main.cpp 側で止めないか: 止めると
    // 「顔モードの間に指を離した」ことが分からなくなる。押したまま顔へ
    // 切り替えて顔モード中に離すと、isDragging_ が true のまま残り、
    // X68K へ戻って触った瞬間に前回の座標との差が巨大な移動量として
    // 流れる。poll は回し続け、送る直前で捨てる方が状態が壊れない。
    void setX68kInputEnabled(bool enabled);

    [[nodiscard]] bool isX68kInputEnabled() const
    {
        return isX68kInputEnabled_;
    }

private:
    // 押しっぱなしで連打にならないよう、離すまで次を送らない。
    int lastKeyIndex_ = -1;
    bool visible_ = true;

    // 既定は true。X68K モードから始まるので (app_mode.h の AppModeMachine)、
    // 初期値もそちらに揃える。
    bool isX68kInputEnabled_ = true;

    // --- マウス ---
    //
    // 画面領域のドラッグをマウスの相対移動へ変換する。
    //
    // Why 絶対座標ではなく相対量にするか: X68000 のマウスは相対量しか
    // 報告しない (SCC のレポートは dx/dy の 1 バイト符号付き。scc.h を見よ)。
    // カーソルの絶対位置を持っているのは IOCS 側のワークで、こちらからは
    // 見えないし書けない。仮にタッチ座標を絶対位置とみなして「そこへ
    // 移動させる差分」を送ろうとすると、IOCS が今どこにカーソルを置いて
    // いるかを知る必要があり、ワークの番地を決め打ちすることになる。
    // その番地は OS のバージョンに依存する (followCursor が同じ理由で
    // VRAM 走査を選んでいる)。
    //
    // 加えて、LCD は 320x240 で X68000 の画面は 768x512。タッチ座標を
    // そのまま絶対位置に使うと、指の届く範囲が画面の一部に限られる。
    // 相対量なら何度でも擦ってカーソルを運べる。
    //
    // Why not 前フレームとの差分をそのまま送らないか: 送っている。
    // ただし「触り始めた瞬間」だけは差分を送らない。前回離した位置と
    // 触り直した位置の差が巨大な移動量になり、カーソルが飛ぶ。
    bool isDragging_ = false;
    int lastTouchX_ = 0;
    int lastTouchY_ = 0;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_INPUT_TOUCH_H
