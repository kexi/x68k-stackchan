// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 顔 ⇄ X68000 の切り替え FSM。
//
// このデバイスは「スタックチャン」と「X68000 エミュレータ」の二役を持つ。
// どちらを今やっているのかを決めるのがこの型で、決定はここ一箇所にしかない。
//
// なぜ FSM を独立させるか (3 つある):
//
// 1. 所有者を 1 つにする。「今どのモードか」を表示コアと入力処理と
//    エミュレーションコアがそれぞれ持つと、切り替えの途中で食い違う。
//    顔を描きながらタッチは X68000 のマウスへ流れる、という状態になる。
//
// 2. 遷移を検査可能にする。「顔から X68K へ戻るときマウスのボタンを
//    離す」のような、実機でしか踏まない副作用の判断がここに集まる。
//    ESP-IDF に触らない純粋なロジックにしておけば、ホストの doctest で
//    全遷移を回せる。M5Unified や Avatar の描画は platform/ の別ファイル。
//
// 3. 「顔の間エミュレーションをどうするか」を明示する。止めるか回すかは
//    Human68k の時計と CPU 予算のトレードオフで、自明な正解が無い
//    (EmulationPolicy のコメントを見よ)。暗黙に埋め込むと、後から
//    「なぜ時計がずれるのか」を追えなくなる。
//
// Why not core/ に置かないか: core/ は X68000 のエミュレーション本体で、
// 「顔モード」という概念を持たない。スタックチャン側の都合を core/ に
// 持ち込むと、ホストのエミュレータランナー (host/main.cpp) にも
// 無関係な状態が生える。platform/ の中で ESP-IDF に依存しないファイル、
// という置き方にしてある (sram_persist.h と同じ扱い)。

#ifndef X68K_PLATFORM_APP_MODE_H
#define X68K_PLATFORM_APP_MODE_H

#include <cstdint>

namespace x68k_platform
{

// デバイスが今やっていること。
//
// Why not 「起動中」を入れないか: 起動 (メモリ確保・SD マウント・ROM 読み)
// が終わるまで FSM は作られない。app_main は失敗したらそこで return して
// しまうので、FSM から見た「起動中」は表現する相手がいない。
enum class AppMode : std::uint8_t
{
    // スタックチャンの顔。X68000 の画面は出さない。
    Face,
    // X68000 の画面。顔は出さない。
    X68k,
};

// 顔モードの間エミュレーションをどう扱うか。
//
// 【この選択が要る理由】
// 顔を出している間 X68000 を回し続けるか止めるかで、壊れるものが違う。
//
//   回し続ける: 実効 3.3〜3.8MHz を出すのに Core1 を占有している
//               (docs/knowledge/cores3-emulator-runtime.md の実測)。
//               顔の描画とサーボはその上に乗るので、顔の動きが渋る。
//   止める:     Human68k の時計がずれる。X68000 の時刻は RTC (RP5C15) を
//               起動時に読んで、以降は MFP の Timer-C 割り込みで
//               ソフトウェアが刻む。CPU を止めると Timer-C も止まるので、
//               顔を出していた時間ぶんそのまま遅れる。
//
// 【選んだもの: KeepRunning を既定にする】
//
// Why not 止める方を既定にしないか: ずれるのは「時計」だけではない。
// X68000 は SASI の転送も FDC のシークも CPU が回してはじめて進む。
// ディスクを読んでいる最中に顔へ切り替えると、DMAC の転送が途中で
// 凍る。戻ってきたときゲストから見えるのは「異常に遅いディスク」で、
// タイムアウトを持つプログラムはエラーにする。時計のずれは `date` で
// 直せるが、転送の途中で止まったディスクは直せない。
//
// Why not 「顔の間だけスライスを細くする」を既定にしないか: それが
// Throttled で、選べるようにはしてある。ただし既定にはしない。
// 顔が滑らかに動くかどうかは実機に Avatar を載せて測るまで分からず、
// 測る前に中間の値を既定に据えると「なぜこの数字なのか」が誰にも
// 説明できなくなる。まず KeepRunning で回し、顔が渋ることが実測で
// 分かってから Throttled に落とす。
//
// Why not Paused を消さないか: 消せない。エミュレーションが halt した
// 後 (未実装命令) は回すものが無く、顔だけを出す状態が要る。
// 「止まっているのはポリシーの選択ではなく事故」という区別を残すため、
// ポリシーとしての Paused も表現できるようにしてある。
enum class EmulationPolicy : std::uint8_t
{
    // 顔モードの間も全速で回す。既定。
    KeepRunning,
    // 顔モードの間はスライスを細くして回す。顔に CPU を譲る。
    Throttled,
    // 顔モードの間は止める。Human68k の時計がずれる。
    Paused,
};

// 切り替えの要求。
//
// Why not AppMode を直接渡さないか: 「顔へ切り替えろ」と「今と逆へ
// 切り替えろ」は別の要求で、後者を呼ぶ側が今のモードを読んで判断すると
// 読んでから渡すまでの間に別のコアが切り替えられる。要求の形にして
// FSM に解釈させれば、判断が所有者の中で閉じる。
enum class ModeRequest : std::uint8_t
{
    ToFace,
    ToX68k,
    Toggle,
};

// 遷移が起きたときにやることの指示。
//
// Why not AppModeMachine が M5Unified や Machine を直接叩かないか:
// 叩くと FSM が ESP-IDF に依存し、ホストのテストから外れる。
// 「何をすべきか」だけを値で返し、実行は呼ぶ側 (main.cpp) に任せる。
// テストは返ってきた値を見れば、実機に焼かずに判断を検査できる。
struct ModeTransition
{
    // 実際にモードが変わったか。同じモードへの要求は false。
    bool changed = false;

