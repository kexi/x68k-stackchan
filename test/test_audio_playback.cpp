// SPDX-License-Identifier: MIT
#include "audio_playback.h"
#include "doctest.h"
#include <vector>
#include <limits>

namespace
{
struct Sink : x68k_platform::AudioSink
{
    std::vector<std::int16_t> samples;
    unsigned restarts = 0;
    unsigned writes = 0;
    bool canRestart = true;
    void write(const std::int16_t* data, std::size_t size) override
    {
        ++writes;
        samples.assign(data, data + size);
    }
    bool restartStream() override
    {
        ++restarts;
        samples.clear();
        return canRestart;
    }
    x68k::u32 sampleRate() const override
    {
        return 15625;
    }
};
void push(x68k_platform::AudioChannel& channel, std::int16_t value)
{
    auto* block = channel.writeBlock();
    REQUIRE(block != nullptr);
    std::fill(block, block + 512, value);
    channel.commit();
}
}  // namespace

TEST_CASE("供給不足は512sample単位で計数し、端をfadeして無音へ落とす")
{
    x68k_platform::AudioChannel channel;
    Sink sink;
    x68k_platform::AudioPlayback output(channel, sink);
    output.submit(false);
    CHECK(output.missingFrames() == 512);
    CHECK(output.lastPeak() == 0);
    push(channel, 32000);
    output.submit(false);
    CHECK(output.sourceFrames() == 512);
    CHECK(channel.pending() == 0);
    CHECK(sink.samples[0] == 500);
    CHECK(sink.samples[63] == 32000);
    CHECK(sink.samples.back() == 32000);
    output.submit(false);
    CHECK(output.missingFrames() == 1024);
    CHECK(sink.samples[0] == 31500);
    CHECK(sink.samples[63] == 0);
    CHECK(sink.samples.back() == 0);
    output.submit(false);
    CHECK(output.lastPeak() == 0);
    CHECK(output.missingFrames() == 1536);
}

TEST_CASE("連続PCMは変更せず、mute中もringを消費して復帰をfadeする")
{
    x68k_platform::AudioChannel channel;
    Sink sink;
    x68k_platform::AudioPlayback output(channel, sink);
    push(channel, -32768);
    output.submit(false);
    CHECK(sink.samples[0] == -512);
    push(channel, 32767);
    output.submit(false);
    CHECK(std::all_of(sink.samples.begin(), sink.samples.end(), [](auto v) { return v == 32767; }));
    push(channel, 10000);
    output.submit(true);
    CHECK(output.mutedFrames() == 512);
    CHECK(output.sourceFrames() == 1536);
    CHECK(output.missingFrames() == 0);
    CHECK(channel.pending() == 0);
    CHECK(sink.samples.back() == 0);
    push(channel, -32000);
    output.submit(false);
    CHECK(sink.samples[0] == -500);
    CHECK(sink.samples[63] == -32000);
}

TEST_CASE("pacingは約33ms以上の先行だけを抑え、遅延や64bit時刻を壊さない")
{
    using x68k_platform::audioNeedsPacing;
    CHECK_FALSE(audioNeedsPacing(0, 0));
    CHECK_FALSE(audioNeedsPacing(327680, 0));
    CHECK(audioNeedsPacing(327690, 0));
    CHECK_FALSE(audioNeedsPacing(10000000, 2000000));
    CHECK_FALSE(audioNeedsPacing(10000000, std::numeric_limits<std::uint64_t>::max()));
    CHECK(audioNeedsPacing(std::numeric_limits<std::uint64_t>::max(), 0));
}

