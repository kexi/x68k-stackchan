// SPDX-License-Identifier: MIT
#include <algorithm>
#include <tuple>
#include <vector>

#include "doctest.h"
#include "machine.h"
#include "guest_audio.h"
#include "audio_playback.h"

namespace
{
using Event = std::tuple<std::uint64_t, bool, std::size_t>;
struct AudioClockMachine
{
    x68k::Machine machine;
    std::vector<x68k::u8> ram = std::vector<x68k::u8>(x68k::kMainRamSize);
    std::vector<Event> accesses;
    std::vector<std::int16_t> pcm;
    std::uint64_t samples = 0;
    unsigned resets = 0;

    AudioClockMachine(bool eventDriven, bool shadow = false)
    {
        put(0, 0);
        put(2, 0x8000);
        put(4, 0);
        put(6, 0x1000);
        // NOP、OPM address/data、ADPCM start/data/status、STOP。
        const x68k::u16 program[] = {0x4e71, 0x13fc, 8,      0x00e9, 1,      0x13fc, 0x78,   0x00e9,
                                     3,      0x4e71, 0x13fc, 2,      0x00e9, 0x2001, 0x13fc, 0x71,
                                     0x00e9, 0x2003, 0x4a39, 0x00e9, 0x2001, 0x4e72, 0x2700};
        x68k::u32 address = 0x1000;
        for (auto word : program)
        {
            put(address, word);
            address += 2;
        }
        x68k::MemoryMap memory{};
        memory.mainRam = ram.data();
        machine.setMemory(memory);
        machine.setEventDriven(eventDriven);
        machine.setShadowVerify(shadow);
        machine.setAudioSyncCallback(
            this,
            [](void* context, x68k::Machine& source, std::uint64_t cycles,
               x68k::Machine::AudioSyncPoint point)
            {
                auto& self = *static_cast<AudioClockMachine*>(context);
                const bool reset = point == x68k::Machine::AudioSyncPoint::kReset;
                if (reset)
                {
                    ++self.resets;
                    self.samples = 0;
                    self.pcm.clear();
                    self.accesses.clear();
                    return;
                }
                const auto target = cycles / x68k_platform::GuestAudioProducer::kCyclesPerSample;
                REQUIRE(target >= self.samples);
                const auto offset = self.pcm.size();
                self.pcm.resize(static_cast<std::size_t>(target));
                const bool hasSamples = target != self.samples;
                if (hasSamples)
                {
                    source.renderAudio(self.pcm.data() + offset,
                                       static_cast<std::size_t>(target - self.samples));
                }
                self.samples = target;
                const bool access = point == x68k::Machine::AudioSyncPoint::kAccess;
                if (access)
                {
                    self.accesses.emplace_back(cycles, source.adpcm().isPlaying(),
                                               source.adpcm().fifoCount());
                }
            });
        machine.reset();
    }

    void put(x68k::u32 address, x68k::u16 value)
    {
        ram[address] = static_cast<x68k::u8>(value >> 8);
        ram[address + 1] = static_cast<x68k::u8>(value);
    }
    void run(x68k::u32 slice, x68k::u32 total = 60000)
    {
        x68k::u32 spent = 0;
        while (spent < total)
        {
            const auto used = machine.run(std::min(slice, total - spent));
            REQUIRE(used != 0);
            spent += used;
        }
        CHECK(machine.audioGuestCycles() == spent);
    }
};
}  // namespace