    // 遷移前後のモード。changed が false なら両方とも同じ値。
    AppMode from = AppMode::Face;
    AppMode to = AppMode::Face;

    // X68000 へ「マウスのボタンを離した」と伝えるべきか。
    //
    // 顔モードへ入るときに立つ。顔モードの間タッチは X68000 へ届かない
    // ので、ボタンを押したまま切り替えるとゲストは押しっぱなしと
    // 見なし続ける。SX-Window ではドラッグが終わらず、ウィンドウが
    // 指に貼り付いたままになる (input_touch.cpp が指を離したときに
    // 同じことをしている)。
    bool shouldReleaseMouseButtons = false;

    // 画面を作り直すべきか。
    //
    // どちらの向きでも立つ。表示は 320x240 の全面を 1 枚で使っており、
    // 顔と X68000 の画面は同じ場所を奪い合う。切り替えた直後は
    // 相手が描いた内容がそのまま残っているので、ダーティ行だけを
    // 送る通常の経路では消えない (DisplayLcd::invalidateAll が要る)。
    bool shouldRedraw = false;
};

// 顔 ⇄ X68000 の切り替えを所有する。
//
// スレッド安全ではない。呼ぶのは表示コア (Core0) だけにする。
//
// Why not アトミックにしないか: 状態が 2 つ (mode_ と policy_) あり、
// 遷移はその両方と ModeTransition の組み立てにまたがる。アトミック
// 変数を並べても組として一貫しないので、「触るのは 1 コアだけ」を
// 約束にする方が確実で安い。エミュレーションコアが読む必要がある値
// (エミュレーションを回してよいか) は、main.cpp が atomic<bool> 1 つに
// 落として渡す。
class AppModeMachine
{
public:
    AppModeMachine() = default;

    explicit AppModeMachine(AppMode initial, EmulationPolicy policy = EmulationPolicy::KeepRunning)
        : mode_(initial), policy_(policy)
    {
    }

    [[nodiscard]] AppMode mode() const
    {
        return mode_;
    }

    [[nodiscard]] EmulationPolicy policy() const
    {
        return policy_;
    }

    // ポリシーを差し替える。
    //
    // 実行中に変えられるようにしてあるのは、実機で顔の滑らかさを見ながら
    // KeepRunning と Throttled を比べたいため (シリアルコンソールから
    // 切り替える想定)。焼き直して測り直すのでは比較にならない。
    void setPolicy(EmulationPolicy policy)
    {
        policy_ = policy;
    }

    // 要求を処理して、呼ぶ側がやるべきことを返す。
    //
    // 同じモードへの要求は「何もしない」を返す。弾かずに受け付けるのは、
    // Toggle 以外の要求が「今どこにいるかを知らない側」から来るため。
    // 例えばシリアルの 'f' は今が顔でも押せる。エラーにしても呼ぶ側が
    // やることは同じ (無視) なので、changed=false で表現する。
    ModeTransition request(ModeRequest request);

    // タッチを X68000 (マウス・キーボード) へ流してよいか。
    //
    // 顔モードでは false。顔を触ったつもりの指が X68000 のカーソルを
    // 動かしたり、見えない仮想キーボードを叩いたりするのを防ぐ。
    [[nodiscard]] bool isX68kInputEnabled() const
    {
        return mode_ == AppMode::X68k;
    }

    // エミュレーションを進めてよいか。
    //
    // X68K モードでは常に true。顔モードではポリシー次第。
    [[nodiscard]] bool shouldRunEmulation() const;

    // 1 スライスで進めるサイクル数。base は X68K モードでの値。
    //
    // Throttled のときだけ顔モードで減らす。KeepRunning は base のまま、
    // Paused は 0 (呼ぶ側は shouldRunEmulation() で先に弾く想定だが、
    // 取り違えても暴走しないよう 0 を返す)。
    [[nodiscard]] std::uint32_t sliceCycles(std::uint32_t base) const;

    // Throttled のときに base を割る数。
    //
    // 4 にしたのは、実効 3.3MHz を 800kHz 程度まで落とせば顔に十分な
    // CPU が渡るという見積もりから。Human68k のアイドル (キー入力待ちの
    // ループ) はこの速度でも問題なく回る。
    //
    // Why not 顔モードのサイクル数を直接持たないか: X68K モードの値は
    // main.cpp が持っており (kSliceCycles)、そちらを変えたときに
    // 顔モード側だけ古い値が残ると、比が意図と変わる。割合で持てばずれない。
    static constexpr std::uint32_t kThrottleDivisor = 4;

private:
    // 既定を X68K にする。
    //
    // Why not 顔から始めないか: 起動直後に見たいのは Human68k が
    // 立ち上がったかどうか。顔から始めると、ROM が読めたのか SASI が
    // 見つかったのかを確かめるのにまず切り替えが要る。実機は画面を
    // 直接見るしかないので、最初に出すのは診断に使える方にする。
    AppMode mode_ = AppMode::X68k;
    EmulationPolicy policy_ = EmulationPolicy::KeepRunning;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_APP_MODE_H
