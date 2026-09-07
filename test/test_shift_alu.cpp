// SPDX-License-Identifier: MIT
#include <array>
#include <vector>
#include "doctest.h"
#include "cpu/shift_alu.h"
#include "machine.h"

namespace
{
// Deliberately serial model: one bit, carry and sign transition at a time.
x68k::alu::ShiftResult reference(x68k::u32 input, unsigned size, unsigned count, unsigned type,
                                 bool left, bool extend)
{
    const auto mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
    const auto sign = 1u << (size * 8u - 1u);
    x68k::alu::ShiftResult out{input & mask, type == 2 && extend, extend, false};
    for (unsigned bit = 0; bit < count; ++bit)
    {
        const auto before = out.value;
        out.carry = left ? (before & sign) != 0 : (before & 1u) != 0;
        const bool incoming = type == 3   ? out.carry
                              : type == 2 ? out.extend
                                          : type == 0 && !left && (before & sign) != 0;
        out.value = left ? ((before << 1u) | (incoming ? 1u : 0u)) & mask
                         : (before >> 1u) | (incoming ? sign : 0u);
        out.overflow |= type == 0 && left && ((before ^ out.value) & sign) != 0;
        const bool changesExtend = type != 3;
        if (changesExtend)
        {
            out.extend = out.carry;
        }
    }
    return out;
}

void checkShift(x68k::u32 value, unsigned size, unsigned count, unsigned type, bool left, bool x)
{
    const auto expected = reference(value, size, count, type, left, x);
    const auto actual = x68k::alu::shift(value, size, count, type, left, x);
    const bool same = actual.value == expected.value && actual.carry == expected.carry &&
                      actual.extend == expected.extend && actual.overflow == expected.overflow;
    INFO("value=" << value << " size=" << size << " count=" << count << " type=" << type
                  << " left=" << left << " X=" << x);
    REQUIRE(same);
}
}  // namespace

TEST_CASE("定数時間shiftは全byte値・全count・全種別・Xと逐次モデルが一致する")
{
    for (unsigned value = 0; value < 256; ++value)
    {
        for (unsigned count = 0; count < 64; ++count)
        {
            for (unsigned variant = 0; variant < 16; ++variant)
            {
                checkShift(value, 1, count, variant & 3u, (variant & 4u) != 0, (variant & 8u) != 0);
            }
        }
    }
}

TEST_CASE("wordとlongの境界bitと固定seed値は全count・種別・Xで一致する")
{
    x68k::u32 seed = 0x7193abd1u;
    for (const unsigned size : {2u, 4u})
    {
        for (unsigned sample = 0; sample < 256; ++sample)
        {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const auto value = sample < 32   ? 1u << sample
                               : sample < 64 ? ~(1u << (sample - 32u))
                                             : seed;
            for (unsigned count = 0; count < 64; ++count)
            {
                for (unsigned variant = 0; variant < 16; ++variant)
                {
                    checkShift(value, size, count, variant & 3u, (variant & 4u) != 0,
                               (variant & 8u) != 0);
                }
            }
        }
    }
}

TEST_CASE("shift命令は即値8・レジスタcountのmod64と上位レジスタ・SR・時間を守る")
{
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
    put(2, 0x8000);
    put(6, 0x1000);
    machine.reset();
    auto& cpu = machine.cpu();
    auto& state = cpu.state();
    unsigned cases = 0;
    for (unsigned width = 0; width < 3; ++width)
    {
        const unsigned size = 1u << width;
        const auto mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
        for (unsigned variant = 0; variant < 16; ++variant)
        {
            for (unsigned countEncoding = 0; countEncoding < 136; ++countEncoding)
            {
                const bool registerCount = countEncoding >= 8;
                const auto field = registerCount ? 1u : countEncoding;
                const auto count = registerCount        ? (countEncoding - 8u) & 63u
                                   : countEncoding == 0 ? 8u
                                                        : countEncoding;
                const auto type = variant & 3u;
                const bool left = (variant & 4u) != 0;
                const bool x = (variant & 8u) != 0;
                const auto opcode = static_cast<x68k::u16>(
                    0xe000u | (field << 9) | (left ? 0x100u : 0u) | (width << 6) |
                    (registerCount ? 0x20u : 0u) | (type << 3));
                put(0x1000, opcode);
                cpu.refillPrefetchForTest(0x1000);
                state.d[0] = 0xa5817e93;
                state.d[1] = countEncoding - 8u;
                state.sr = static_cast<x68k::u16>(0x270fu | (x ? 0x10u : 0u));
                const auto expected = reference(state.d[0], size, count, type, left, x);
                const auto expectedSr =
                    0x2700u | (expected.extend ? 0x10u : 0u) |
                    ((expected.value & (1u << (size * 8u - 1u))) != 0 ? 8u : 0u) |
                    (expected.value == 0 ? 4u : 0u) | (expected.overflow ? 2u : 0u) |
                    (expected.carry ? 1u : 0u);
                CHECK(cpu.step() == (size == 4 ? 8u : 6u) + 2u * count);
                CHECK(state.d[0] == ((0xa5817e93u & ~mask) | expected.value));
                CHECK(state.d[1] == countEncoding - 8u);
                CHECK(state.sr == expectedSr);
                CHECK(state.pc == 0x1006);
                CHECK(state.a[7] == 0x8000);
                ++cases;
            }
        }
    }
    CHECK(cases == 6528);
}
