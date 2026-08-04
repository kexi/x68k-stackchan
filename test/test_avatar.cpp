// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: スタックチャンの顔が
//
//   1. まばたきを実時間で刻むこと (コマが飛んでも間隔が保たれる)
//   2. まばたきの間隔が一定でないこと (メトロノームに見えない)
//   3. まぶたが閉じる途中で目が実際に潰れること (状態だけでなく画素で)
//   4. 表情ごとに画素として違う絵が出ること (FaceExpression が飾りでない)
//   5. バッファが無くても落ちないこと
//   6. サーボの角度がパルス幅とデューティへ正しく写ること
//
// なぜホストで検査するか: 実機の LCD は直接見られない。顔が正しく
// 描けているかを確かめる術が「人に写真を撮ってもらう」しか無いと、
// まばたきが止まっていても表情が変わっていなくても気付けない。
// Avatar は RGB565 の配列へ画素を置くだけで M5GFX に触らないので
// (avatar.h の冒頭を見よ)、ここで画素を直接読んで判定できる。
//
// サーボの角度変換も同じ理由でここに置く。首が実際に回ったかは目で
// 見るしかないが、「どのパルス幅を指令したか」は実機を繋がずに検査できる。
// 可動域の外へ突っ込む計算ミスはここで止まる。

#include <cstdint>
#include <set>
#include <vector>

#include "avatar.h"
#include "doctest.h"
#include "servo.h"

namespace
{

using x68k_platform::Avatar;
using x68k_platform::BlinkPhase;
using x68k_platform::FaceExpression;

constexpr std::size_t kPixelCount = static_cast<std::size_t>(Avatar::kWidth) * Avatar::kHeight;

// 表示コアのループ周期。実機は vTaskDelay(16) で回る。
constexpr std::uint32_t kFrameMs = 16;

// 検査用のバッファを持つ顔。
struct TestFace
{
    std::vector<std::uint16_t> pixels;
    Avatar avatar;

    TestFace() : pixels(kPixelCount, 0)
    {
        avatar.setSpriteBuffer(pixels.data());
    }

    [[nodiscard]] std::uint16_t at(std::uint32_t x, std::uint32_t y) const
    {
        return pixels[static_cast<std::size_t>(y) * Avatar::kWidth + x];
    }

    // 白い画素の数。顔の「量」を測る大雑把な指標。
    [[nodiscard]] std::size_t litCount() const
    {
        std::size_t count = 0;
        for (const std::uint16_t p : pixels)
        {
            if (p != 0)
            {
                ++count;
            }
        }
        return count;
    }

    // 指定した列で白い画素が縦に何個あるか。目の高さを測る。
    [[nodiscard]] std::size_t columnHeight(std::uint32_t x) const
    {
        std::size_t count = 0;
        for (std::uint32_t y = 0; y < Avatar::kHeight; ++y)
        {
            if (at(x, y) != 0)
            {
                ++count;
            }
        }
        return count;
    }
};

// 目の中心が来る列。avatar.cpp の kEyeLeftX / kEyeRightX と揃えてある。
constexpr std::uint32_t kLeftEyeColumn = 104;
constexpr std::uint32_t kRightEyeColumn = 216;

// まばたきが 1 回終わるまで進める。掛かった時間 (ミリ秒) を返す。
std::uint32_t advanceOneBlink(Avatar& avatar)
{
    const std::uint32_t startCount = avatar.blinkCount();
    std::uint32_t elapsed = 0;
    // 上限を置く。まばたきが起きない実装になったとき、無限ループで
    // テストが固まるのではなく失敗させたい。
    constexpr std::uint32_t kLimitMs = 60000;
    while (avatar.blinkCount() == startCount && elapsed < kLimitMs)
    {
        avatar.tick(kFrameMs);
        elapsed += kFrameMs;
    }
    REQUIRE(avatar.blinkCount() == startCount + 1);
    return elapsed;
}

}  // namespace

