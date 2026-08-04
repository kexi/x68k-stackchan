// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 首振りサーボの口と、LEDC による実装。
//
// 【既定は NullServo (何もしない)】サーボが付いていない CoreS3 単体が
// 通常の開発形態で、顔も X68000 もサーボ無しで成立する。無い方を例外
// 扱いにすると、常に警告が出るか初期化に失敗して起動が止まる。
//
// 実物を駆動するのは LedcServo (servo.cpp)。ESP-IDF の LEDC で
// 50Hz・パルス幅 0.5-2.4ms の標準的なホビーサーボ信号を出す。
//
// 【重要】サーボが繋がっているかはソフトウェアからは分からない。
// サーボの信号線は入力専用で、応答も位置のフィードバックも返らない。
// LedcServo::isAttached() が返すのは「PWM の口を開けたか」であって
// 「モータが付いているか」ではない。したがって:
//
//   - 付いていない状態で LedcServo を使っても害は無い。GPIO が
//     50Hz で振れるだけで、電流も流れない。
//   - 逆に「付いている」と表示されても実際に首が回った証拠にはならない。
//     首が動いたことは目で見るしかない。
//
// Why not 既定を LedcServo にしないか: どのピンに何が繋がっているかは
// 個体で変わる。CoreS3 のスタックチャンはサーボを Port.A (GPIO 1/2) に
// 載せる作例が多いが、その 2 本は M5Unified が外部 I2C にも使う
// (M5Unified.cpp の _pin_table_i2c_ex_in で CoreS3 は ex_i2c = GPIO 1/2)。
// Port.A に I2C の Unit を挿している人の環境で既定を PWM にすると、
// I2C のバスへ 50Hz の矩形波を流し込むことになる。使う人が明示的に
// 選んだときだけ張る。

#ifndef X68K_PLATFORM_SERVO_H
#define X68K_PLATFORM_SERVO_H

#include <cstdint>

namespace x68k_platform
{

// 首の向き。単位は度。
//
// 原点は「正面・水平」。スタックチャンの慣習に合わせてある。
//   pan:  左右。正が右向き。
//   tilt: 上下。正が上向き。
//
// Why not ラジアンにしないか: サーボの可動域はデータシートも
// スタックチャンの作例も度で書かれている (概ね ±45 度)。変換を挟むと、
// 実機で角度を詰めるときに毎回頭の中で換算することになる。
struct HeadPose
{
    float pan = 0.0F;
    float tilt = 0.0F;
};

// 首振りの口。
//
// Why not 関数ポインタや std::function にしないか: 状態を持つ実装
// (現在角度・可動域の制限・速度制限) が要る。仮想関数 1 段の分岐は、
// 呼ぶ頻度 (切り替えのたび、あるいは数十 Hz) では測れない。
class Servo
{
public:
    virtual ~Servo() = default;

    // サーボを使えるようにする。使えなければ false。
    //
    // 呼ぶ側は false を異常として扱わないこと。サーボが付いていないのは
    // 普通の状態で、顔も X68000 も動く。
    virtual bool begin() = 0;

    // 首を向ける。可動域の外は実装側で丸める。
    virtual void setPose(const HeadPose& pose) = 0;

    // 力を抜く (PWM を止める)。
    //
    // 顔モードを抜けるときに呼ぶ想定。X68000 を触っている間サーボを
    // 保持し続けると、電流を食うだけでなく、安物のサーボは唸りが出る。
    virtual void detach() = 0;

    // サーボが実際に付いているか。
    //
    // begin() が成功したかではなく「動かす先があるか」。NullServo は
    // begin() が true を返すが、ここは false。
    [[nodiscard]] virtual bool isAttached() const = 0;
};

// 何もしないサーボ。既定。
//
// サーボが付いていない CoreS3 単体で動かすときはこれを使う。
// 呼ぶ側に nullptr チェックを書かせないための実装で、Null Object。
//
// Why not Servo* を nullptr にして呼ぶ側で分岐させないか: 分岐が
// 呼び出し箇所ぶん増える。1 つ書き忘れると、サーボが付いていない
// 個体でだけ落ちる。落ちる条件が「持っていないハードウェア」なので、
// 手元では再現しない。
class NullServo final : public Servo
{
public:
    bool begin() override
    {
        return true;
    }

