// SPDX-License-Identifier: MIT
#ifndef X68K_PLATFORM_GUEST_AUDIO_H
#define X68K_PLATFORM_GUEST_AUDIO_H

#include <algorithm>
#include <array>
#include <cstring>

#include "audio.h"

namespace x68k_platform
{
// Core1専用。10MHz/15625Hzの固定レート。再生キュー水位から未来を合成しない。
// Machineのcallbackへ接続する側が寿命とsample rateを保証する。
class GuestAudioProducer
{
public:
    // ゲスト 10MHz / 合成レート。Opm::kDefaultSampleRate と積が 10^7 になる
    // 値でなければ、音の速さがゲスト時間からずれる。
    // 62500Hz なので 160。倍音の折り返しと波形の階段を避けるためレートを上げた経緯は
    // opm.h の kDefaultSampleRate のコメントを見よ。
    static constexpr std::uint64_t kCyclesPerSample = 160;
    explicit GuestAudioProducer(AudioChannel& channel) : channel_(channel) {}
    GuestAudioProducer(const GuestAudioProducer&) = delete;
    GuestAudioProducer& operator=(const GuestAudioProducer&) = delete;

    // resetは新epoch。公開済みringは消費者所有なので巻き戻さない。
    void reset()
    {
        discardedOnReset_ += filled_;
        filled_ = 0;
        samples_ = 0;
        channel_.restartStream(paused_);
    }

    void setPaused(bool paused)
    {
        const bool changed = paused != paused_;
        if (!changed)
        {
            return;
        }
        discardedOnPause_ += filled_;
        filled_ = 0;
        paused_ = paused;
        channel_.restartStream(paused);
    }

    bool syncTo(x68k::Machine& machine, std::uint64_t cycles)
    {
        const auto target = cycles / kCyclesPerSample;
        const bool backwards = target < samples_;
        if (backwards)
        {
            ++clockErrors_;
            return false;
        }
        while (samples_ < target)
        {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(AudioChannel::kBlockFrames - filled_, target - samples_));
            machine.renderAudio(partial_.data() + filled_, count);
            filled_ += count;
            samples_ += count;
            const bool complete = filled_ == AudioChannel::kBlockFrames;
            if (!complete)
            {
                continue;
            }
            if (paused_)
            {
                discardedOnPause_ += AudioChannel::kBlockFrames;
                filled_ = 0;
                continue;
            }
            auto* const out = channel_.writeBlock();
            const bool available = out != nullptr;
            if (available)
            {
                std::memcpy(out, partial_.data(), sizeof(partial_));
                channel_.commit();
                ++published_;
            }
            else
            {
                // 満杯でも位相/FIFOを止めない。失ったサンプルを明示してpacingへ返す。
                droppedSamples_ += AudioChannel::kBlockFrames;
            }
            filled_ = 0;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t samples() const
    {
        return samples_;
    }
    [[nodiscard]] std::size_t partialFrames() const
    {
        return filled_;
    }
    [[nodiscard]] std::uint64_t publishedBlocks() const
    {
        return published_;
    }
    [[nodiscard]] std::uint64_t droppedSamples() const
    {
        return droppedSamples_;
    }
    [[nodiscard]] std::uint64_t discardedOnReset() const
    {
        return discardedOnReset_;
    }
    [[nodiscard]] std::uint64_t clockErrors() const
    {
        return clockErrors_;
    }
    [[nodiscard]] std::uint64_t discardedOnPause() const
    {
        return discardedOnPause_;
    }

private:
    AudioChannel& channel_;
    std::array<std::int16_t, AudioChannel::kBlockFrames> partial_{};
    std::size_t filled_ = 0;
    std::uint64_t samples_ = 0;
    std::uint64_t published_ = 0;
    std::uint64_t droppedSamples_ = 0;
    std::uint64_t discardedOnReset_ = 0;
    std::uint64_t clockErrors_ = 0;
    std::uint64_t discardedOnPause_ = 0;
    bool paused_ = false;
};
}  // namespace x68k_platform
#endif
