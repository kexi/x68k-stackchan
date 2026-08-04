// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// AudioSink を M5Unified のスピーカーで実装する。
//
// Why not audio.cpp に入れないか: audio.cpp は ESP-IDF に触らないので
// ホストのテストからそのままビルドでき、リングの受け渡しを検査できる
// (test/test_audio.cpp)。M5Unified を入れた瞬間にそれができなくなる。
// 実機依存はこのヘッダに閉じ込め、実機ビルドからだけインクルードする。

#ifndef X68K_PLATFORM_SPEAKER_M5_H
#define X68K_PLATFORM_SPEAKER_M5_H

#include <M5Unified.h>
#include <esp_log.h>

#include "audio.h"

namespace x68k_platform
{

class M5SpeakerSink final : public AudioSink
{
public:
    // 出力レート。Machine の合成レート (OPM/ADPCM とも既定 15625Hz) と揃える。
    //
    // Why not 48000Hz (M5Unified の既定) にするか: 揃えないと playRaw の
    // 内部で線形補間のリサンプルが走る。合成側を 48kHz へ上げるのは
    // 3 倍のコストで論外なので、どちらにせよ変換は要るが、I2S 側の
    // sample_rate を 15625 にしておけば Speaker は等倍で流すだけになり、
    // 変換のぶんが Core0 から消える。
    static constexpr x68k::u32 kSampleRate = x68k::Opm::kDefaultSampleRate;

    // スピーカーを開始する。使えなければ false。
    bool begin()
    {
        auto cfg = M5.Speaker.config();
        cfg.sample_rate = kSampleRate;
        cfg.stereo = false;

        // I2S の背景タスクを Core0 へ固定する。
        //
        // 固定しないと (既定は ~0 = 任意) スケジューラが空いている方へ
        // 置く。エミュレーションが Core1 を 100% 使い切っているので、
        // 音声タスクが Core1 に載ると 8 スライスに 1 回の vTaskDelay まで
        // 順番が回らず、DMA が枯れて音が途切れる。issue #8 が引く
        // stackchan-dapan の知見 (Wi-Fi/lwIP を Core0 へ pin しないと
        // 音が途切れる) と同じ話で、ここでは「侵される側」が音声になる。
        cfg.task_pinned_core = 0;

        // 表示ループ (Core0 の while) より高い優先度にする。
        // 表示は 16ms ごとに 150KB を SPI へ流すので、同じ優先度だと
        // その間 DMA の補充が止まる。
        cfg.task_priority = 3;

        M5.Speaker.config(cfg);
        if (!M5.Speaker.begin())
        {
            return false;
        }

        // 実際に I2S が張れたかまで確かめる。
        //
        // Why not begin() の戻り値だけを信じないか: Speaker_Class::begin は
        // 「既に走っていれば即 true」を返し (Speaker_Class.cpp:918)、
        // playRaw も _task_handle が無いと何もせず true を返す
        // (_play_raw の 1 行目)。pin_data_out が付いていない機種では
        // 「begin は成功、playRaw も成功、しかし 1 サンプルも出ていない」
        // という状態になり、ログ上は正常に見えてしまう。実測で
        // 「1 ブロックあたりの実時間がレートより桁違いに速い」形で
        // 露見したので、ここで判別できるようにする。
        if (!M5.Speaker.isEnabled() || !M5.Speaker.isRunning())
        {
            ESP_LOGW("x68k.spk", "スピーカーが有効になりません (pin_data_out=%d running=%d)",
                     cfg.pin_data_out, static_cast<int>(M5.Speaker.isRunning()));
            return false;
        }

        // 既定の音量は最大 (255)。CoreS3 の内蔵スピーカーで X68000 の
        // FM を最大振幅で鳴らすと割れるので下げる。
        M5.Speaker.setVolume(160);

        ESP_LOGI("x68k.spk", "スピーカー: %u Hz pin_data_out=%d bck=%d ws=%d core=%u",
                 static_cast<unsigned>(cfg.sample_rate), cfg.pin_data_out, cfg.pin_bck, cfg.pin_ws,
                 static_cast<unsigned>(cfg.task_pinned_core));
        return true;
    }

    void end()
    {
        M5.Speaker.end();
    }

    void write(const std::int16_t* samples, std::size_t frames) override
    {
        // stop_current_sound = false。前のブロックを切らずに後ろへ繋ぐ。
        //
        // 前の 2 枚がまだ再生待ちならここで待つ (Speaker_Class.cpp の
        // _set_next_wav)。待つのはこのタスクだけで、エミュレーションコアは
        // リングへ積むだけなので影響を受けない。これがリングを挟んだ理由。
        M5.Speaker.playRaw(samples, frames, kSampleRate, false, 1, 0, false);
    }

    [[nodiscard]] x68k::u32 sampleRate() const override
    {
        return kSampleRate;
    }
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_SPEAKER_M5_H
