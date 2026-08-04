// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// LEDC でホビーサーボの PWM を出す。角度とパルス幅の対応は servo.h。

#include "servo.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>

namespace x68k_platform
{
namespace
{

constexpr char kTag[] = "x68k.servo";

// 使う LEDC のタイマとチャンネル。
//
// Why 低速モードか: ESP32-S3 の LEDC には高速モードが無い (LOW_SPEED_MODE
// だけ)。ここを HIGH_SPEED_MODE にすると ledc_timer_config が
// ESP_ERR_INVALID_ARG を返す。
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_3;
constexpr ledc_channel_t kPanChannel = LEDC_CHANNEL_6;
constexpr ledc_channel_t kTiltChannel = LEDC_CHANNEL_7;

// Why タイマとチャンネルを末尾から取るか: 同じ LEDC を M5Unified が
// 使う可能性がある。CoreS3 は LCD のバックライトを AXP2101 (I2C の
// 電源 IC) で調光するので今は衝突しないが、ほかの M5 デバイスでは
// バックライトが LEDC のチャンネル 0 付近に載る。末尾を使えば、
// 将来 M5GFX が LEDC を掴む構成でも当たりにくい。

}  // namespace

void LedcServo::setPins(int panGpio, int tiltGpio)
{
    panGpio_ = panGpio;
    tiltGpio_ = tiltGpio;
}

bool LedcServo::begin()
{
    const bool hasNoPin = panGpio_ < 0 && tiltGpio_ < 0;
    if (hasNoPin)
    {
        ESP_LOGW(kTag, "サーボのピンが 1 本も指定されていません。首は振りません");
        return false;
    }

    ledc_timer_config_t timerCfg = {};
    timerCfg.speed_mode = kMode;
    timerCfg.duty_resolution = static_cast<ledc_timer_bit_t>(kResolutionBits);
    timerCfg.timer_num = kTimer;
    timerCfg.freq_hz = kServoFrequencyHz;
    timerCfg.clk_cfg = LEDC_AUTO_CLK;
    timerCfg.deconfigure = false;

    const esp_err_t timerErr = ledc_timer_config(&timerCfg);
    if (timerErr != ESP_OK)
    {
        // 起動は止めない。
        //
        // Why not ここで abort しないか: サーボは飾りで、X68000 の
        // エミュレーションはサーボ無しで完結する。LEDC が取れないことを
        // 致命傷にすると、顔も画面も出ないまま終わる。
        ESP_LOGE(kTag, "LEDC のタイマを用意できません (%s)。首は振りません",
                 esp_err_to_name(timerErr));
        return false;
    }

    // 中立のデューティで開ける。
    //
    // Why not デューティ 0 で開けないか: デューティ 0 は「パルスが無い」
    // 状態で、サーボは前の位置を保持したまま脱力する。付いている個体では
    // 首がだらんと落ち、電源投入のたびにガクンと鳴る。中立で開ければ、
    // 最初のパルスから正面を向く。
    //
    // Why 中立が安全か: 可動域の中央なので、どんな組み付けでも
    // 物理的な止まりへ押し付けない。
    const std::uint32_t centerDuty = servoPulseUsToDuty(kServoCenterPulseUs, kResolutionBits);

    bool isAnyChannelReady = false;

    const bool hasPan = panGpio_ >= 0;
    if (hasPan)
    {
        ledc_channel_config_t channelCfg = {};
        channelCfg.gpio_num = panGpio_;
        channelCfg.speed_mode = kMode;
        channelCfg.channel = kPanChannel;
        channelCfg.intr_type = LEDC_INTR_DISABLE;
        channelCfg.timer_sel = kTimer;
        channelCfg.duty = centerDuty;
        channelCfg.hpoint = 0;
        const esp_err_t err = ledc_channel_config(&channelCfg);
        if (err == ESP_OK)
        {
            panDuty_ = centerDuty;
            isAnyChannelReady = true;
        }
        else
        {
            ESP_LOGE(kTag, "pan (GPIO%d) を開けません (%s)", panGpio_, esp_err_to_name(err));
        }
    }

    const bool hasTilt = tiltGpio_ >= 0;
    if (hasTilt)
    {
        ledc_channel_config_t channelCfg = {};
        channelCfg.gpio_num = tiltGpio_;
        channelCfg.speed_mode = kMode;
        channelCfg.channel = kTiltChannel;
        channelCfg.intr_type = LEDC_INTR_DISABLE;
        channelCfg.timer_sel = kTimer;
        channelCfg.duty = centerDuty;
        channelCfg.hpoint = 0;
        const esp_err_t err = ledc_channel_config(&channelCfg);
        if (err == ESP_OK)
        {
            tiltDuty_ = centerDuty;
            isAnyChannelReady = true;
        }
        else
        {
            ESP_LOGE(kTag, "tilt (GPIO%d) を開けません (%s)", tiltGpio_, esp_err_to_name(err));
        }
    }

    isReady_ = isAnyChannelReady;
    isDetached_ = false;

    if (!isReady_)
    {
        return false;
    }

    ESP_LOGI(kTag, "LEDC を張りました pan=GPIO%d tilt=GPIO%d %uHz %u ビット 中立 duty=%u", panGpio_,
             tiltGpio_, static_cast<unsigned>(kServoFrequencyHz),
             static_cast<unsigned>(kResolutionBits), static_cast<unsigned>(centerDuty));

    // 【重要】ここで真を返しても、サーボが繋がっている保証は無い。
    //
    // サーボの信号線は入力専用で応答を返さないので、繋がっているかは
    // ソフトウェアからは判定できない。この行が意味するのは「GPIO が
    // 50Hz で振れている」ことだけ。首が動いたかは目で見るしかない。
    ESP_LOGW(kTag, "PWM を出しています。サーボが実際に繋がっているかは判定できません");
    return true;
}

void LedcServo::setPose(const HeadPose& pose)
{
    pose_ = pose;

    if (!isReady_)
    {
        return;
    }

    const std::uint32_t panDuty =
        servoPulseUsToDuty(servoAngleToPulseUs(pose.pan), kResolutionBits);
    const std::uint32_t tiltDuty =
        servoPulseUsToDuty(servoAngleToPulseUs(pose.tilt), kResolutionBits);

    const bool hasPan = panGpio_ >= 0;
    if (hasPan)
    {
        ledc_set_duty(kMode, kPanChannel, panDuty);
        ledc_update_duty(kMode, kPanChannel);
        panDuty_ = panDuty;
    }

    const bool hasTilt = tiltGpio_ >= 0;
    if (hasTilt)
    {
        ledc_set_duty(kMode, kTiltChannel, tiltDuty);
        ledc_update_duty(kMode, kTiltChannel);
        tiltDuty_ = tiltDuty;
    }

    // detach した後の setPose は張り直しになる。
    isDetached_ = false;
}

void LedcServo::detach()
{
    if (!isReady_)
    {
        return;
    }

    // 出力を止めて Low に落とす。
    //
    // ledc_stop の第 3 引数が停止後の出力レベル。0 = Low。
    // High で止めるとサーボによっては端まで回そうとする (servo.h を見よ)。
    const bool hasPan = panGpio_ >= 0;
    if (hasPan)
    {
        ledc_stop(kMode, kPanChannel, 0);
        panDuty_ = 0;
    }

    const bool hasTilt = tiltGpio_ >= 0;
    if (hasTilt)
    {
        ledc_stop(kMode, kTiltChannel, 0);
        tiltDuty_ = 0;
    }

    isDetached_ = true;
}

}  // namespace x68k_platform
