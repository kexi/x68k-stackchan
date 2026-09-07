// SPDX-License-Identifier: MIT
#include <algorithm>
#include <vector>

#include "doctest.h"
#include "machine.h"
#include "cpu/block_planner.h"
#include "cpu/control_timing.h"

TEST_CASE("LEA/PEA/JMP/JSRの全制御EAは仕様表の命令時間と副作用を保つ")
{
    // MC68000UM table 8-10, independently tabulated expected cycles.
    constexpr x68k::u16 forms[] = {0x10, 0x28, 0x30, 0x38, 0x39, 0x3a, 0x3b};
    constexpr x68k::u16 bases[] = {0x43c0, 0x4840, 0x4ec0, 0x4e80};
    constexpr x68k::u32 expected[4][7] = {{4, 8, 12, 8, 12, 8, 12},
                                          {12, 16, 20, 16, 20, 16, 20},
                                          {8, 10, 14, 10, 12, 10, 14},
                                          {16, 18, 22, 18, 20, 18, 22}};
    x68k::Machine machine;
    std::vector<x68k::u8> ram(x68k::kMainRamSize);
    x68k::MemoryMap memory{};
    memory.mainRam = ram.data();
    machine.setMemory(memory);
    const auto put = [&](x68k::u32 at, x68k::u16 word)
    {
        ram[at] = static_cast<x68k::u8>(word >> 8);
        ram[at + 1] = static_cast<x68k::u8>(word);
    };
    for (unsigned instruction = 0; instruction < 4; ++instruction)
    {
        for (unsigned form = 0; form < 7; ++form)
        {
            const auto opcode = static_cast<x68k::u16>(bases[instruction] | forms[form]);
            CAPTURE(opcode);
            std::fill(ram.begin(), ram.end(), 0);
            put(2, 0x8000);
            put(6, 0x1000);
            put(0x1000, opcode);
            // Nonzero target addresses and displacement/index extension words.
            const x68k::u32 targets[] = {0x4000, 0x4020, 0x4024, 0x4020, 0x4020, 0x1022, 0x1026};
            const x68k::u16 extensions[] = {0, 0x20, 0x20, 0x4020, 0, 0x20, 0x20};
            put(0x1002, extensions[form]);
            const bool absoluteLong = form == 4;
            if (absoluteLong)
            {
                put(0x1004, 0x4020);
            }
            machine.reset();
            auto& state = machine.cpu().state();
            state.a[0] = 0x4000;
            state.d[0] = 4;
            state.sr = 0x271f;
            CHECK(x68k::controlInstructionCycles(opcode) == expected[instruction][form]);
            CHECK(machine.step() == expected[instruction][form]);
            CHECK_FALSE(machine.isHalted());
            CHECK(state.sr == 0x271f);
            const x68k::u32 next = form == 0 ? 0x1002u : absoluteLong ? 0x1006u : 0x1004u;
            const bool transfersControl = instruction >= 2;
            CHECK(state.pc == (transfersControl ? targets[form] : next) + 4);
            const bool pushesStack = instruction == 1 || instruction == 3;
            CHECK(state.a[7] == (pushesStack ? 0x7ffcu : 0x8000u));
            if (pushesStack)
            {
                const auto pushed = (static_cast<x68k::u32>(ram[0x7ffc]) << 24) |
                                    (static_cast<x68k::u32>(ram[0x7ffd]) << 16) |
                                    (static_cast<x68k::u32>(ram[0x7ffe]) << 8) | ram[0x7fff];
                CHECK(pushed == (instruction == 3 ? next : targets[form]));
            }
            const bool isLea = instruction == 0;
            if (isLea)
            {
                CHECK(state.a[1] == targets[form]);
            }
            x68k::PlannedOp planned{};
            const bool supported = x68k::BlockPlanner::planOne(opcode, 0x1000, planned);
            if (supported)
            {
                CHECK(planned.cycles == expected[instruction][form]);
            }
        }
    }
}