TEST_SUITE("avatar")
{
    // --- まばたきの時間 ---------------------------------------------------

    TEST_CASE("起動直後は目が開いている")
    {
        const Avatar avatar;
        CHECK(avatar.blinkPhase() == BlinkPhase::Open);
        CHECK(avatar.lidClosure() == 0);
        CHECK(avatar.blinkCount() == 0);
    }

    TEST_CASE("まばたきは宣言した間隔の範囲で起きる")
    {
        // 等間隔だとメトロノームに見えるので範囲から選ぶ。ただし範囲を
        // 外れると「瞬かない」か「痙攣する」のどちらかになる。
        Avatar avatar;
        for (int i = 0; i < 20; ++i)
        {
            const std::uint32_t elapsed = advanceOneBlink(avatar);
            // 待ち時間 + 閉じる + 開く。コマ幅ぶんの誤差を見込む。
            const std::uint32_t minMs =
                Avatar::kBlinkIntervalMinMs + Avatar::kBlinkClosingMs + Avatar::kBlinkOpeningMs;
            const std::uint32_t maxMs = Avatar::kBlinkIntervalMaxMs + Avatar::kBlinkClosingMs +
                                        Avatar::kBlinkOpeningMs + kFrameMs;
            CHECK(elapsed >= minMs);
            CHECK(elapsed <= maxMs);
        }
    }

    TEST_CASE("まばたきの間隔は一定でない")
    {
        // 固定間隔で瞬くと機械仕掛けに見える。実際にばらけていることを
        // 確かめる (乱数の種は固定なので結果は決定的)。
        Avatar avatar;
        std::set<std::uint32_t> intervals;
        for (int i = 0; i < 12; ++i)
        {
            intervals.insert(advanceOneBlink(avatar));
        }
        CHECK(intervals.size() > 1);
    }

    TEST_CASE("コマが飛んでもまばたきの位相が実時間どおりに進む")
    {
        // 実機の表示ループは転送やモード切り替えで間隔が揺れる。
        // 1 回の呼び出しで 1 段階しか進めない実装だと、コマ落ちのたびに
        // まぶたが閉じたまま止まる。
        //
        // 細かく刻んだ顔と、粗く刻んだ顔が、同じ実時間で同じ回数だけ
        // 瞬くことを確かめる。
        Avatar fine;
        Avatar coarse;

        constexpr std::uint32_t kTotalMs = 30000;
        for (std::uint32_t t = 0; t < kTotalMs; t += 5)
        {
            fine.tick(5);
        }
        for (std::uint32_t t = 0; t < kTotalMs; t += 250)
        {
            coarse.tick(250);
        }

        CHECK(fine.blinkCount() == coarse.blinkCount());
        CHECK(fine.blinkCount() > 0);
    }

    TEST_CASE("閉じる方が開くより速い")
    {
        // 同じ時間で閉じて開くと、ゆっくり目を閉じる眠そうな動きになる。
        CHECK(Avatar::kBlinkClosingMs < Avatar::kBlinkOpeningMs);
    }

    TEST_CASE("まばたきの途中で全ての段階を通る")
    {
        Avatar avatar;
        std::set<BlinkPhase> seen;
        constexpr std::uint32_t kLimitMs = 60000;
        for (std::uint32_t t = 0; t < kLimitMs && avatar.blinkCount() == 0; t += kFrameMs)
        {
            avatar.tick(kFrameMs);
            seen.insert(avatar.blinkPhase());
        }
        CHECK(seen.count(BlinkPhase::Closing) == 1);
        CHECK(seen.count(BlinkPhase::Opening) == 1);
        CHECK(avatar.blinkPhase() == BlinkPhase::Open);
    }

    TEST_CASE("tick は見た目が変わったときだけ true を返す")
    {
        // 呼ぶ側は true のときだけ 320x240 を描いて SPI で送る。
        // 常に true を返すと、目が開いたままの間も毎コマ転送することになる。
        Avatar avatar;
        // 開いて待っている間は変化しない。
        CHECK_FALSE(avatar.tick(1));
        CHECK(avatar.lidClosure() == 0);

        // まばたきが始まるまで飛ばす。
        avatar.tick(avatar.msUntilBlink());
        // 閉じ始めたので変化がある。
        CHECK(avatar.tick(kFrameMs));
    }

    // --- 画素として顔が出ているか -----------------------------------------

    TEST_CASE("バッファが無ければ描けないが落ちない")
    {
        // スプライトの確保に失敗した実機でも起動は続く。
        Avatar avatar;
        CHECK_FALSE(avatar.hasSpriteBuffer());
        CHECK_FALSE(avatar.render());
        // tick は描画と独立に進む。
        avatar.tick(1000);
        CHECK(avatar.frameCount() == 0);
    }

    TEST_CASE("素の顔は目と口が出る")
    {
        TestFace face;
        REQUIRE(face.avatar.render());
        CHECK(face.avatar.frameCount() == 1);

        // 左右の目の中心が白い。
        CHECK(face.at(kLeftEyeColumn, 96) != 0);
        CHECK(face.at(kRightEyeColumn, 96) != 0);
        // 目と目の間は背景。ここが白いと顔が 1 つの塊になっている。
        CHECK(face.at(160, 96) == 0);
        // 口がある。
        CHECK(face.at(160, 176) != 0);
        // 四隅は背景。
        CHECK(face.at(0, 0) == 0);
        CHECK(face.at(Avatar::kWidth - 1, Avatar::kHeight - 1) == 0);
    }

    TEST_CASE("まぶたが降りると目が実際に潰れる")
    {
        // 状態 (lidClosure) が動いていても、描画が反映していなければ
        // 画面上は瞬いていない。画素で確かめる。
        TestFace face;
        REQUIRE(face.avatar.render());
        const std::size_t openHeight = face.columnHeight(kLeftEyeColumn);
        CHECK(openHeight > 0);

        // まばたきが始まるまで飛ばし、閉じきるまで進める。
        face.avatar.tick(face.avatar.msUntilBlink());
        while (face.avatar.blinkPhase() == BlinkPhase::Closing)
        {
            face.avatar.tick(kFrameMs);
        }
        REQUIRE(face.avatar.lidClosure() == 255);

        REQUIRE(face.avatar.render());
        const std::size_t closedHeight = face.columnHeight(kLeftEyeColumn);
        CHECK(closedHeight < openHeight);

        // 完全には消えない。消すと「目が無い顔」になる。
        CHECK(closedHeight > 0);

        // 口はまばたきで変わらない。連動すると顔が痙攣して見える。
        CHECK(face.at(160, 176) != 0);
    }

    TEST_CASE("まぶたは途中の開き具合も描き分ける")
    {
        // 閉じきった絵と開いた絵の 2 枚しか無いと、瞬きがパッと切り替わる
        // 点滅に見える。途中がその間の高さになっていることを確かめる。
        TestFace face;
        REQUIRE(face.avatar.render());
        const std::size_t openHeight = face.columnHeight(kLeftEyeColumn);

        face.avatar.tick(face.avatar.msUntilBlink());
        // 閉じる段階の途中まで進める。
        face.avatar.tick(Avatar::kBlinkClosingMs / 2);
        REQUIRE(face.avatar.blinkPhase() == BlinkPhase::Closing);
        REQUIRE(face.avatar.lidClosure() > 0);
        REQUIRE(face.avatar.lidClosure() < 255);

        REQUIRE(face.avatar.render());
        const std::size_t midHeight = face.columnHeight(kLeftEyeColumn);
        CHECK(midHeight < openHeight);
        CHECK(midHeight > 0);
    }

    // --- 表情 -------------------------------------------------------------

    TEST_CASE("表情ごとに違う絵が出る")
    {
        // FaceExpression が保存されるだけで描画に効いていないと、
        // 表情を変えても画面が変わらない。全表情の絵を突き合わせる。
        std::vector<std::vector<std::uint16_t>> snapshots;
        for (std::size_t i = 0; i < x68k_platform::kFaceExpressionCount; ++i)
        {
            TestFace face;
            face.avatar.setExpression(static_cast<FaceExpression>(i));
            REQUIRE(face.avatar.render());
            snapshots.push_back(face.pixels);
        }

        for (std::size_t a = 0; a < snapshots.size(); ++a)
        {
            for (std::size_t b = a + 1; b < snapshots.size(); ++b)
            {
                CHECK(snapshots[a] != snapshots[b]);
            }
        }
    }

    TEST_CASE("どの表情でも目と口が画面の中に収まる")
    {
        // はみ出しても putPixel が捨てるので落ちはしないが、顔の一部が
        // 切れた絵になる。白い画素が縁に触れていないことで確かめる。
        for (std::size_t i = 0; i < x68k_platform::kFaceExpressionCount; ++i)
        {
            TestFace face;
            face.avatar.setExpression(static_cast<FaceExpression>(i));
            REQUIRE(face.avatar.render());

            for (std::uint32_t x = 0; x < Avatar::kWidth; ++x)
            {
                CHECK(face.at(x, 0) == 0);
                CHECK(face.at(x, Avatar::kHeight - 1) == 0);
            }
            for (std::uint32_t y = 0; y < Avatar::kHeight; ++y)
            {
                CHECK(face.at(0, y) == 0);
                CHECK(face.at(Avatar::kWidth - 1, y) == 0);
            }
        }
    }

    TEST_CASE("眠い顔は素の顔より目が細い")
    {
        // 表情の意味づけが描画と合っていることを確かめる。眠いのに
        // 目が大きいと、名前と絵が食い違ったまま気付かない。
        TestFace neutral;
        neutral.avatar.setExpression(FaceExpression::Neutral);
        REQUIRE(neutral.avatar.render());

        TestFace sleepy;
        sleepy.avatar.setExpression(FaceExpression::Sleepy);
        REQUIRE(sleepy.avatar.render());

        CHECK(sleepy.columnHeight(kLeftEyeColumn) < neutral.columnHeight(kLeftEyeColumn));
    }

    TEST_CASE("驚いた顔は素の顔より目が大きい")
    {
        TestFace neutral;
        neutral.avatar.setExpression(FaceExpression::Neutral);
        REQUIRE(neutral.avatar.render());

        TestFace surprised;
        surprised.avatar.setExpression(FaceExpression::Surprised);
        REQUIRE(surprised.avatar.render());

        CHECK(surprised.columnHeight(kLeftEyeColumn) > neutral.columnHeight(kLeftEyeColumn));
    }

    TEST_CASE("笑い目も瞬くと潰れる")
    {
        // 笑い目 (∩) は既に閉じた目の形をしている。そのまま瞬かせても
        // 変化が出ないので、閉じる間は塗り潰しの目に戻す扱いにしてある。
        // その切り替えが効いていることを確かめる。
        TestFace face;
        face.avatar.setExpression(FaceExpression::Happy);
        REQUIRE(face.avatar.render());
        const std::size_t openLit = face.litCount();

        face.avatar.tick(face.avatar.msUntilBlink());
        while (face.avatar.blinkPhase() == BlinkPhase::Closing)
        {
            face.avatar.tick(kFrameMs);
        }
        REQUIRE(face.avatar.lidClosure() == 255);
        REQUIRE(face.avatar.render());

        CHECK(face.litCount() != openLit);
    }

    TEST_CASE("表情を変えてもまばたきの位相は保たれる")
    {
        // 表情を変えるたびにまばたきが最初からになると、シリアルから
        // 表情を切り替えている間ずっと瞬かない。
        Avatar avatar;
        avatar.tick(1000);
        const std::uint32_t before = avatar.msUntilBlink();
        avatar.setExpression(FaceExpression::Happy);
        CHECK(avatar.msUntilBlink() == before);
        CHECK(avatar.blinkPhase() == BlinkPhase::Open);
    }

    TEST_CASE("描くたびに前のコマが残らない")
    {
        // 全面を塗り直さないと、前の表情の口が新しい顔に残る。
        TestFace face;
        face.avatar.setExpression(FaceExpression::Surprised);
        REQUIRE(face.avatar.render());

        face.avatar.setExpression(FaceExpression::Sleepy);
        REQUIRE(face.avatar.render());

        // 眠い顔だけを描いた結果と一致すること。
        TestFace fresh;
        fresh.avatar.setExpression(FaceExpression::Sleepy);
        REQUIRE(fresh.avatar.render());

        CHECK(face.pixels == fresh.pixels);
    }
}

