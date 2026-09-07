// SPDX-License-Identifier: MIT
#include <algorithm>
#include <vector>

#include "doctest.h"
#include "machine.h"
#include "cpu/block_planner.h"
#include "cpu/immediate_timing.h"

TEST_CASE("即値六演算とquick二演算は幅と全data-alterable EAの仕様時間を返す")
{
    constexpr x68k::u16 operations[] = {0x0000, 0x0200, 0x0400, 0x0600,
                                        0x0a00, 0x0c00, 0x5600, 0x5700};
    constexpr x68k::u16 forms[] = {0, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x39};
    constexpr x68k::u32 eaByteWord[] = {0, 4, 4, 6, 8, 10, 8, 12};
    constexpr x68k::u32 registerLong[] = {16, 14, 16, 16, 16, 14, 8, 8};
    constexpr x68k::u32 memoryLongBase[] = {20, 20, 20, 20, 20, 12, 12, 12};
    constexpr x68k::u32 memoryShortBase[] = {12, 12, 12, 12, 12, 8, 8, 8};
    constexpr x68k::u32 result[] = {11, 2, 7, 13, 9, 10, 13, 7};
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
    for (unsigned operation = 0; operation < 8; ++operation)
    {
        for (unsigned width = 0; width < 3; ++width)
        {
            for (unsigned form = 0; form < 8; ++form)
            {
                const x68k::u32 size = 1u << width;
                const bool longOperand = width == 2;
                const bool quick = operation >= 6;
                const bool memoryTarget = form != 0;
                const auto opcode =
                    static_cast<x68k::u16>(operations[operation] | (width << 6) | forms[form]);
                CAPTURE(opcode);
                std::fill(ram.begin(), ram.end(), 0);
                put(2, 0x8000);
                put(6, 0x1000);
                put(0x1000, opcode);
                x68k::u32 next = 0x1002;
                if (!quick)
                {
                    if (longOperand)
                    {
                        put(next, 0);
                        next += 2;
                    }
                    put(next, 3);
                    next += 2;
                }
                const x68k::u16 extensions[] = {0, 0, 0, 0, 0x20, 0x1020, 0x4020, 0};
                const bool extended = form >= 4;
                if (extended)
                {
                    put(next, extensions[form]);
                    next += 2;
                    const bool absoluteLong = form == 7;
                    if (absoluteLong)
                    {
                        put(next, 0x4020);
                        next += 2;
                    }
                }
                const x68k::u32 targets[] = {0,      0x4000, 0x4000, 0x4000 - size,
                                             0x4020, 0x4024, 0x4020, 0x4020};
                const auto target = targets[form];
                if (memoryTarget)
                {
                    ram[target - 1] = 0x5a;
                    ram[target + size - 1] = 10;
                    ram[target + size] = 0xa5;
                }
                machine.reset();
                auto& state = machine.cpu().state();
                const auto initial = longOperand ? 10u : 0x1234000au;
                state.d[0] = initial;
                state.d[1] = 4;
                state.a[0] = 0x4000;
                state.sr = 0x2710;
                const auto registerCycles = longOperand ? registerLong[operation] : quick ? 4u : 8u;
                const auto memoryBase =
                    longOperand ? memoryLongBase[operation] : memoryShortBase[operation];
                const auto cycles = memoryTarget
                                        ? memoryBase + eaByteWord[form] + (longOperand ? 4u : 0u)
                                        : registerCycles;
                CHECK((quick ? x68k::quickInstructionCycles(opcode, size)
                             : x68k::immediateInstructionCycles(opcode, size)) == cycles);
                CHECK(machine.step() == cycles);
                CHECK_FALSE(machine.isHalted());
                CHECK(state.pc == next + 4);
                CHECK(state.a[7] == 0x8000);
                CHECK(state.a[0] == (form == 2 ? 0x4000 + size : form == 3 ? target : 0x4000));
                const bool changesExtend = operation == 2 || operation == 3 || quick;
                CHECK(state.sr == (changesExtend ? 0x2700 : 0x2710));
                if (memoryTarget)
                {
                    x68k::u32 actual = 0;
                    for (x68k::u32 byte = 0; byte < size; ++byte)
                    {
                        actual = (actual << 8) | ram[target + byte];
                    }
                    CHECK(actual == result[operation]);
                    CHECK(ram[target - 1] == 0x5a);
                    CHECK(ram[target + size] == 0xa5);
                    CHECK(state.d[0] == initial);
                }
                else
                {
                    CHECK(state.d[0] ==
                          (longOperand ? result[operation] : 0x12340000u | result[operation]));
                }
                x68k::PlannedOp planned{};
                const bool supported = x68k::BlockPlanner::planOne(opcode, 0x1000, planned);
                if (supported)
                {
                    CHECK(planned.cycles == cycles);
                }
            }
        }
    }
}

TEST_CASE("ADDQ/SUBQのAn宛てはwordとlongでも32bit更新しSRを変えない")
{
    const x68k::u16 opcodes[] = {0x5648, 0x5688, 0x5748, 0x5788};
    const x68k::u32 cycles[] = {4, 8, 8, 8};
    x68k::Machine machine;
    std::vector<x68k::u8> ram(x68k::kMainRamSize);
    x68k::MemoryMap memory{};
    memory.mainRam = ram.data();
    machine.setMemory(memory);
    ram[2] = 0x80;
    ram[6] = 0x10;
    for (unsigned i = 0; i < 4; ++i)
    {
        CAPTURE(i);
        ram[0x1000] = static_cast<x68k::u8>(opcodes[i] >> 8);
        ram[0x1001] = static_cast<x68k::u8>(opcodes[i]);
        machine.reset();
        auto& state = machine.cpu().state();
        state.a[0] = 0x1234ffff;
        state.sr = 0x271f;
        CHECK(machine.step() == cycles[i]);
        CHECK(state.a[0] == (i < 2 ? 0x12350002u : 0x1234fffcu));
        CHECK(state.sr == 0x271f);
        x68k::PlannedOp planned{};
        REQUIRE(x68k::BlockPlanner::planOne(opcodes[i], 0x1000, planned));
        CHECK(planned.cycles == cycles[i]);
    }
}
