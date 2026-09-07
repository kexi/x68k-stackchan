// SPDX-License-Identifier: MIT
#include <algorithm>
#include <vector>
#include "doctest.h"
#include "machine.h"
#include "cpu/block_planner.h"

TEST_CASE("BTSTはstatic/dynamicとEAの時間を区別しZ以外と読取対象を保存する")
{
    constexpr x68k::u16 forms[] = {0, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x39};
    constexpr unsigned eaCycles[] = {0, 4, 4, 6, 8, 10, 8, 12};
    x68k::Machine machine;
    std::vector<x68k::u8> ram(x68k::kMainRamSize);
    x68k::MemoryMap memory{};
    memory.mainRam = ram.data();
    machine.setMemory(memory);
    const auto put = [&](unsigned at, x68k::u16 value)
    {
        ram[at] = static_cast<x68k::u8>(value >> 8);
        ram[at + 1] = static_cast<x68k::u8>(value);
    };
    for (bool dynamic : {false, true})
    {
        for (unsigned form = 0; form < 8; ++form)
        {
            for (unsigned bit : {0u, 4u, 31u, 255u})
            {
                const auto opcode =
                    static_cast<x68k::u16>((dynamic ? 0x0500 : 0x0800) | forms[form]);
                CAPTURE(opcode);
                CAPTURE(bit);
                std::fill(ram.begin(), ram.end(), 0);
                put(2, 0x8000);
                put(6, 0x1000);
                put(0x1000, opcode);
                unsigned next = 0x1002;
                if (!dynamic)
                {
                    put(next, static_cast<x68k::u16>(bit));
                    next += 2;
                }
                const bool extension = form >= 4;
                if (extension)
                {
                    constexpr x68k::u16 extensions[] = {0, 0, 0, 0, 0x20, 0x1020, 0x4020, 0};
                    put(next, extensions[form]);
                    next += 2;
                    const bool absoluteLong = form == 7;
                    if (absoluteLong)
                    {
                        put(next, 0x4020);
                        next += 2;
                    }
                }
                constexpr unsigned targets[] = {0,      0x4000, 0x4000, 0x3fff,
                                                0x4020, 0x4024, 0x4020, 0x4020};
                const auto target = targets[form];
                const bool memoryEa = form != 0;
                if (memoryEa)
                {
                    ram[target] = 0x10;
                }
                machine.reset();
                auto& state = machine.cpu().state();
                state.d[0] = 0x80000010;
                state.d[1] = 4;
                state.d[2] = bit;
                state.a[0] = 0x4000;
                state.sr = 0x271b;
                const auto cycles = (memoryEa ? 4u : 6u) + (dynamic ? 0u : 4u) + eaCycles[form];
                CHECK(machine.step() == cycles);
                const auto value = memoryEa ? 0x10u : 0x80000010u;
                const auto mask = 1u << (bit & (memoryEa ? 7u : 31u));
                CHECK(state.sr == (0x271bu | ((value & mask) == 0 ? 4u : 0u)));
                CHECK(state.pc == next + 4);
                CHECK(state.d[0] == 0x80000010u);
                CHECK(state.d[1] == 4);
                CHECK(state.d[2] == bit);
                CHECK(state.a[0] == (form == 2 ? 0x4001 : form == 3 ? 0x3fff : 0x4000));
                CHECK(state.a[7] == 0x8000);
                if (memoryEa)
                {
                    CHECK(ram[target] == 0x10);
                }
                x68k::PlannedOp plan{};
                const bool planned = x68k::BlockPlanner::planOne(opcode, 0x1000, plan);
                if (planned)
                {
                    CHECK(plan.cycles == cycles);
                }
            }
        }
    }
}
