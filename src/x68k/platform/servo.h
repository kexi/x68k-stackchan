// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 首振りサーボの口。
//
// 【重要】この実装は現時点で何も動かさない。サーボが繋がっていない
// 前提で、すべての操作を捨てる。実機に PWM を出す部分は未実装。
//
// Why not 最初から PWM を出す実装を書かないか (3 つある):
//
// 1. 繋がっていないものを駆動できない。CoreS3 のスタックチャン用サーボは
//    Port.A (GPIO 1/2) か、専用の基板 (Stack-chan_Takao_Base など) に
//    載る。どちらが付くかで GPIO も PWM のチャンネル数も変わる。
//    手元の個体で確かめずに番号を決め打ちすると、別の用途に割り当てられた
//    ピンを叩くことになる。CoreS3 の GPIO 1/2 は M5Unified が I2C や
//    電源管理で使う可能性があり、当てずっぽうで LEDC を張るのは危ない。
//
// 2. 「サーボが無い」が普通の状態。開発は CoreS3 単体で進んでおり、
//    顔と X68000 の切り替えはサーボ無しで成立する。無い方を例外扱いに
//    すると、常に警告が出るか、初期化に失敗して起動が止まる。
//
// 3. 呼ぶ側を先に固めたい。切り替えのたびに「顔を正面へ戻す」ような
//    指示は FSM 側の判断で、サーボの有無とは独立に決まる。口だけ
//    決めておけば、呼ぶ側のコードはハードウェアが付いた後も変わらない。
//
// 付けるときにやること:
//   NullServo を置き換える実装を足し、main.cpp が渡すインスタンスを
//   差し替える。角度の単位と原点 (下の Servo のコメント) は変えない。

#ifndef X68K_PLATFORM_SERVO_H
#define X68K_PLATFORM_SERVO_H

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

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_SERVO_H
