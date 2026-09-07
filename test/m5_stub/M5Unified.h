#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#include "doctest.h"

struct TestSpeaker
{
    struct Config
    {
        unsigned sample_rate = 0;
        bool stereo = false;
        unsigned task_pinned_core = 0;
        unsigned task_priority = 0;
        int pin_data_out = 1, pin_bck = 2, pin_ws = 3;
    } settings;
    struct Retained
    {
        const std::int16_t* pointer;
        std::vector<std::int16_t> expected;
    };
    std::deque<Retained> retained;
    unsigned volume = 255;
    bool reject = false;
    bool enabled = true;
    bool running = true;
    unsigned playCalls = 0;
    Config config() const
    {
        return settings;
    }
    void config(Config value)
    {
        settings = value;
    }
    bool begin()
    {
        return true;
    }
    bool isEnabled() const
    {
        return enabled;
    }
    bool isRunning() const
    {
        return running;
    }
    void setVolume(unsigned value)
    {
        volume = value;
    }
    std::size_t getPlayingChannels() const
    {
        return retained.empty() ? 0 : 1;
    }
    void end()
    {
        retained.clear();
    }
    void verify() const
    {
        for (const auto& block : retained)
        {
            for (std::size_t i = 0; i < block.expected.size(); ++i)
            {
                CHECK(block.pointer[i] == block.expected[i]);
            }
        }
    }
    bool playRaw(const std::int16_t* samples, std::size_t frames, unsigned rate, bool stereo,
                 unsigned repeat, int channel, bool stop)
    {
        ++playCalls;
        verify();
        // opm.h の kDefaultSampleRate と同じ値。stub なので include せず直書きする。
        CHECK(rate == 62500);
        CHECK_FALSE(stereo);
        CHECK(repeat == 1);
        CHECK(channel == 0);
        CHECK_FALSE(stop);
        if (reject)
        {
            return false;
        }
        const bool isFull = retained.size() == 2;
        if (isFull)
        {
            retained.pop_front();
        }
        retained.push_back({samples, {samples, samples + frames}});
        return true;
    }
};
inline struct TestM5
{
    TestSpeaker Speaker;
} M5;
