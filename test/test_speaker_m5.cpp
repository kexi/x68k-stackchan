#include "speaker_m5.h"
#include "doctest.h"
#include <algorithm>
#include <array>

TEST_CASE("再生投入の計測は初回を空欠と数えず受付拒否と再開を区別する")
{
    M5.Speaker = TestSpeaker{};
    x68k_platform::M5SpeakerSink sink;
    REQUIRE(sink.begin());
    std::array<std::int16_t, x68k_platform::AudioChannel::kBlockFrames> samples{};
    sink.write(samples.data(), samples.size());
    CHECK(sink.submissionStats().accepted == 1);
    CHECK(sink.submissionStats().emptyBeforeSubmit == 0);
    sink.write(samples.data(), samples.size());
    CHECK(sink.submissionStats().emptyBeforeSubmit == 0);
    M5.Speaker.retained.clear();
    M5.Speaker.reject = true;
    sink.write(samples.data(), samples.size());
    CHECK(sink.submissionStats().accepted == 2);
    CHECK(sink.submissionStats().rejected == 1);
    CHECK(sink.submissionStats().emptyBeforeSubmit == 0);
    M5.Speaker.reject = false;
    sink.write(samples.data(), samples.size());
    CHECK(sink.submissionStats().accepted == 3);
    CHECK(sink.submissionStats().emptyBeforeSubmit == 1);
    sink.write(samples.data(), 0);
    CHECK(sink.submissionStats().rejected == 2);
    sink.end();
    CHECK(sink.submissionStats().accepted == 0);
    CHECK(sink.submissionStats().rejected == 0);
    CHECK(sink.submissionStats().emptyBeforeSubmit == 0);
    REQUIRE(sink.begin());
    sink.write(samples.data(), samples.size());
    CHECK(sink.submissionStats().emptyBeforeSubmit == 0);
    sink.end();
}

TEST_CASE("PCM保持領域が確保できなければスピーカー初期化は失敗する")
{
    testHeapFailure = true;
    x68k_platform::M5SpeakerSink sink;
    CHECK_FALSE(sink.begin());
    testHeapFailure = false;
    CHECK(sink.begin());
    sink.end();
    CHECK(sink.begin());
    sink.end();
}

TEST_CASE("出力タスク停止をplayRawの空成功とせず保持中PCMを変更しない")
{
    M5.Speaker = TestSpeaker{};
    x68k_platform::M5SpeakerSink sink;
    REQUIRE(sink.begin());
    std::array<std::int16_t, x68k_platform::AudioChannel::kBlockFrames> samples{};
    samples.fill(1234);
    sink.write(samples.data(), samples.size());
    sink.write(samples.data(), samples.size());
    REQUIRE(M5.Speaker.playCalls == 2);
    M5.Speaker.running = false;
    samples.fill(-32768);
    sink.write(samples.data(), samples.size());
    CHECK(sink.submissionStats().rejected == 1);
    CHECK(sink.submissionStats().accepted == 2);
    CHECK(M5.Speaker.playCalls == 2);
    M5.Speaker.verify();
    M5.Speaker.running = true;
    M5.Speaker.enabled = false;
    sink.write(samples.data(), samples.size());
    CHECK(sink.submissionStats().rejected == 2);
    CHECK(M5.Speaker.playCalls == 2);
    M5.Speaker.verify();
    sink.end();
    M5.Speaker = TestSpeaker{};
}

TEST_CASE("M5出力は呼び出し元の再利用と受付失敗でも再生中の2枚を保持する")
{
    M5.Speaker = TestSpeaker{};
    x68k_platform::M5SpeakerSink sink;
    REQUIRE(sink.begin());
    CHECK(M5.Speaker.volume == 40);
    std::array<std::int16_t, x68k_platform::AudioChannel::kBlockFrames> samples{};
    for (std::int16_t n = 1; n < 20; ++n)
    {
        M5.Speaker.reject = n % 4 == 0;
        std::fill(samples.begin(), samples.end(), n);
        sink.write(samples.data(), samples.size());
        std::fill(samples.begin(), samples.end(), -1);
        M5.Speaker.verify();
    }
    sink.end();
}

TEST_CASE("stream再初期化はM5の参照を解放してからPCMを再利用し累積統計を保つ")
{
    M5.Speaker = TestSpeaker{};
    x68k_platform::M5SpeakerSink sink;
    REQUIRE(sink.begin());
    std::array<std::int16_t, x68k_platform::AudioChannel::kBlockFrames> samples{};
    samples.fill(1234);
    sink.write(samples.data(), samples.size());
    sink.write(samples.data(), samples.size());
    REQUIRE(M5.Speaker.retained.size() == 2);
    REQUIRE(sink.restartStream());
    CHECK(M5.Speaker.retained.empty());
    CHECK(sink.submissionStats().accepted == 2);
    CHECK(M5.Speaker.volume == 40);
    samples.fill(-1234);
    sink.write(samples.data(), samples.size());
    M5.Speaker.verify();
    CHECK(sink.submissionStats().accepted == 3);
    CHECK(sink.submissionStats().emptyBeforeSubmit == 0);
    sink.end();
}
