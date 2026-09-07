// SPDX-License-Identifier: MIT
#include <algorithm>
#include <vector>

#include "doctest.h"
#include "machine.h"
#include "cpu/scc_timing.h"

TEST_CASE("Sccは真偽と全合法EAに応じた時間でbyteだけを書きSRを保つ")
{
    constexpr x68k::u16 forms[] = {0x00, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x39};
    constexpr x68k::u32 memoryCycles[] = {0, 12, 12, 14, 16, 18, 16, 20};
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
    for (unsigned variant = 0; variant < 4; ++variant)
    {
        for (unsigned form = 0; form < 8; ++form)
        {
            // ST, SF, SEQ with Z clear, SEQ with Z set.
            const x68k::u16 conditions[] = {0, 1, 7, 7};
            const bool condition = variant == 0 || variant == 3;
            const auto opcode =
                static_cast<x68k::u16>(0x50c0 | (conditions[variant] << 8) | forms[form]);
            CAPTURE(opcode);
            CAPTURE(variant);
            std::fill(ram.begin(), ram.end(), 0);
            put(2, 0x8000);
            put(6, 0x1000);
            put(0x1000, opcode);
            const x68k::u16 extensions[] = {0, 0, 0, 0, 0x20, 0x20, 0x4020, 0};
            put(0x1002, extensions[form]);
            put(0x1004, 0x4020);
            machine.reset();
            auto& state = machine.cpu().state();
            state.a[0] = 0x4000;
            state.d[0] = 0x12340004;
            const auto sr = static_cast<x68k::u16>(variant == 3 ? 0x271f : 0x271b);
            state.sr = sr;
            const x68k::u32 targets[] = {0, 0x4000, 0x4000, 0x3fff, 0x4020, 0x4024, 0x4020, 0x4020};
            const auto target = targets[form];
            const bool memoryTarget = form != 0;
            if (memoryTarget)
            {
                ram[target - 1] = 0x5a;
                ram[target] = 0x42;
                ram[target + 1] = 0xa5;
            }
            const auto cycles = memoryTarget ? memoryCycles[form] : condition ? 6u : 4u;
            CHECK(x68k::sccInstructionCycles(opcode, condition) == cycles);
            CHECK(machine.step() == cycles);
            CHECK_FALSE(machine.isHalted());
            CHECK(state.sr == sr);
            CHECK(state.a[7] == 0x8000);
            CHECK(state.a[0] == (form == 2 ? 0x4001u : form == 3 ? 0x3fffu : 0x4000u));
            CHECK(state.pc == (form < 4 ? 0x1006u : form == 7 ? 0x100au : 0x1008u));
            const auto value = condition ? 0xffu : 0u;
            if (memoryTarget)
            {
                CHECK(ram[target] == value);
                CHECK(ram[target - 1] == 0x5a);
                CHECK(ram[target + 1] == 0xa5);
                CHECK(state.d[0] == 0x12340004);
            }
            else
            {
                CHECK(state.d[0] == (0x12340000u | value));
            }
        }
    }
}