TEST_CASE("音源MMIO直前の命令境界時刻とPCMは通常/イベント/影照合のslice分割で一致する")
{
    AudioClockMachine reference(false);
    reference.run(4);
    REQUIRE(reference.accesses.size() == 4);
    // NOP(4) + MOVE.B #imm,abs.l(20)。通知は次の命令の開始境界。
    CHECK(std::get<0>(reference.accesses[0]) == 24);
    CHECK_FALSE(std::get<1>(reference.accesses[1]));
    CHECK(std::get<2>(reference.accesses[2]) == 0);
    CHECK(std::get<2>(reference.accesses[3]) == 1);
    for (bool event : {false, true})
    {
        for (auto slice : {4u, 64u, 1024u, 60000u})
        {
            AudioClockMachine candidate(event);
            candidate.run(slice);
            CHECK(candidate.accesses == reference.accesses);
            CHECK(candidate.pcm == reference.pcm);
        }
    }
    AudioClockMachine shadow(false, true);
    shadow.run(64);
    CHECK(shadow.accesses == reference.accesses);
    CHECK(shadow.pcm == reference.pcm);
}

TEST_CASE("ゲスト時刻生産はMMIO境界のPCMを端数保持して512sampleずつ公開する")
{
    AudioClockMachine reference(false);
    // 1024 sample (= 2 ブロック) ぶん回す。AudioChannel のリングは 4 枚しか
    // 無いので、公開待ちを 3 枚以上作るとこの検証の前提が崩れる。
    // サンプル数がレートに依らず 1024 になるよう、cycles 側を追従させる。
    constexpr std::uint64_t kCps = x68k_platform::GuestAudioProducer::kCyclesPerSample;
    constexpr std::uint64_t kRunCycles = 1024 * kCps;
    reference.run(4, static_cast<x68k::u32>(kRunCycles));
    REQUIRE(reference.pcm.size() == 1024);
    CHECK(std::any_of(reference.pcm.begin(), reference.pcm.end(), [](auto v) { return v != 0; }));
    AudioClockMachine candidate(true);
    x68k_platform::AudioChannel channel;
    x68k_platform::GuestAudioProducer producer(channel);
    candidate.machine.setAudioSyncCallback(
        &producer,
        [](void* context, x68k::Machine& machine, std::uint64_t cycles,
           x68k::Machine::AudioSyncPoint point)
        {
            auto& output = *static_cast<x68k_platform::GuestAudioProducer*>(context);
            const bool reset = point == x68k::Machine::AudioSyncPoint::kReset;
            if (reset)
            {
                output.reset();
                return;
            }
            CHECK(output.syncTo(machine, cycles));
        });
    candidate.run(60000, static_cast<x68k::u32>(kRunCycles));
    CHECK(producer.samples() == 1024);
    CHECK(producer.partialFrames() == 0);
    CHECK(producer.publishedBlocks() == 2);
    std::vector<std::int16_t> actual;
    while (const auto* block = channel.readBlock())
    {
        actual.insert(actual.end(), block, block + 512);
        channel.releaseRead();
    }
    CHECK(actual == reference.pcm);
    // 次のサンプル境界の手前までは増えず、境界を跨ぐと端数が1つ出る。
    CHECK(producer.syncTo(candidate.machine, kRunCycles + kCps - 1));
    CHECK(producer.samples() == 1024);
    CHECK(producer.syncTo(candidate.machine, kRunCycles + kCps));
    CHECK(producer.partialFrames() == 1);
    CHECK_FALSE(producer.syncTo(candidate.machine, 0));
    CHECK(producer.clockErrors() == 1);
    candidate.machine.reset();
    CHECK(producer.samples() == 0);
    CHECK(producer.partialFrames() == 0);
    CHECK(producer.discardedOnReset() == 1);
}

