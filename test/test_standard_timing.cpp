// SPDX-License-Identifier: MIT
#include <algorithm>
#include <vector>

#include "doctest.h"
#include "machine.h"
#include "cpu/block_planner.h"
#include "cpu/standard_timing.h"

TEST_CASE("通常ALUは合法EAと幅の仕様時間で実行し結果と周辺状態を保つ")
{
    // Independent table 8-1/8-4 expectations, including the long register/immediate footnote.
    constexpr x68k::u16 bases[] = {0xd400, 0x9400, 0xc400, 0x8400, 0xb400, 0xd500, 0x9500,
                                   0xc500, 0x8500, 0xb500, 0xd4c0, 0x94c0, 0xb4c0};
    constexpr x68k::u16 forms[] = {0,    8,    0x10, 0x18, 0x20, 0x28,
                                   0x30, 0x38, 0x39, 0x3a, 0x3b, 0x3c};
    constexpr x68k::u32 eaShort[] = {0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4};
    constexpr x68k::u32 results[] = {13, 7, 2, 11, 10, 13, 7, 2, 11, 9, 13, 7, 10};
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
    unsigned cases = 0;
    for (unsigned operation = 0; operation < 13; ++operation)
    {
        const bool addressTarget = operation >= 10;
        const bool writesEa = operation >= 5 && operation < 10;
        const bool compare = operation == 4 || operation == 12;
        for (unsigned width = 0; width < 3; ++width)
        {
            const bool invalidAddressByte = addressTarget && width == 0;
            if (invalidAddressByte)
            {
                continue;
            }
            const x68k::u32 size = 1u << width;
            const bool longOperand = width == 2;
            for (unsigned form = 0; form < 12; ++form)
            {
                const bool invalidWriteEa =
                    writesEa && (form == 1 || form >= 9 || (form == 0 && operation != 9));
                const bool invalidReadAn =
                    !writesEa && form == 1 && (width == 0 || operation == 2 || operation == 3);
                if (invalidWriteEa || invalidReadAn)
                {
                    continue;
                }
                ++cases;
                const auto widthBits = addressTarget ? (longOperand ? 0x100u : 0u) : width << 6;
                const auto opcode =
                    static_cast<x68k::u16>(bases[operation] | widthBits | forms[form]);
                CAPTURE(opcode);
                std::fill(ram.begin(), ram.end(), 0);
                put(2, 0x8000);
                put(6, 0x1000);
                put(0x1000, opcode);
                x68k::u32 next = 0x1002;
                const bool extended = form >= 5;
                if (extended)
                {
                    const x68k::u16 extension[] = {0,      0,      0, 0,    0,      0x20,
                                                   0x1020, 0x4020, 0, 0x20, 0x1020, 3};
                    const bool immediateLong = form == 11 && longOperand;
                    put(next, immediateLong ? 0 : extension[form]);
                    next += 2;
                    const bool extraWord = form == 8 || immediateLong;
                    if (extraWord)
                    {
                        put(next, immediateLong ? 3 : 0x4020);
                        next += 2;
                    }
                }
                const x68k::u32 targets[] = {0,      0,      0x4000, 0x4000, 0x4000 - size, 0x4020,
                                             0x4024, 0x4020, 0x4020, 0x1022, 0x1026,        0};
                const bool memoryEa = form >= 2 && form <= 10;
                const auto target = targets[form];
                if (memoryEa)
                {
                    ram[target - 1] = 0x5a;
                    ram[target + size - 1] = writesEa ? 10 : 3;
                    ram[target + size] = 0xa5;
                }
                machine.reset();
                auto& state = machine.cpu().state();
                const bool eorRegister = writesEa && form == 0;
                state.d[0] = eorRegister ? 10 : 3;
                state.d[1] = 4;
                state.d[2] = writesEa ? 3 : 10;
                state.a[0] = form == 1 ? 3 : 0x4000;
                state.a[2] = 10;
                state.sr = 0x2710;
                const auto eaCycles = eaShort[form] + (longOperand && form >= 2 ? 4u : 0u);
                const bool shortInternal = form <= 1 || form == 11;
                const x68k::u32 base = addressTarget ? (compare       ? 6u
                                                        : longOperand ? (shortInternal ? 8u : 6u)
                                                                      : 8u)
                                       : writesEa    ? (eorRegister   ? (longOperand ? 8u : 4u)
                                                        : longOperand ? 12u
                                                                      : 8u)
                                       : longOperand ? (compare         ? 6u
                                                        : shortInternal ? 8u
                                                                        : 6u)
                                                     : 4u;
                const auto cycles = base + eaCycles;
                CHECK(x68k::standardInstructionCycles(opcode, size) == cycles);
                CHECK(machine.step() == cycles);
                CHECK_FALSE(machine.isHalted());
                CHECK(state.pc == next + 4);
                CHECK(state.a[7] == 0x8000);
                CHECK(state.a[0] == (form == 1   ? 3u
                                     : form == 3 ? 0x4000 + size
                                     : form == 4 ? target
                                                 : 0x4000));
                const bool changesExtend =
                    operation == 0 || operation == 1 || operation == 5 || operation == 6;
                CHECK(state.sr == (changesExtend ? 0x2700 : 0x2710));
                CHECK(state.a[2] == (addressTarget ? results[operation] : 10u));
                CHECK(state.d[2] == (writesEa ? 3u : addressTarget ? 10u : results[operation]));
                CHECK(state.d[0] == (eorRegister ? results[operation] : 3u));
                if (memoryEa)
                {
                    x68k::u32 actual = 0;
                    for (x68k::u32 byte = 0; byte < size; ++byte)
                    {
                        actual = (actual << 8) | ram[target + byte];
                    }
                    CHECK(actual == (writesEa ? results[operation] : 3u));
                    CHECK(ram[target - 1] == 0x5a);
                    CHECK(ram[target + size] == 0xa5);
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
    CHECK(cases == 351);
}
