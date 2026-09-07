// SPDX-License-Identifier: MIT
#include "doctest.h"
#include "frame_channel.h"

#include <array>

TEST_CASE("frame preflight rejects uninitialized, aliased and repeated initialization")
{
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{};
    CHECK(frames.tryWriteBuffer() == nullptr);
    CHECK_FALSE(frames.begin(nullptr, b.data()));
    CHECK_FALSE(frames.begin(a.data(), a.data()));
    REQUIRE(frames.begin(a.data(), b.data()));
    CHECK_FALSE(frames.begin(a.data(), b.data()));
    CHECK(frames.tryWriteBuffer() == a.data());
}

TEST_CASE("pending and in-flight frames prevent composition without changing pixels")
{
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{};
    REQUIRE(frames.begin(a.data(), b.data()));
    auto* target = frames.tryWriteBuffer();
    REQUIRE(target != nullptr);
    target[0] = 17;
    REQUIRE(frames.publish());
    CHECK(frames.tryWriteBuffer() == nullptr);
    auto* front = frames.take();
    REQUIRE(front == a.data());
    CHECK(frames.tryWriteBuffer() == nullptr);
    CHECK_FALSE(frames.publish());
    CHECK(front[0] == 17);
    frames.done();
    target = frames.tryWriteBuffer();
    REQUIRE(target == b.data());
    target[0] = 29;
    CHECK(frames.take() == nullptr);
    REQUIRE(frames.publish());
    front = frames.take();
    REQUIRE(front == b.data());
    CHECK(front[0] == 29);
    frames.done();
    CHECK(frames.tryWriteBuffer() == a.data());
}

TEST_CASE("no-change rendering need not publish or consume frame capacity")
{
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{};
    REQUIRE(frames.begin(a.data(), b.data()));
    for (int i = 0; i < 10; ++i)
    {
        CHECK(frames.tryWriteBuffer() == a.data());
        CHECK(frames.take() == nullptr);
    }
    REQUIRE(frames.publish());
    CHECK(frames.take() == a.data());
    frames.done();
    CHECK(frames.tryWriteBuffer() == b.data());
}