TEST_CASE("低速区間後の追いつきでもキュー待機が端数511sampleからの連続生産を溢れさせない")
{
    x68k::Machine machine;
    x68k_platform::AudioChannel channel;
    x68k_platform::GuestAudioProducer producer(channel);
    REQUIRE(producer.syncTo(machine, 1023u * x68k_platform::GuestAudioProducer::kCyclesPerSample));
    REQUIRE(channel.pending() == 1);
    REQUIRE(producer.partialFrames() == 511);
    std::uint64_t cycles = 1023u * x68k_platform::GuestAudioProducer::kCyclesPerSample;
    unsigned waits = 0;
    for (unsigned turn = 0; turn < 1000; ++turn)
    {
        const bool wait = x68k_platform::audioNeedsQueuePacing(channel.pending());
        if (wait)
            ++waits;
        else
        {
            // Normal 60000-cycle slice plus a deliberately generous instruction overshoot.
            cycles += 65536;
            REQUIRE(producer.syncTo(machine, cycles));
        }
        CHECK(channel.pending() <= 2);
        const bool consume = turn % 40 == 39;
        if (consume && channel.readBlock() != nullptr)
            channel.releaseRead();
    }
    CHECK(waits > 0);
    CHECK(channel.droppedBlocks() == 0);
    CHECK(producer.droppedSamples() == 0);
    CHECK_FALSE(x68k_platform::audioNeedsQueuePacing(0));
    CHECK_FALSE(x68k_platform::audioNeedsQueuePacing(1));
    CHECK(x68k_platform::audioNeedsQueuePacing(2));
    CHECK(x68k_platform::audioNeedsQueuePacing(3));
}

TEST_CASE("ring満杯でもゲスト音源を進めて破棄sample数を明示する")
{
    x68k::Machine machine;
    x68k::Machine reference;
    for (auto* source : {&machine, &reference})
    {
        source->adpcm().writeCommand(2);
        for (unsigned i = 0; i < 256; ++i)
        {
            source->adpcm().writeData(static_cast<x68k::u8>(i));
        }
    }
    x68k_platform::AudioChannel channel;
    x68k_platform::GuestAudioProducer producer(channel);
    CHECK(producer.syncTo(machine, 2048u * x68k_platform::GuestAudioProducer::kCyclesPerSample));
    CHECK(channel.pending() == 3);
    CHECK(producer.samples() == 2048);
    CHECK(producer.droppedSamples() == 512);
    CHECK(channel.droppedBlocks() == 1);
    std::vector<std::int16_t> expected(2048);
    reference.renderAudio(expected.data(), expected.size());
    CHECK(machine.adpcm().fifoCount() == reference.adpcm().fifoCount());
    CHECK(machine.adpcm().signalLevel() == reference.adpcm().signalLevel());
    CHECK(machine.adpcm().stepIndex() == reference.adpcm().stepIndex());
    for (unsigned block = 0; block < 3; ++block)
    {
        const auto* data = channel.readBlock();
        REQUIRE(data != nullptr);
        CHECK(std::equal(data, data + 512, expected.begin() + block * 512));
        channel.releaseRead();
    }
    CHECK(producer.syncTo(machine, 2560u * x68k_platform::GuestAudioProducer::kCyclesPerSample));
    reference.renderAudio(expected.data(), 512);
    REQUIRE(channel.readBlock() != nullptr);
    CHECK(std::equal(channel.readBlock(), channel.readBlock() + 512, expected.begin()));
}

TEST_CASE("一時停止は公開済みと端数PCMを破棄し再開後も音源時刻を巻き戻さない")
{
    x68k::Machine machine;
    x68k::Machine reference;
    for (auto* source : {&machine, &reference})
    {
        source->adpcm().writeCommand(2);
        for (unsigned i = 0; i < 256; ++i)
        {
            source->adpcm().writeData(static_cast<x68k::u8>(i));
        }
    }
    x68k_platform::AudioChannel channel;
    x68k_platform::GuestAudioProducer producer(channel);
    REQUIRE(producer.syncTo(machine, 600u * x68k_platform::GuestAudioProducer::kCyclesPerSample));
    CHECK(channel.pending() == 1);
    CHECK(producer.partialFrames() == 88);
    producer.setPaused(true);
    CHECK(producer.samples() == 600);
    CHECK(producer.partialFrames() == 0);
    CHECK(producer.discardedOnPause() == 88);
    CHECK(channel.readBlock() == nullptr);
    const auto pausedState = channel.streamState();
    producer.setPaused(true);
    CHECK(channel.streamState() == pausedState);
    producer.setPaused(false);
    REQUIRE(producer.syncTo(machine, 1112u * x68k_platform::GuestAudioProducer::kCyclesPerSample));
    std::vector<std::int16_t> expected(1112);
    reference.renderAudio(expected.data(), expected.size());
    const auto* block = channel.readBlock();
    REQUIRE(block != nullptr);
    CHECK(std::equal(block, block + 512, expected.begin() + 600));
    channel.releaseRead();
    REQUIRE(producer.syncTo(machine, 1624u * x68k_platform::GuestAudioProducer::kCyclesPerSample));
    CHECK(channel.pending() == 1);
    producer.reset();
    CHECK(producer.samples() == 0);
    CHECK(channel.readBlock() == nullptr);
}

