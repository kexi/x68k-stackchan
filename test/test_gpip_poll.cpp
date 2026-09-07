// SPDX-License-Identifier: MIT
#include <array>
#include <vector>
#include "doctest.h"
#include "machine.h"
#include "cpu/gpip_poll_loop.h"

namespace
{
struct PollFixture
{
    x68k::Machine machine;
    std::vector<x68k::u8> ram = std::vector<x68k::u8>(x68k::kMainRamSize);
    void put(unsigned at, x68k::u16 value)
    {
        ram[at] = static_cast<x68k::u8>(value >> 8);
        ram[at + 1] = static_cast<x68k::u8>(value);
    }
    void setup(bool event, bool timerRead, unsigned switches)
    {
        put(2, 0x8000);
        put(6, 0x400);
        // Every read contributes to a rotating hash, rather than only checking the last GPIP.
        std::vector<x68k::u16> code{0x1039, 0x00e8, 0x8001, 0xd280, 0xe399};
        if (timerRead)
        {
            code.insert(code.end(), {0x1439, 0x00e8, 0x8021, 0xd282});
        }
        const auto length = static_cast<unsigned>(code.size()) * 2u + 2u;
        code.push_back(static_cast<x68k::u16>(0x6000u | ((0u - length) & 0xffu)));
        unsigned at = 0x400;
        for (const auto word : code)
        {
            put(at, word);
            at += 2;
        }
        x68k::MemoryMap memory{};
        memory.mainRam = ram.data();
        machine.setMemory(memory);
        machine.setEventDriven(event);
        machine.reset();
        x68k::PerfSwitch perf{};
        perf.inlineMfpTimer = (switches & 1u) != 0;
        perf.inlineRtcTick = (switches & 2u) != 0;
        perf.inlineCrtcTick = (switches & 4u) != 0;
        machine.setPerfSwitch(perf);
        machine.mfp().write(x68k::Mfp::kIera, 0);
        machine.mfp().write(x68k::Mfp::kIerb, 0);
        machine.mfp().write(x68k::Mfp::kTbdr, 251);
        machine.mfp().write(x68k::Mfp::kTbcr, 1);
        machine.mfp().tickFast<true>(
            6);  // An unaligned timer phase must not disappear in batching.
    }
};
}  // namespace

