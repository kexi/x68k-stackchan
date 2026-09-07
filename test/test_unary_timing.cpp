// SPDX-License-Identifier: MIT
#include <algorithm>
#include <vector>
#include "doctest.h"
#include "machine.h"
#include "cpu/block_planner.h"
#include "cpu/operand_timing.h"

TEST_CASE("単項演算とメモリshiftは合法EAの仕様時間と結果・副作用を守る")
{
    constexpr x68k::u16 bases[] = {0x4000, 0x4200, 0x4400, 0x4600, 0x4a00, 0xe0c0, 0xe1c0,
                                   0xe2c0, 0xe3c0, 0xe4c0, 0xe5c0, 0xe6c0, 0xe7c0};
    constexpr x68k::u16 forms[] = {0, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x39};
    constexpr unsigned eaCycles[] = {0, 4, 4, 6, 8, 10, 8, 12};
    constexpr x68k::u32 values[] = {0xfffffffau, 0,  0xfffffffbu, 0xfffffffau, 5,      2, 10,
                                    2,           10, 0x8002,      11,          0x8002, 10};
    constexpr x68k::u16 flags[] = {0x19, 0x14, 0x19, 0x18, 0x10, 0x11, 0,
                                   0x11, 0,    0x19, 0,    0x19, 0x10};
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
    unsigned cases = 0;
    for (unsigned operation = 0; operation < 13; ++operation)
    {
        const bool shift = operation >= 5;
        for (unsigned width = 0; width < 3; ++width)
        {
            const bool nonWordShift = shift && width != 1;
            if (nonWordShift)
            {
                continue;
            }
            const unsigned size = 1u << width;
            const auto mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
            for (unsigned form = shift ? 1u : 0u; form < 8; ++form)
            {
                const auto opcode = static_cast<x68k::u16>(bases[operation] | forms[form] |
                                                           (shift ? 0u : width << 6));
                CAPTURE(opcode);
                std::fill(ram.begin(), ram.end(), 0);
                put(2, 0x8000);
                put(6, 0x1000);
                put(0x1000, opcode);
                unsigned next = 0x1002;
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
                const unsigned targets[] = {0,      0x4000, 0x4000, 0x4000 - size,
                                            0x4020, 0x4024, 0x4020, 0x4020};
                const auto target = targets[form];
                const bool memoryEa = form != 0;
                if (memoryEa)
                {
                    ram[target - 1] = 0x5a;
                    ram[target + size - 1] = 5;
                    ram[target + size] = 0xa5;
                }
                machine.reset();
                auto& state = machine.cpu().state();
                state.d[0] = 0xa5810000u | 5;
                // Word/long register tests use exactly the same low operand as memory tests.
                state.d[0] = (state.d[0] & ~mask) | 5;
                state.d[1] = 4;
                state.a[0] = 0x4000;
                state.sr = 0x2710;
                const auto ea = eaCycles[form] + (size == 4 && memoryEa ? 4u : 0u);
                const auto base = shift            ? 8u
                                  : operation == 4 ? 4u
                                  : memoryEa       ? (size == 4 ? 12u : 8u)
                                  : size == 4      ? 6u
                                                   : 4u;
                CHECK(machine.step() == base + ea);
                CHECK_FALSE(machine.isHalted());
                CHECK(state.pc == next + 4);
                CHECK(state.sr == (0x2700u | flags[operation]));
                CHECK(state.a[0] == (form == 2 ? 0x4000 + size : form == 3 ? target : 0x4000));
                CHECK(state.a[7] == 0x8000);
                CHECK(state.d[1] == 4);
                const auto result = values[operation] & mask;
                if (memoryEa)
                {
                    x68k::u32 actual = 0;
                    for (unsigned byte = 0; byte < size; ++byte)
                    {
                        actual = (actual << 8) | ram[target + byte];
                    }
                    CHECK(actual == result);
                    CHECK(ram[target - 1] == 0x5a);
                    CHECK(ram[target + size] == 0xa5);
                    CHECK(state.d[0] == ((0xa5810000u & ~mask) | 5));
                }
                else
                {
                    CHECK(state.d[0] == ((0xa5810000u & ~mask) | result));
                }
                x68k::PlannedOp plan{};
                const bool planned = x68k::BlockPlanner::planOne(opcode, 0x1000, plan);
                if (planned)
                {
                    CHECK(plan.cycles == base + ea);
                }
                ++cases;
            }
        }
    }
    CHECK(cases == 176);
}
