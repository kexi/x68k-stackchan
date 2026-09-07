// SPDX-License-Identifier: MIT
#include <algorithm>
#include <vector>

#include "doctest.h"
#include "machine.h"
#include "cpu/block_planner.h"
#include "cpu/move_timing.h"

TEST_CASE("MOVE/MOVEAの命令時間はMC68000UM表8-2/8-3の値になる")
{
    // Independent examples from the manual, not from the production timing helper.
    struct Example
    {
        x68k::u16 opcode;
        x68k::u32 cycles;
    };
    constexpr Example examples[] = {
        {0x1200, 4},  {0x3200, 4},  {0x2200, 4},  {0x1210, 8},  {0x1218, 8},  {0x1220, 10},
        {0x1228, 12}, {0x1230, 14}, {0x1238, 12}, {0x1239, 16}, {0x123a, 12}, {0x123b, 14},
        {0x123c, 8},  {0x2210, 12}, {0x2218, 12}, {0x2220, 14}, {0x2228, 16}, {0x2230, 18},
        {0x2238, 16}, {0x2239, 20}, {0x223a, 16}, {0x223b, 18}, {0x223c, 12}, {0x1280, 8},
        {0x12c0, 8},  {0x1300, 8},  {0x1340, 12}, {0x1380, 14}, {0x11c0, 12}, {0x13c0, 16},
        {0x2280, 12}, {0x22c0, 12}, {0x2300, 12}, {0x2340, 16}, {0x2380, 18}, {0x21c0, 16},
        {0x23c0, 20}, {0x327c, 8},  {0x227c, 12}, {0x3260, 10}, {0x2260, 14}, {0x13fc, 20},
        {0x23fc, 28}, {0x23f9, 36}, {0x23a0, 28}, {0x3318, 12}, {0x2318, 20}, {0x33a0, 20}};
    x68k::Machine machine;
    std::vector<x68k::u8> ram(x68k::kMainRamSize);
    x68k::MemoryMap memory{};
    memory.mainRam = ram.data();
    machine.setMemory(memory);
    const auto put = [&](x68k::u32 address, x68k::u16 word)
    {
        ram[address] = static_cast<x68k::u8>(word >> 8);
        ram[address + 1] = static_cast<x68k::u8>(word);
    };
    for (const auto& example : examples)
    {
        CAPTURE(example.opcode);
        std::fill(ram.begin(), ram.end(), 0);
        put(2, 0x8000);
        put(6, 0x1000);
        put(0x1000, example.opcode);
        machine.reset();
        machine.cpu().state().a[0] = 0x4000;
        machine.cpu().state().a[1] = 0x6000;
        const auto group = example.opcode >> 12;
        const x68k::u32 size = group == 1 ? 1u : group == 2 ? 4u : 2u;
        CHECK(x68k::moveInstructionCycles(example.opcode, size) == example.cycles);
        CHECK(machine.step() == example.cycles);
        CHECK_FALSE(machine.isHalted());
        x68k::PlannedOp plan{};
        const bool supported = x68k::BlockPlanner::planOne(example.opcode, 0x1000, plan);
        if (supported)
        {
            CHECK(plan.cycles == example.cycles);
        }
    }
}
