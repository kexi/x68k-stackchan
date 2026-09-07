// SPDX-License-Identifier: MIT
#include <array>
#include <algorithm>
#include <limits>
#include "doctest.h"
#include "cpu/gpip_poll_loop.h"

namespace
{
struct PollBus : x68k::Bus
{
    std::array<x68k::u8, 1024> ram{};
    x68k::u8 gpip = 0;
    x68k::u16 read16(x68k::u32 address) override
    {
        return static_cast<x68k::u16>((read8(address) << 8) | read8(address + 1));
    }
    x68k::u8 read8(x68k::u32 address) override
    {
        const bool io = address == 0xe88001;
        if (io)
            return gpip;
        REQUIRE(address < ram.size());
        return ram[address];
    }
    void write16(x68k::u32 address, x68k::u16 value) override
    {
        write8(address, static_cast<x68k::u8>(value >> 8));
        write8(address + 1, static_cast<x68k::u8>(value));
    }
    void write8(x68k::u32 address, x68k::u8 value) override
    {
        REQUIRE(address < ram.size());
        ram[address] = value;
    }
};
struct Fixture
{
    PollBus bus;
    x68k::M68k cpu{bus};
    void setup(x68k::u16 branch, x68k::u32 count = 18000, x68k::u32 pc = 0x100)
    {
        const x68k::u16 code[] = {0x1039, 0x00e8, 0x8001, 0x0800, 4,
                                  branch, 0x5381, 0x66f0, 0x4e71, 0x4e71};
        for (unsigned i = 0; i < 10; ++i)
            bus.write16(pc + i * 2, code[i]);
        cpu.setFastRam(bus.ram.data(), static_cast<x68k::u32>(bus.ram.size()));
        cpu.setFastRamReadable(true);
        cpu.refillPrefetchForTest(pc);
        auto& state = cpu.state();
        state.sr = 0x271f;
        state.d[0] = 0xabcd1234;
        state.d[1] = count;
        state.a[7] = 0x300;
        bus.gpip = branch == 0x6604 ? 0xef : 0xff;
    }
};
void equalState(const x68k::M68kState& a, const x68k::M68kState& b)
{
    CHECK(std::equal(std::begin(a.d), std::end(a.d), std::begin(b.d)));
    CHECK(std::equal(std::begin(a.a), std::end(a.a), std::begin(b.a)));
    CHECK(a.pc == b.pc);
    CHECK(a.sr == b.sr);
    CHECK(a.ir == b.ir);
    CHECK(a.irc == b.irc);
    CHECK(a.usp == b.usp);
    CHECK(a.ssp == b.ssp);
    CHECK(a.halted == b.halted);
    CHECK(a.stopped == b.stopped);
}
}  // namespace

TEST_CASE("GPIP待機の完全周回は通常の5命令実行とレジスタ・CCR・prefetchまで一致する")
{
    for (const x68k::u16 branch : {x68k::u16{0x6604}, x68k::u16{0x6704}})
    {
        for (unsigned deadline = 1; deadline <= 520; ++deadline)
        {
            for (const unsigned count : {0u, 1u, 2u, 3u, 18000u, 18001u})
            {
                Fixture expected, actual;
                expected.setup(branch, count);
                actual.setup(branch, count);
                const auto used = x68k::tryGpipPollLoop(actual.cpu, actual.bus.gpip,
                                                        -static_cast<std::int32_t>(deadline));
                const unsigned loops =
                    count >= 2 && count <= 18000 ? std::min((deadline - 1) / 52, count - 1) : 0;
                CHECK(used == (loops >= 2 ? loops * 52 : 0));
                unsigned spent = 0;
                while (spent < used)
                    spent += expected.cpu.step();
                CHECK(spent == used);
                equalState(expected.cpu.state(), actual.cpu.state());
            }
        }
    }
}

TEST_CASE("GPIP待機は退出・IRQ・trace・命令変更・先読み窓外では状態を変えずfallbackする")
{
    for (unsigned fault = 0; fault < 17; ++fault)
    {
        Fixture fixture;
        fixture.setup(0x6604, 18000, 0x1f8);
        if (fault < 8)
            fixture.bus.ram[0x1f8 + fault * 2 + 1] ^= 1;
        else if (fault == 8)
            fixture.cpu.state().irc ^= 1;
        else if (fault == 9)
            fixture.bus.gpip ^= 0x10;
        else if (fault == 10)
            fixture.cpu.requestInterrupt(6);
        else if (fault == 11)
            fixture.cpu.state().sr |= 0x8000;
        else if (fault == 12)
            fixture.cpu.state().halted = true;
        else if (fault == 13)
            fixture.cpu.state().stopped = true;
        else if (fault == 14)
            fixture.cpu.setFastRamReadable(false);
        else if (fault == 15)
            fixture.cpu.setFastRam(fixture.bus.ram.data(), 0x1f8 + 18);
        const auto before = fixture.cpu.state();
        CHECK(x68k::tryGpipPollLoop(fixture.cpu, fixture.bus.gpip, fault == 16 ? 0 : -1000) == 0);
        equalState(before, fixture.cpu.state());
    }
    Fixture relocated;
    relocated.setup(0x6704, 18000, 0x1f8);
    CHECK(x68k::tryGpipPollLoop(relocated.cpu, relocated.bus.gpip, -1000) == 988);
    Fixture longest;
    longest.setup(0x6604);
    CHECK(x68k::tryGpipPollLoop(longest.cpu, longest.bus.gpip,
                                std::numeric_limits<std::int32_t>::min()) == 17999u * 52);
    CHECK(longest.cpu.state().d[1] == 1);
}