TEST_CASE("GPIP待機の一括実行はslice・CRTC両edge・timer・外部wakeを跨いでも通常実行と一致する")
{
    for (unsigned switches = 0; switches < 8; ++switches)
    {
        for (unsigned span : {53u, 105u, 9973u, 60000u, 180342u})
        {
            PollFixture reference, candidate;
            std::vector<std::int16_t> referencePcm, candidatePcm;
            for (auto* fixture : {&reference, &candidate})
            {
                fixture->setup(true, false, switches);
                const x68k::u16 code[] = {0x223c, 0,      18000,  0x1039, 0x00e8, 0x8001,
                                          0x0800, 4,      0x6604, 0x5381, 0x66f0, 0x223c,
                                          0,      18000,  0x1039, 0x00e8, 0x8001, 0x0800,
                                          4,      0x6704, 0x5381, 0x66f0, 0x60d2, 0x4e71};
                unsigned at = 0x400;
                for (auto word : code)
                {
                    fixture->put(at, word);
                    at += 2;
                }
                fixture->machine.bus().setRomMappedAtZero(false);
                fixture->machine.cpu().refillPrefetchForTest(0x400);
                if (const bool withIrq = (switches & 1u) != 0; withIrq)
                {
                    // Record both the interrupted PC and saved SR without touching D0/D1.
                    const x68k::u16 handler[] = {0x5283, 0x282f, 2,      0xda84, 0x3c17, 0xde86,
                                                 0x13fc, 0x00df, 0x00e8, 0x800d, 0x4e73};
                    unsigned address = 0x900;
                    for (auto word : handler)
                    {
                        fixture->put(address, word);
                        address += 2;
                    }
                    fixture->put(0x45 * 4, 0);
                    fixture->put(0x45 * 4 + 2, 0x900);
                    auto& machine = fixture->machine;
                    machine.cpu().state().sr = 0x2000;
                    machine.mfp().write(x68k::Mfp::kVr, 0x40);
                    machine.mfp().write(x68k::Mfp::kTcdr, 200);
                    machine.mfp().write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
                    machine.mfp().write(x68k::Mfp::kImrb, x68k::Mfp::kIntTimerC);
                    machine.mfp().write(x68k::Mfp::kTcdcr, 0x70);
                }
            }
            candidate.machine.setGpipPollAcceleration(true);
            for (auto* fixture : {&reference, &candidate})
            {
                auto* pcm = fixture == &reference ? &referencePcm : &candidatePcm;
                fixture->machine.adpcm().writeCommand(2);
                for (unsigned byte = 0; byte < 256; ++byte)
                {
                    fixture->machine.adpcm().writeData(static_cast<x68k::u8>(byte));
                }
                fixture->machine.setAudioSyncCallback(
                    pcm,
                    [](void* context, x68k::Machine& machine, std::uint64_t cycles,
                       x68k::Machine::AudioSyncPoint point)
                    {
                        auto& output = *static_cast<std::vector<std::int16_t>*>(context);
                        const bool reset = point == x68k::Machine::AudioSyncPoint::kReset;
                        if (reset)
                        {
                            output.clear();
                            return;
                        }
                        const auto old = output.size();
                        const auto target = static_cast<std::size_t>(cycles / 640);
                        REQUIRE(target >= old);
                        output.resize(target);
                        const bool hasSamples = target > old;
                        if (hasSamples)
                            machine.renderAudio(output.data() + old, target - old);
                    });
            }
            for (unsigned slice = 0; slice < 24; ++slice)
            {
                if (const bool wake = slice == 8 || slice == 17; wake)
                {
                    const auto key = static_cast<x68k::u8>(slice == 8 ? 0x20 : 0xa0);
                    reference.machine.pressKey(key);
                    candidate.machine.pressKey(key);
                }
                CHECK(candidate.machine.run(span) == reference.machine.run(span));
                const auto& a = reference.machine.cpu().state();
                const auto& b = candidate.machine.cpu().state();
                CHECK(std::equal(std::begin(a.d), std::end(a.d), std::begin(b.d)));
                CHECK(std::equal(std::begin(a.a), std::end(a.a), std::begin(b.a)));
                CHECK(a.pc == b.pc);
                CHECK(a.sr == b.sr);
                CHECK(a.ir == b.ir);
                CHECK(a.irc == b.irc);
                CHECK(std::equal(reference.ram.begin() + 0x7ff0, reference.ram.begin() + 0x8000,
                                 candidate.ram.begin() + 0x7ff0));
                CHECK(reference.machine.audioGuestCycles() == candidate.machine.audioGuestCycles());
                CHECK(referencePcm == candidatePcm);
                CHECK(reference.machine.crtc().rasterNumber() ==
                      candidate.machine.crtc().rasterNumber());
                CHECK(reference.machine.crtc().inVerticalBlank() ==
                      candidate.machine.crtc().inVerticalBlank());
                CHECK(reference.machine.mfp().read(x68k::Mfp::kTbdr) ==
                      candidate.machine.mfp().read(x68k::Mfp::kTbdr));
                CHECK(reference.machine.cpu().pendingInterruptLevel() ==
                      candidate.machine.cpu().pendingInterruptLevel());
            }
            if (const bool enoughTime = span >= 9973; enoughTime)
            {
                CHECK(candidate.machine.gpipPollSkippedCycles() > 0);
            }
            CHECK(reference.machine.gpipPollSkippedCycles() == 0);
            if (const bool irqExpected = (switches & 1u) != 0 && span >= 9973; irqExpected)
            {
                CHECK(candidate.machine.cpu().state().d[3] > 0);
            }
            candidate.machine.reset();
            CHECK(candidate.machine.gpipPollSkippedCycles() == 0);
        }
    }
}

TEST_CASE("GPIPポーリングの全読取hashは毎命令tickと一致しtimer読取・外部wakeも保つ")
{
    for (const bool timerRead : {false, true})
    {
        for (unsigned switches = 0; switches < 8; ++switches)
        {
            for (const unsigned span : {9973u, 60000u, 180342u})
            {
                PollFixture reference;
                PollFixture candidate;
                reference.setup(false, timerRead, switches);
                candidate.setup(true, timerRead, switches);
                for (unsigned slice = 0; slice < 24; ++slice)
                {
                    const bool externalWake = slice == 8 || slice == 17;
                    if (externalWake)
                    {
                        const auto key = static_cast<x68k::u8>(slice == 8 ? 0x20 : 0xa0);
                        reference.machine.pressKey(key);
                        candidate.machine.pressKey(key);
                    }
                    const auto expected = reference.machine.run(span);
                    CHECK(candidate.machine.run(span) == expected);
                    const auto& before = reference.machine.cpu().state();
                    const auto& after = candidate.machine.cpu().state();
                    CHECK_FALSE(candidate.machine.isHalted());
                    CHECK(after.pc == before.pc);
                    CHECK(after.sr == before.sr);
                    CHECK(after.d[0] == before.d[0]);
                    CHECK(after.d[1] == before.d[1]);
                    CHECK(after.d[2] == before.d[2]);
                    CHECK(candidate.machine.mfp().read(x68k::Mfp::kTbdr) ==
                          reference.machine.mfp().read(x68k::Mfp::kTbdr));
                    CHECK(candidate.machine.crtc().rasterNumber() ==
                          reference.machine.crtc().rasterNumber());
                    CHECK(candidate.machine.crtc().inVerticalBlank() ==
                          reference.machine.crtc().inVerticalBlank());
                }
                CHECK(candidate.machine.cpu().state().d[1] != 0);
            }
        }
    }
}
