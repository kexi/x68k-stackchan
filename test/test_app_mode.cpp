// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: 顔 ⇄ X68000 の切り替え FSM が
//
//   1. 全ての遷移で「今どのモードか」を 1 つに決めること
//   2. 顔モードの間は X68000 への入力経路を閉じること
//   3. 顔モードのエミュレーション方針 (回す/絞る/止める) が宣言どおりに
//      振る舞うこと
//   4. 意味の無い遷移 (同じモードへの要求) を「変化なし」として扱い、
//      副作用を出さないこと
//
// なぜホストで検査するか: この判断は実機でしか踏まない副作用
// (マウスのボタンが押しっぱなしになる、切り替え直後に前のモードの画面が
// 残る) を防ぐためにあり、実機に焼いて目で見る形では取りこぼす。
// FSM を ESP-IDF 非依存にしてあるので (app_mode.h の冒頭を見よ)、
// ここで全遷移を機械的に回せる。

#include <cstdint>
#include <initializer_list>

#include "app_mode.h"
#include "doctest.h"
#include "servo.h"

namespace
{

using x68k_platform::AppMode;
using x68k_platform::AppModeMachine;
using x68k_platform::EmulationPolicy;
using x68k_platform::ModeRequest;
using x68k_platform::ModeTransition;

// main.cpp が使うスライス幅。ここを変えても比が保たれることを確かめる。
constexpr std::uint32_t kSliceCycles = 20000;

}  // namespace

