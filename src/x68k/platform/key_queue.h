// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 打たれたキーをエミュレーションコアへ渡す。
//
// なぜ要るか (2 つある):
//
// 1. データ競合。Machine::pressKey() は MFP のキー受信レジスタを書く。
//    エミュレーションコアは同じレジスタを割り込み処理で読むので、
//    表示コアから直接呼ぶと競合する。
//
// 2. 取りこぼし。MFP の受信レジスタは 1 バイトしか保持しない。押下と
//    解放を続けざまに書くと、CPU が読み出す前に上書きされて入力が
//    丸ごと消える。実効 3〜4MHz では 1 バイト読むのに数フレームかかる。
//
// 打たれた文字を溜めておき、エミュレーションコアが自分のペースで
// 1 イベントずつ MFP へ渡す。押下と解放の間隔もそこで空ける。
//
// Why not FreeRTOS の queue をそのまま使うか: 使っている。この型は
// 「ASCII を積むと押下と解放に分けて送られる」という約束を持たせる薄い
// 包みで、間隔の調整もここに閉じ込めてある。呼ぶ側が押下・解放の対を
// 意識せずに済む。

#ifndef X68K_PLATFORM_KEY_QUEUE_H
#define X68K_PLATFORM_KEY_QUEUE_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "machine.h"

namespace x68k_platform
{

class KeyQueue
{
public:
    // 押下と解放の間に空けるスライス数。
    //
    // drain() は表示の周期ではなくエミュレーションのスライスごとに
    // 呼ばれる。この値を入れた後 4 回は待って返り、5 回目に次のイベントを
    // 送るので、実際の間隔は 5 スライス。実効 3.2MHz なら約 31ms。
    //
    // Why not ゲストが読んだことを確認しないか: MFP の UDR は
    // 「読まれたか」を保持しない。確認するには Machine::pressKey が
    // 受理の可否を返し、断られたイベントをキューの先頭へ戻す仕組みが要る。
    // 実機で入力が通ることは確かめてあるので、まずは固定間隔で足りている。
    // 割り込みを長く止めるプログラムを動かすようになったら作り直す。
    static constexpr int kStepsPerEvent = 4;

    bool begin();

    // --- どのコアからでも呼べる ---

    // 打たれた文字を積む。溢れたら捨てる (押しっぱなしで詰まるより、
    // 取りこぼす方が扱いやすい)。
    void push(char c);

    // --- エミュレーションコアから呼ぶ ---

    // 溜まったキーを 1 イベントずつ MFP へ流す。スライスごとに呼ぶ。
    void drain(x68k::Machine& machine);

private:
    QueueHandle_t queue_ = nullptr;

    // 押下を送った後、解放を送るまでの待ち。
    int stepsLeft_ = 0;
    x68k::u8 pendingRelease_ = 0;
};

// マウスの動きをエミュレーションコアへ渡す。
//
// KeyQueue と分けてあるのは、届け先も溢れたときの扱いも違うため。
// キーは MFP のシリアルへ 1 バイトずつ流れ、取りこぼすと打った文字が消える。
// マウスは SCC の受信 FIFO へ 3 バイトのレポートとして積まれ、移動量は
// 相対値なので「溜まったぶんを足し合わせる」ことができる。
//
// Why not KeyQueue と同じく 1 イベントずつ間隔を空けて送らないか:
// SCC の受信 FIFO は 8 段あり (scc.h の kRxFifoSize)、1 レポート 3 バイトを
// 2 つぶん保持できる。間隔を空けるより、溜まった移動量を 1 レポートに
// まとめて送る方がカーソルの追従が良い。指を速く滑らせたときに
// 移動量が目減りしない。
//
// Why not 表示コアから Machine::moveMouse を直接呼ばないか: KeyQueue と
// 同じ理由。SCC の受信 FIFO と割り込み保留はエミュレーションコアが
// 割り込み処理で触るので、表示コアから書くとデータ競合になる。
class MouseQueue
{
public:
    bool begin();

    // --- どのコアからでも呼べる ---

    // 移動量とボタンの状態を積む。dx/dy は前回からの相対量。
    //
    // 溜まっているぶんへ足し込む。押しっぱなしで指を滑らせると
    // 毎ループ呼ばれるので、1 件ずつキューに積むと溢れる。
    void push(int dx, int dy, bool leftButton, bool rightButton);

    // --- エミュレーションコアから呼ぶ ---

    // 溜まった動きを SCC へ流す。スライスごとに呼ぶ。
    //
    // 動きもボタンの変化も無ければ何もしない。X68000 の IOCS は
    // 受け取ったレポートをそのままカーソル位置へ足すので、中身の無い
    // レポートを送っても実害は無いが、割り込みを無駄に上げることになる。
    //
    // SCC がレポートを断ったら (受信 FIFO に 3 バイトの空きが無い)、
    // 送ろうとした移動量とボタンの状態を差し戻して次回に送り直す。
    void drain(x68k::Machine& machine);

private:
    // 溜まった移動量とボタンの状態を守る。
    //
    // Why not FreeRTOS の queue を使わないか: KeyQueue と違って
    // 「溜まったぶんを足し合わせる」ため、積む側が既存の値を読んで
    // 書き戻す必要がある。queue では中身を覗いて書き換えられない。
    SemaphoreHandle_t mutex_ = nullptr;

    int pendingDx_ = 0;
    int pendingDy_ = 0;
    bool leftButton_ = false;
    bool rightButton_ = false;

    // 前回 SCC が「受理した」レポートのボタンの状態。変化の検出に使う。
    //
    // Why not 送ろうとした時点で更新しないか: SCC は FIFO に空きが無いと
    // レポートを丸ごと捨てる。送る前に更新すると、捨てられたボタンの変化が
    // 「送信済み」として記録され、以後は変化なしと判断されて二度と送られない。
    // 解放が消えるとゲストはボタンを押しっぱなしと見なす。
    bool sentLeftButton_ = false;
    bool sentRightButton_ = false;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_KEY_QUEUE_H