TEST_CASE("stepと実行モード切替でも音声時刻は連続しresetは新epochを通知する")
{
    AudioClockMachine scene(false);
    CHECK(scene.machine.step() == 4);
    CHECK(scene.machine.audioGuestCycles() == 4);
    scene.machine.setEventDriven(true);
    scene.machine.run(1024);
    const auto before = scene.machine.audioGuestCycles();
    scene.machine.setEventDriven(false);
    const auto used = scene.machine.run(64);
    CHECK(scene.machine.audioGuestCycles() == before + used);
    REQUIRE(scene.accesses.size() == 4);
    CHECK(std::get<0>(scene.accesses[0]) == 24);
    scene.machine.reset();
    CHECK(scene.machine.audioGuestCycles() == 0);
    CHECK(scene.resets == 2);
    CHECK(scene.pcm.empty());
    scene.machine.setEventDriven(true);
    scene.machine.run(1024);
    CHECK(std::get<0>(scene.accesses[0]) == 24);
}

TEST_CASE("単一slice内のFM key-on/off間の波形を最終レジスタ値だけで失わない")
{
    auto prepare = [](AudioClockMachine& fixture)
    {
        x68k::u32 at = 0x1000;
        auto emit = [&](x68k::u16 word)
        {
            fixture.put(at, word);
            at += 2;
        };
        auto write = [&](x68k::u16 value, x68k::u16 offset)
        {
            emit(0x13fc);
            emit(value);
            emit(0x00e9);
            emit(offset);
        };
        write(8, 1);
        write(0x78, 3);
        for (unsigned i = 0; i < 4096; ++i)
        {
            emit(0x4e71);
        }
        write(8, 1);
        write(0, 3);
        emit(0x4e72);
        emit(0x2700);
        fixture.machine.reset();
        auto& opm = fixture.machine.opm();
        auto reg = [&](x68k::u8 address, x68k::u8 value)
        {
            opm.writeAddress(address);
            opm.writeData(value);
        };
        for (x68k::u8 slot = 0; slot < 4; ++slot)
        {
            const auto offset = static_cast<x68k::u8>(slot * 8);
            reg(static_cast<x68k::u8>(0x40 + offset), 1);
            reg(static_cast<x68k::u8>(0x60 + offset), 0);
            reg(static_cast<x68k::u8>(0x80 + offset), 0x1f);
            reg(static_cast<x68k::u8>(0xa0 + offset), 0);
            reg(static_cast<x68k::u8>(0xc0 + offset), 0);
            reg(static_cast<x68k::u8>(0xe0 + offset), 0x0f);
        }
        reg(0x20, 0xc7);
        reg(0x28, 0x4a);
    };
    AudioClockMachine reference(false);
    AudioClockMachine candidate(true);
    prepare(reference);
    prepare(candidate);
    reference.run(4);
    candidate.run(60000);
    REQUIRE(candidate.accesses.size() == 2);
    CHECK(candidate.accesses == reference.accesses);
    CHECK(candidate.pcm == reference.pcm);
    CHECK(std::any_of(candidate.pcm.begin(), candidate.pcm.begin() + 25,
                      [](auto sample) { return sample != 0; }));
}