TEST_SUITE("app-mode")
{
    // --- 初期状態 ---------------------------------------------------------

    TEST_CASE("既定は X68K モードで、入力もエミュレーションも通る")
    {
        // 起動直後に見たいのは Human68k が立ち上がったかどうか。
        // 顔から始めると ROM が読めたかを確かめるのに切り替えが要る。
        const AppModeMachine machine;
        CHECK(machine.mode() == AppMode::X68k);
        CHECK(machine.policy() == EmulationPolicy::KeepRunning);
        CHECK(machine.isX68kInputEnabled());
        CHECK(machine.shouldRunEmulation());
    }

    TEST_CASE("初期モードとポリシーを指定して作れる")
    {
        const AppModeMachine machine(AppMode::Face, EmulationPolicy::Paused);
        CHECK(machine.mode() == AppMode::Face);
        CHECK(machine.policy() == EmulationPolicy::Paused);
    }

    // --- 遷移 -------------------------------------------------------------

    TEST_CASE("X68K から顔へ切り替わる")
    {
        AppModeMachine machine(AppMode::X68k);

        const ModeTransition transition = machine.request(ModeRequest::ToFace);

        CHECK(transition.changed);
        CHECK(transition.from == AppMode::X68k);
        CHECK(transition.to == AppMode::Face);
        CHECK(machine.mode() == AppMode::Face);
    }

    TEST_CASE("顔から X68K へ切り替わる")
    {
        AppModeMachine machine(AppMode::Face);

        const ModeTransition transition = machine.request(ModeRequest::ToX68k);

        CHECK(transition.changed);
        CHECK(transition.from == AppMode::Face);
        CHECK(transition.to == AppMode::X68k);
        CHECK(machine.mode() == AppMode::X68k);
    }

    TEST_CASE("Toggle は今と逆のモードへ行く")
    {
        AppModeMachine machine(AppMode::X68k);

        CHECK(machine.request(ModeRequest::Toggle).to == AppMode::Face);
        CHECK(machine.mode() == AppMode::Face);

        CHECK(machine.request(ModeRequest::Toggle).to == AppMode::X68k);
        CHECK(machine.mode() == AppMode::X68k);
    }

    TEST_CASE("Toggle を繰り返しても 2 つのモードの外へ出ない")
    {
        // 状態が増えていないことの確認。往復するだけで、
        // 3 つ目の状態や「どちらでもない」状態は生まれない。
        AppModeMachine machine(AppMode::X68k);
        for (int i = 0; i < 10; ++i)
        {
            const AppMode expected = (i % 2 == 0) ? AppMode::Face : AppMode::X68k;
            CHECK(machine.request(ModeRequest::Toggle).to == expected);
            CHECK(machine.mode() == expected);
        }
    }

    // --- 無効な遷移 (同じモードへの要求) ----------------------------------

    TEST_CASE("同じモードへの要求は変化なしとして扱われ、副作用を出さない")
    {
        // シリアルの 'f' は今が顔でも押せる。エラーにしても呼ぶ側が
        // やることは同じ (無視) なので changed=false で表現する。
        // ここで shouldRedraw が立つと、連打するたびに 320x240 の
        // 全画面を作り直すことになる。
        SUBCASE("顔モードで顔を要求する")
        {
            AppModeMachine machine(AppMode::Face);

            const ModeTransition transition = machine.request(ModeRequest::ToFace);

            CHECK_FALSE(transition.changed);
            CHECK(transition.from == AppMode::Face);
            CHECK(transition.to == AppMode::Face);
            CHECK_FALSE(transition.shouldRedraw);
            CHECK_FALSE(transition.shouldReleaseMouseButtons);
            CHECK(machine.mode() == AppMode::Face);
        }

        SUBCASE("X68K モードで X68K を要求する")
        {
            AppModeMachine machine(AppMode::X68k);

            const ModeTransition transition = machine.request(ModeRequest::ToX68k);

            CHECK_FALSE(transition.changed);
            CHECK_FALSE(transition.shouldRedraw);
            CHECK_FALSE(transition.shouldReleaseMouseButtons);
            CHECK(machine.mode() == AppMode::X68k);
        }
    }

    // --- 遷移の副作用 -----------------------------------------------------

    TEST_CASE("顔へ入るときだけマウスのボタンを離す")
    {
        // 顔モードの間タッチは X68000 へ届かない。押したまま切り替えると
        // ゲストはボタンを押しっぱなしと見なし続け、SX-Window では
        // ウィンドウが指に貼り付いたままになる。
        SUBCASE("X68K -> 顔 で離す")
        {
            AppModeMachine machine(AppMode::X68k);
            CHECK(machine.request(ModeRequest::ToFace).shouldReleaseMouseButtons);
        }

        // 戻る時点でゲストのボタンは既に離れている。もう一度離すと
        // SCC へ変化の無いレポートを積んで割り込みを無駄に上げるだけ。
        SUBCASE("顔 -> X68K では離さない")
        {
            AppModeMachine machine(AppMode::Face);
            CHECK_FALSE(machine.request(ModeRequest::ToX68k).shouldReleaseMouseButtons);
        }
    }

    TEST_CASE("モードが変わったらどちらの向きでも描き直す")
    {
        // 顔と X68000 は同じ 320x240 を奪い合う。切り替えた直後は
        // 相手の描いた内容が残っているので、ダーティ行だけを送る
        // 通常の経路では消えない。
        SUBCASE("X68K -> 顔")
        {
            AppModeMachine machine(AppMode::X68k);
            CHECK(machine.request(ModeRequest::ToFace).shouldRedraw);
        }

        SUBCASE("顔 -> X68K")
        {
            AppModeMachine machine(AppMode::Face);
            CHECK(machine.request(ModeRequest::ToX68k).shouldRedraw);
        }
    }

    // --- 入力の遮断 -------------------------------------------------------

    TEST_CASE("X68000 への入力は X68K モードでだけ通る")
    {
        // 顔を触ったつもりの指が X68000 のカーソルを動かしたり、
        // 見えない仮想キーボードを叩いたりするのを防ぐ。
        AppModeMachine machine(AppMode::X68k);
        CHECK(machine.isX68kInputEnabled());

        machine.request(ModeRequest::ToFace);
        CHECK_FALSE(machine.isX68kInputEnabled());

        machine.request(ModeRequest::ToX68k);
        CHECK(machine.isX68kInputEnabled());
    }

    TEST_CASE("入力の可否はポリシーに影響されない")
    {
        // エミュレーションを回すかどうかと、タッチを届けるかどうかは
        // 別の関心事。顔モードで裏で回していても、指は顔を触っている。
        for (const EmulationPolicy policy :
             {EmulationPolicy::KeepRunning, EmulationPolicy::Throttled, EmulationPolicy::Paused})
        {
            AppModeMachine machine(AppMode::Face, policy);
            CHECK_FALSE(machine.isX68kInputEnabled());

            machine.request(ModeRequest::ToX68k);
            CHECK(machine.isX68kInputEnabled());
        }
    }

    // --- エミュレーションの方針 -------------------------------------------

    TEST_CASE("X68K モードではポリシーによらず全速で回す")
    {
        // ポリシーが効くのは顔モードの間だけ。X68000 を見ている間に
        // 絞ったり止めたりする理由は無い。
        for (const EmulationPolicy policy :
             {EmulationPolicy::KeepRunning, EmulationPolicy::Throttled, EmulationPolicy::Paused})
        {
            const AppModeMachine machine(AppMode::X68k, policy);
            CHECK(machine.shouldRunEmulation());
            CHECK(machine.sliceCycles(kSliceCycles) == kSliceCycles);
        }
    }

    TEST_CASE("KeepRunning: 顔モードでも全速で回す")
    {
        // 既定。止めると SASI の転送や FDC のシークが途中で凍り、
        // 戻ったときゲストからは「異常に遅いディスク」に見える。
        const AppModeMachine machine(AppMode::Face, EmulationPolicy::KeepRunning);
        CHECK(machine.shouldRunEmulation());
        CHECK(machine.sliceCycles(kSliceCycles) == kSliceCycles);
    }

    TEST_CASE("Throttled: 顔モードでは回すがスライスを細くする")
    {
        const AppModeMachine machine(AppMode::Face, EmulationPolicy::Throttled);
        CHECK(machine.shouldRunEmulation());
        CHECK(machine.sliceCycles(kSliceCycles) == kSliceCycles / AppModeMachine::kThrottleDivisor);
        // 「絞る」であって「止める」ではない。
        CHECK(machine.sliceCycles(kSliceCycles) > 0);
        CHECK(machine.sliceCycles(kSliceCycles) < kSliceCycles);
    }

    TEST_CASE("Throttled: base が小さくても 1 サイクルは進める")
    {
        // 0 を返すと Throttled のつもりが Paused と区別できなくなる。
        const AppModeMachine machine(AppMode::Face, EmulationPolicy::Throttled);
        CHECK(machine.sliceCycles(1) == 1);
        CHECK(machine.sliceCycles(AppModeMachine::kThrottleDivisor - 1) == 1);
        CHECK(machine.shouldRunEmulation());
    }

    TEST_CASE("Paused: 顔モードでは止める")
    {
        // Human68k の時計がずれる方の選択。既定ではない。
        const AppModeMachine machine(AppMode::Face, EmulationPolicy::Paused);
        CHECK_FALSE(machine.shouldRunEmulation());
        CHECK(machine.sliceCycles(kSliceCycles) == 0);
    }

    TEST_CASE("ポリシーは実行中に差し替えられ、その場で効く")
    {
        // 実機で顔の滑らかさを見ながら KeepRunning と Throttled を
        // 比べたい。焼き直して測り直すのでは比較にならない。
        AppModeMachine machine(AppMode::Face, EmulationPolicy::KeepRunning);
        CHECK(machine.sliceCycles(kSliceCycles) == kSliceCycles);

        machine.setPolicy(EmulationPolicy::Paused);
        CHECK(machine.policy() == EmulationPolicy::Paused);
        CHECK_FALSE(machine.shouldRunEmulation());

        machine.setPolicy(EmulationPolicy::Throttled);
        CHECK(machine.shouldRunEmulation());
        CHECK(machine.sliceCycles(kSliceCycles) == kSliceCycles / AppModeMachine::kThrottleDivisor);
    }

    TEST_CASE("ポリシーを変えてもモードは動かない")
    {
        // 方針の差し替えは切り替えではない。
        AppModeMachine machine(AppMode::Face, EmulationPolicy::KeepRunning);
        machine.setPolicy(EmulationPolicy::Paused);
        CHECK(machine.mode() == AppMode::Face);
        CHECK_FALSE(machine.isX68kInputEnabled());
    }

    TEST_CASE("顔で止めていても X68K へ戻れば必ず回る")
    {
        // Paused から抜ける経路が塞がっていないこと。ここが壊れると
        // 一度顔にした後エミュレータが二度と動かない。
        AppModeMachine machine(AppMode::X68k, EmulationPolicy::Paused);

        machine.request(ModeRequest::ToFace);
        CHECK_FALSE(machine.shouldRunEmulation());

        machine.request(ModeRequest::ToX68k);
        CHECK(machine.shouldRunEmulation());
        CHECK(machine.sliceCycles(kSliceCycles) == kSliceCycles);
    }
}