    void setPose(const HeadPose& pose) override
    {
        // 捨てるが、最後の指示は覚えておく。
        //
        // Why not 完全に捨てないか: サーボが無い個体でも「FSM が
        // 首をどこへ向けようとしたか」を検査したい。覚えていれば
        // ホストのテストから確かめられる。
        pose_ = pose;
    }

    void detach() override
    {
        isDetached_ = true;
    }

    [[nodiscard]] bool isAttached() const override
    {
        return false;
    }

    // --- テストと診断のための覗き口 ---

    [[nodiscard]] const HeadPose& lastPose() const
    {
        return pose_;
    }

    [[nodiscard]] bool isDetached() const
    {
        return isDetached_;
    }

private:
    HeadPose pose_;
    bool isDetached_ = false;
};

// --- ホビーサーボの信号 ------------------------------------------------------
//
// 一般的なアナログサーボ (SG90 / SG92R など、スタックチャンの作例が使うもの)
// は周期 20ms (50Hz) の矩形波を受け取り、パルスの幅で角度を決める。
// 中立が約 1.5ms、両端が約 0.5ms と 2.4ms。

// PWM の周期。50Hz。
inline constexpr std::uint32_t kServoFrequencyHz = 50;

// パルス幅の下限・中立・上限 (マイクロ秒)。
//
// Why 0.5-2.4ms にするか: 規格上は 1.0-2.0ms が 90 度ぶんだが、SG90 系は
// 0.5-2.4ms で約 180 度回る。スタックチャンの作例もこの幅を使う。
// 狭く取ると首の可動域が半分になる。
//
// Why not もっと広げないか: 端を超えるとサーボは物理的な止まりへ押し付け
// 続け、モータが唸って発熱する。データシートの範囲で止める。
inline constexpr std::uint32_t kServoMinPulseUs = 500;
inline constexpr std::uint32_t kServoCenterPulseUs = 1450;
inline constexpr std::uint32_t kServoMaxPulseUs = 2400;

// 首の可動域 (度)。原点は正面・水平。
//
// Why ±45 度に絞るか: サーボ自体は ±90 度回るが、スタックチャンの
// 頭は筐体と配線に当たる。作例も概ね ±45 度で使う。
inline constexpr float kServoMaxAngleDeg = 45.0F;

// 角度をパルス幅 (マイクロ秒) へ変換する。可動域の外は丸める。
//
// Why not LedcServo のメンバにしないか: この変換は ESP-IDF を必要と
// しない純粋な算術で、間違えると首が可動域の外へ突っ込む。ホストの
// テストから直接呼べる形にしておけば、実機を繋がずに端と中立を
// 検査できる (サーボの動きは目で見るしかないが、指令値は検査できる)。
[[nodiscard]] inline std::uint32_t servoAngleToPulseUs(float angleDeg)
{
    const bool isBelowRange = angleDeg < -kServoMaxAngleDeg;
    if (isBelowRange)
    {
        angleDeg = -kServoMaxAngleDeg;
    }
    const bool isAboveRange = angleDeg > kServoMaxAngleDeg;
    if (isAboveRange)
    {
        angleDeg = kServoMaxAngleDeg;
    }

    // 中立から左右で幅が違う (中立 1450us に対し下 950us / 上 950us)。
    // 対称なので片側の幅を掛けるだけで済む。
    const float halfSpanUs = angleDeg >= 0.0F
                                 ? static_cast<float>(kServoMaxPulseUs - kServoCenterPulseUs)
                                 : static_cast<float>(kServoCenterPulseUs - kServoMinPulseUs);
    const float offsetUs = angleDeg / kServoMaxAngleDeg * halfSpanUs;
    return static_cast<std::uint32_t>(static_cast<float>(kServoCenterPulseUs) + offsetUs + 0.5F);
}

// パルス幅 (マイクロ秒) を LEDC のデューティ値へ変換する。
//
// LEDC は「周期を 2^resolutionBits 等分したうちいくつ High か」で
// デューティを表す。周期は 1/kServoFrequencyHz = 20ms なので、
// duty = pulseUs / 20000us * 2^bits。
//
// Why not LedcServo の中で計算しないか: 上と同じ理由。分解能を変えた
// ときに中立のデューティが合っているかを、実機に焼かずに確かめたい。
[[nodiscard]] inline std::uint32_t servoPulseUsToDuty(std::uint32_t pulseUs,
                                                      std::uint32_t resolutionBits)
{
    const std::uint32_t periodUs = 1000000U / kServoFrequencyHz;
    const std::uint64_t maxDuty = 1ULL << resolutionBits;
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(pulseUs) * maxDuty / periodUs);
}

