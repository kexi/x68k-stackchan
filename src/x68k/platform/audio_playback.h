// SPDX-License-Identifier: MIT
#ifndef X68K_PLATFORM_AUDIO_PLAYBACK_H
#define X68K_PLATFORM_AUDIO_PLAYBACK_H

#include <algorithm>
#include <array>
#include <cstring>
#include "audio.h"

namespace x68k_platform
{
// Core0所有。供給不足を音源の未来合成で埋めず、明示的な無音として数える。
class AudioPlayback
{
public:
    static constexpr std::size_t kFadeFrames = 64;
    AudioPlayback(AudioChannel& channel, AudioSink& sink)
        : channel_(channel), sink_(sink), streamState_(channel.streamState())
    {
    }

    void submit(bool muted)
    {
        applyStreamState();
        const auto* const source = channel_.readBlock();
        bool supplied = source != nullptr;
        if (supplied)
        {
            std::memcpy(block_.data(), source, sizeof(block_));
            // sinkの待機中もproducerは次のslotを使える。出力用コピーはこのクラス所有。
            channel_.releaseRead();
        }
        // A reset during the copy invalidates its content, not the lease lifetime.
        const bool changedDuringCopy = channel_.streamState() != streamState_;
        if (changedDuringCopy)
        {
            applyStreamState();
            supplied = false;
        }
        const bool paused = (streamState_ & 1u) != 0;
        sourceFrames_ += supplied && !paused ? AudioChannel::kBlockFrames : 0;
        pausedFrames_ += paused ? AudioChannel::kBlockFrames : 0;
        missingFrames_ += !supplied && !paused ? AudioChannel::kBlockFrames : 0;
        const bool silent = !supplied || muted || paused;
        if (silent)
        {
            block_.fill(0);
            for (std::size_t i = 0; i < kFadeFrames; ++i)
            {
                block_[i] =
                    static_cast<std::int16_t>(static_cast<std::int32_t>(last_) *
                                              static_cast<std::int32_t>(kFadeFrames - 1 - i) /
                                              static_cast<std::int32_t>(kFadeFrames));
            }
        }
        else if (wasSilent_)
        {
            for (std::size_t i = 0; i < kFadeFrames; ++i)
            {
                block_[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(block_[i]) *
                                                      static_cast<std::int32_t>(i + 1) /
                                                      static_cast<std::int32_t>(kFadeFrames));
            }
        }
        mutedFrames_ += muted ? AudioChannel::kBlockFrames : 0;
        wasSilent_ = silent;
        last_ = block_.back();
        lastPeak_ = peakAmplitude(block_.data(), block_.size());
        if (streamReady_)
        {
            sink_.write(block_.data(), block_.size());
        }
        else
        {
            failedFrames_ += AudioChannel::kBlockFrames;
        }
    }

    [[nodiscard]] std::uint64_t sourceFrames() const
    {
        return sourceFrames_;
    }
    [[nodiscard]] std::uint64_t missingFrames() const
    {
        return missingFrames_;
    }
    [[nodiscard]] std::uint64_t mutedFrames() const
    {
        return mutedFrames_;
    }
    [[nodiscard]] std::int32_t lastPeak() const
    {
        return lastPeak_;
    }
    [[nodiscard]] std::uint64_t pausedFrames() const
    {
        return pausedFrames_;
    }
    [[nodiscard]] std::uint64_t failedFrames() const
    {
        return failedFrames_;
    }
    [[nodiscard]] std::uint32_t streamRestarts() const
    {
        return streamRestarts_;
    }
    [[nodiscard]] std::uint32_t restartFailures() const
    {
        return restartFailures_;
    }

private:
    void applyStreamState()
    {
        const auto state = channel_.streamState();
        const bool changed = state != streamState_;
        if (!changed)
        {
            return;
        }
        streamReady_ = sink_.restartStream();
        ++streamRestarts_;
        restartFailures_ += streamReady_ ? 0u : 1u;
        streamState_ = state;
        // No fade may refer to a sample from the old guest epoch.
        last_ = 0;
        wasSilent_ = true;
    }

    AudioChannel& channel_;
    AudioSink& sink_;
    std::array<std::int16_t, AudioChannel::kBlockFrames> block_{};
    std::uint64_t sourceFrames_ = 0;
    std::uint64_t missingFrames_ = 0;
    std::uint64_t mutedFrames_ = 0;
    std::int16_t last_ = 0;
    std::int32_t lastPeak_ = 0;
    bool wasSilent_ = true;
    std::uint32_t streamState_ = 0;
    std::uint32_t streamRestarts_ = 0;
    std::uint32_t restartFailures_ = 0;
    std::uint64_t pausedFrames_ = 0;
    std::uint64_t failedFrames_ = 0;
    bool streamReady_ = true;
};

inline bool audioNeedsQueuePacing(std::size_t pending)
{
    // Clock lag from a slow scene must not permit an unbounded catch-up burst.
    // Leave room for the next slice without blocking inside the PCM producer.
    return pending >= AudioChannel::kBlockCount - 2;
}

// 先読みを約33msに制限。速度不足を待機でさらに遅くせず、先行時だけ待つ。
inline bool audioNeedsPacing(std::uint64_t cycles, std::uint64_t elapsedUs)
{
    constexpr std::uint64_t lead = 327680;
    const auto guestUs = cycles / 10;
    return guestUs > elapsedUs && guestUs - elapsedUs > lead / 10;
}

// Core1 only. Pausing the guest must also pause its wall-clock reference; otherwise
// resume/reset can run without a lead limit for the entire previous pause/epoch.
class AudioPacer
{
public:
    bool needsPacing(std::uint64_t cycles, std::uint64_t nowUs, bool paused)
    {
        const bool newEpoch = !initialized_ || cycles < previousCycles_ || nowUs < previousUs_;
        if (newEpoch)
        {
            epochCycles_ = cycles;
            elapsedUs_ = 0;
            initialized_ = true;
        }
        else if (!wasPaused_)
        {
            elapsedUs_ += nowUs - previousUs_;
        }
        previousCycles_ = cycles;
        previousUs_ = nowUs;
        wasPaused_ = paused;
        return !paused && audioNeedsPacing(cycles - epochCycles_, elapsedUs_);
    }

private:
    std::uint64_t epochCycles_ = 0;
    std::uint64_t previousCycles_ = 0;
    std::uint64_t previousUs_ = 0;
    std::uint64_t elapsedUs_ = 0;
    bool initialized_ = false;
    bool wasPaused_ = false;
};
}  // namespace x68k_platform
#endif