// --- サーボ ---------------------------------------------------------------
//
// 保証すること: サーボが繋がっていない個体でも、呼ぶ側が分岐せずに
// 首を振る指示を出せること。実装は何もしないが、落ちも黙りもしない。

TEST_SUITE("servo")
{
    TEST_CASE("NullServo は begin に成功するが「付いていない」と答える")
    {
        // begin() の失敗は異常を意味する。サーボが無いのは普通の状態
        // なので、そちらは isAttached() で表す。取り違えると、サーボを
        // 持っていない個体で起動が止まる。
        x68k_platform::NullServo servo;
        CHECK(servo.begin());
        CHECK_FALSE(servo.isAttached());
    }

    TEST_CASE("NullServo は指示を捨てるが、最後の指示を覚えている")
    {
        // サーボが無い個体でも「FSM が首をどこへ向けようとしたか」を
        // 検査できるようにしてある。
        x68k_platform::NullServo servo;
        REQUIRE(servo.begin());

        servo.setPose({30.0F, -10.0F});

        CHECK(servo.lastPose().pan == doctest::Approx(30.0F));
        CHECK(servo.lastPose().tilt == doctest::Approx(-10.0F));
        CHECK_FALSE(servo.isAttached());
    }

    TEST_CASE("HeadPose の既定は正面・水平")
    {
        const x68k_platform::HeadPose pose;
        CHECK(pose.pan == doctest::Approx(0.0F));
        CHECK(pose.tilt == doctest::Approx(0.0F));
    }

    TEST_CASE("detach しても落ちない")
    {
        x68k_platform::NullServo servo;
        REQUIRE(servo.begin());
        CHECK_FALSE(servo.isDetached());

        servo.detach();

        CHECK(servo.isDetached());
    }
}