TEST_SUITE("servo")
{
    // --- 角度からパルス幅へ -----------------------------------------------

    TEST_CASE("正面は中立のパルス幅")
    {
        // 起動時と顔モードへ入るときに {} (0 度) を出す。ここが中立から
        // ずれていると、電源を入れるたびに首が斜めを向く。
        CHECK(x68k_platform::servoAngleToPulseUs(0.0F) == x68k_platform::kServoCenterPulseUs);
    }

    TEST_CASE("可動域の端がパルス幅の端に対応する")
    {
        CHECK(x68k_platform::servoAngleToPulseUs(x68k_platform::kServoMaxAngleDeg) ==
              x68k_platform::kServoMaxPulseUs);
        CHECK(x68k_platform::servoAngleToPulseUs(-x68k_platform::kServoMaxAngleDeg) ==
              x68k_platform::kServoMinPulseUs);
    }

    TEST_CASE("可動域の外は端で丸める")
    {
        // 丸めないとサーボが物理的な止まりへ押し付け続け、唸って発熱する。
        CHECK(x68k_platform::servoAngleToPulseUs(180.0F) == x68k_platform::kServoMaxPulseUs);
        CHECK(x68k_platform::servoAngleToPulseUs(-180.0F) == x68k_platform::kServoMinPulseUs);
    }

    TEST_CASE("角度が増えるとパルス幅も増える")
    {
        // 符号を取り違えると首が逆へ回る。実機でしか気付けない種類の
        // 間違いなので、向きをここで固定する。
        std::uint32_t previous = 0;
        for (int deg = -45; deg <= 45; deg += 5)
        {
            const std::uint32_t pulse = x68k_platform::servoAngleToPulseUs(static_cast<float>(deg));
            CHECK(pulse > previous);
            previous = pulse;
        }
    }

    // --- パルス幅からデューティへ -----------------------------------------

    TEST_CASE("デューティが 50Hz の周期に対する割合になっている")
    {
        // 周期 20000us を 2^16 等分する。1.5ms なら全体の 7.5%。
        constexpr std::uint32_t bits = x68k_platform::LedcServo::kResolutionBits;
        const std::uint32_t duty = x68k_platform::servoPulseUsToDuty(1500, bits);
        const std::uint32_t expected = static_cast<std::uint32_t>(1500ULL * (1ULL << bits) / 20000);
        CHECK(duty == expected);

        // 割合として妥当か (7.5% ± わずか)。
        const double ratio = static_cast<double>(duty) / static_cast<double>(1U << bits);
        CHECK(ratio > 0.074);
        CHECK(ratio < 0.076);
    }

    TEST_CASE("可動域の端でもデューティが分解能に収まる")
    {
        // 溢れると LEDC が別の値として受け取り、まったく違う角度になる。
        constexpr std::uint32_t bits = x68k_platform::LedcServo::kResolutionBits;
        const std::uint32_t maxDuty = x68k_platform::servoPulseUsToDuty(
            x68k_platform::servoAngleToPulseUs(x68k_platform::kServoMaxAngleDeg), bits);
        CHECK(maxDuty < (1U << bits));
        CHECK(maxDuty > 0);
    }

    // --- サーボが無い状態 -------------------------------------------------

    TEST_CASE("NullServo は付いていないと答え、指示を捨てる")
    {
        // サーボが無いのが通常の状態。呼ぶ側に nullptr チェックを
        // 書かせないための Null Object。
        x68k_platform::NullServo servo;
        CHECK(servo.begin());
        CHECK_FALSE(servo.isAttached());

        servo.setPose({30.0F, -10.0F});
        // 捨てるが覚えている。FSM が首をどこへ向けようとしたかは検査したい。
        CHECK(servo.lastPose().pan == doctest::Approx(30.0F));
        CHECK(servo.lastPose().tilt == doctest::Approx(-10.0F));

        servo.detach();
        CHECK(servo.isDetached());
    }
}
