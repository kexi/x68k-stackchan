#include "doctest.h"
#include "key_queue.h"

TEST_CASE("ASCII keys retain automatic release while raw scan holds until explicit release")
{
    x68k_platform::KeyQueue keys;
    x68k::Machine machine;
    REQUIRE(keys.begin());
    keys.push('d');
    keys.drain(machine);
    CHECK(machine.ioRead8(x68k::kMfpBase + 0x2F) == 0x20);
    for (int i = 0; i <= keys.kStepsPerEvent; ++i)
    {
        keys.drain(machine);
    }
    CHECK(machine.ioRead8(x68k::kMfpBase + 0x2F) == 0xA0);
    for (int i = 0; i <= keys.kStepsPerEvent; ++i)
    {
        keys.drain(machine);
    }
    REQUIRE(keys.pushScan(0x20));
    keys.drain(machine);
    CHECK(machine.ioRead8(x68k::kMfpBase + 0x2F) == 0x20);
    for (int i = 0; i < 30; ++i)
    {
        keys.drain(machine);
    }
    CHECK((machine.ioRead8(x68k::kMfpBase + 0x2B) & 0x80) == 0);
    REQUIRE(keys.pushScan(0x25));
    REQUIRE(keys.pushScan(0xA0));
    keys.drain(machine);
    CHECK(machine.ioRead8(x68k::kMfpBase + 0x2F) == 0x25);
    for (int i = 0; i <= keys.kStepsPerEvent; ++i)
    {
        keys.drain(machine);
    }
    CHECK(machine.ioRead8(x68k::kMfpBase + 0x2F) == 0xA0);
}

TEST_CASE("Raw scan rejects invalid or overflowing input without blocking")
{
    x68k_platform::KeyQueue keys;
    CHECK_FALSE(keys.pushScan(0x20));
    REQUIRE(keys.begin());
    CHECK_FALSE(keys.pushScan(0));
    CHECK_FALSE(keys.pushScan(0x80));
    for (int i = 0; i < 64; ++i)
    {
        REQUIRE(keys.pushScan(0x20));
    }
    CHECK_FALSE(keys.pushScan(0xA0));
}