TEST_CASE("pauseの壁時計をゲスト先行枠へ加算せずreset時にepochを張り直す")
{
    x68k_platform::AudioPacer pacer;
    CHECK_FALSE(pacer.needsPacing(0, 9000000, false));
    CHECK_FALSE(pacer.needsPacing(10000000, 10000000, false));
    CHECK_FALSE(pacer.needsPacing(10000000, 10000000, true));
    CHECK_FALSE(pacer.needsPacing(10000000, 36010000000ULL, true));
    CHECK_FALSE(pacer.needsPacing(10000000, 36010000000ULL, false));
    CHECK_FALSE(pacer.needsPacing(10327680, 36010000000ULL, false));
    CHECK(pacer.needsPacing(10327690, 36010000000ULL, false));
    // An output-pacing wait is not a user pause and must let wall time catch up.
    CHECK_FALSE(pacer.needsPacing(10327690, 36010040000ULL, false));
    CHECK_FALSE(pacer.needsPacing(0, 36010040000ULL, false));
    CHECK(pacer.needsPacing(327690, 36010040000ULL, false));
    // Restored nonzero clocks also have an independent epoch.
    CHECK_FALSE(pacer.needsPacing(100, 36010040001ULL, false));
    CHECK(pacer.needsPacing(327790, 36010040001ULL, false));
}

TEST_CASE("epochをまたぐ読取leaseは生産側から取り消さず解放後に旧PCMだけを捨てる")
{
    x68k_platform::AudioChannel channel;
    push(channel, 100);
    push(channel, 200);
    push(channel, 300);
    const auto* leased = channel.readBlock();
    REQUIRE(leased != nullptr);
    channel.restartStream(false);
    CHECK(channel.readBlock() == leased);
    CHECK(leased[0] == 100);
    CHECK(channel.writeBlock() == nullptr);
    channel.releaseRead();
    // A new-epoch commit can follow the discarded prefix before the consumer wakes.
    push(channel, 400);
    const auto* fresh = channel.readBlock();
    REQUIRE(fresh != nullptr);
    CHECK(fresh[0] == 400);
    channel.releaseRead();
    CHECK(channel.readBlock() == nullptr);
    for (unsigned epoch = 0; epoch < 100; ++epoch)
    {
        push(channel, 500);
        channel.restartStream((epoch & 1u) != 0);
        CHECK(channel.readBlock() == nullptr);
        CHECK(channel.pending() == 0);
    }
}

TEST_CASE("pauseは不足として数えず再開とresetでは古いPCMとfade履歴を鳴らさない")
{
    x68k_platform::AudioChannel channel;
    Sink sink;
    x68k_platform::AudioPlayback output(channel, sink);
    push(channel, 32000);
    output.submit(false);
    push(channel, 16000);
    channel.restartStream(true);
    output.submit(false);
    CHECK(output.pausedFrames() == 512);
    CHECK(output.missingFrames() == 0);
    CHECK(output.lastPeak() == 0);
    CHECK(channel.pending() == 0);
    CHECK(sink.restarts == 1);
    channel.restartStream(false);
    push(channel, -32000);
    output.submit(false);
    CHECK(sink.samples[0] == -500);
    CHECK(sink.samples[63] == -32000);
    CHECK(output.sourceFrames() == 1024);
    CHECK(output.streamRestarts() == 2);
    push(channel, 20000);
    channel.restartStream(false);
    output.submit(false);
    CHECK(output.lastPeak() == 0);
    CHECK(output.missingFrames() == 512);
    CHECK(output.sourceFrames() == 1024);
}

TEST_CASE("sink再初期化失敗中は書込を止め次のepochで復旧できる")
{
    x68k_platform::AudioChannel channel;
    Sink sink;
    x68k_platform::AudioPlayback output(channel, sink);
    sink.canRestart = false;
    channel.restartStream(false);
    push(channel, 32000);
    output.submit(false);
    CHECK(sink.writes == 0);
    CHECK(output.failedFrames() == 512);
    CHECK(output.restartFailures() == 1);
    output.submit(false);
    CHECK(sink.restarts == 1);
    CHECK(output.failedFrames() == 1024);
    sink.canRestart = true;
    channel.restartStream(false);
    push(channel, 16000);
    output.submit(false);
    CHECK(sink.writes == 1);
    CHECK(sink.samples[0] == 250);
    CHECK(output.restartFailures() == 1);
}