// LEDC で実際に PWM を出すサーボ。実体は servo.cpp (ESP-IDF が要る)。
//
// 【重要】「繋がっているか」は分からない。isAttached() が真を返すのは
// 「LEDC の口を開けられた」という意味で、モータの有無ではない
// (このファイル冒頭を見よ)。
//
// Why not ヘッダオンリーにしないか: driver/ledc.h を引くと、servo.h を
// 読む側すべてが ESP-IDF を要求することになる。app_mode のテストは
// servo.h を読んでおり (test_app_mode.cpp)、ホストでビルドできなくなる。
// 宣言だけ置いて実体を servo.cpp へ追い出せば、ホスト側は今までどおり。
class LedcServo final : public Servo
{
public:
    // LEDC の分解能。
    //
    // Why 16 ビットか: 周期 20ms を 65536 等分するので 1 段が 0.305us。
    // サーボの分解能 (良くて 1us 程度) より細かく、角度の丸めが見えない。
    //
    // Why not もっと上げないか: LEDC のクロックは APB 80MHz。
    // 50Hz で使える最大の分解能は log2(80MHz / 50Hz) = 20.6 ビットで、
    // 16 は余裕を持って収まる。上げても得るものが無い。
    static constexpr std::uint32_t kResolutionBits = 16;

    // 使う GPIO を決める。begin() より前に呼ぶ。
    //
    // 既定は CoreS3 の Port.A (pan=GPIO1, tilt=GPIO2)。スタックチャンの
    // 作例が最も多く使う配線に合わせてある。
    //
    // 負の値を渡すとその軸を使わない。片方のサーボしか付けない構成
    // (pan だけ振る) を表せるようにしてある。
    void setPins(int panGpio, int tiltGpio);

    // LEDC を用意する。失敗したら false。
    //
    // 失敗しても呼ぶ側は続行してよい。以後の setPose は捨てられる。
    bool begin() override;

    // 首を向ける。可動域の外は servoAngleToPulseUs が丸める。
    void setPose(const HeadPose& pose) override;

    // PWM を止める。GPIO は Low に落とす。
    //
    // Why Low にするか: 出力を止めるだけだと最後の状態 (High の途中なら
    // High) で固まる。サーボによっては連続 High を異常なパルスと解釈して
    // 端まで回そうとする。Low に落とせば「信号なし」になり、サーボは
    // 保持をやめて自由になる。
    void detach() override;

    [[nodiscard]] bool isAttached() const override
    {
        return isReady_;
    }

    // --- 診断のための覗き口 ---
    //
    // 実機ではサーボの動きを目で見るしかないので、せめて「何を出したか」を
    // ログへ出せるようにする。

    [[nodiscard]] const HeadPose& lastPose() const
    {
        return pose_;
    }

    [[nodiscard]] std::uint32_t lastPanDuty() const
    {
        return panDuty_;
    }

    [[nodiscard]] std::uint32_t lastTiltDuty() const
    {
        return tiltDuty_;
    }

    [[nodiscard]] bool isDetached() const
    {
        return isDetached_;
    }

private:
    int panGpio_ = 1;
    int tiltGpio_ = 2;
    bool isReady_ = false;
    bool isDetached_ = false;
    HeadPose pose_;
    std::uint32_t panDuty_ = 0;
    std::uint32_t tiltDuty_ = 0;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_SERVO_H
