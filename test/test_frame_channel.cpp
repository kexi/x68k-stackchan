// SPDX-License-Identifier: MIT
#include "doctest.h"
#include "frame_channel.h"

#include <array>

TEST_CASE("frame preflight rejects uninitialized, aliased and repeated initialization")
{
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{}, c{};
    CHECK(frames.tryWriteBuffer() == nullptr);
    CHECK_FALSE(frames.begin(nullptr, b.data(), c.data()));
    CHECK_FALSE(frames.begin(a.data(), nullptr, c.data()));
    CHECK_FALSE(frames.begin(a.data(), b.data(), nullptr));
    CHECK_FALSE(frames.begin(a.data(), a.data(), c.data()));
    CHECK_FALSE(frames.begin(a.data(), b.data(), b.data()));
    CHECK_FALSE(frames.begin(a.data(), b.data(), a.data()));
    REQUIRE(frames.begin(a.data(), b.data(), c.data()));
    CHECK_FALSE(frames.begin(a.data(), b.data(), c.data()));
    CHECK(frames.tryWriteBuffer() == a.data());
}

TEST_CASE("転送中でも次の1枚を書ける (3枚にした理由そのもの)")
{
    // 2 枚だった頃は、転送中に書ける枚が無く Core1 が止まっていた。
    // 3 枚あれば「転送中」「公開待ち」「書いている」が同時に成り立つ。
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{}, c{};
    REQUIRE(frames.begin(a.data(), b.data(), c.data()));

    auto* first = frames.tryWriteBuffer();
    REQUIRE(first != nullptr);
    first[0] = 17;
    REQUIRE(frames.publish());

    // Core0 が 1 枚目を転送し始める。
    auto* inTransfer = frames.take();
    REQUIRE(inTransfer == first);

    // 転送中でも 2 枚目を借りて書けること。ここが 2 枚では nullptr だった。
    auto* second = frames.tryWriteBuffer();
    REQUIRE(second != nullptr);
    CHECK(second != inTransfer);
    second[0] = 29;
    REQUIRE(frames.publish());

    // さらに 3 枚目も借りられる。
    auto* third = frames.tryWriteBuffer();
    REQUIRE(third != nullptr);
    CHECK(third != inTransfer);
    CHECK(third != second);

    // 転送中の 1 枚は書き換わっていない。
    CHECK(inTransfer[0] == 17);

    // 転送は 1 枚ずつ。終わるまで次は渡さない。
    CHECK(frames.take() == nullptr);

    frames.done();
    // 公開済みの 2 枚目が渡る。
    CHECK(frames.take() == second);
    CHECK(second[0] == 29);
}

TEST_CASE("公開待ちが複数あるとき、古い方から渡す")
{
    // 新しい絵を先に出すと、次に古い絵が出たときに画面が巻き戻って見える。
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{}, c{};
    REQUIRE(frames.begin(a.data(), b.data(), c.data()));

    auto* first = frames.tryWriteBuffer();
    first[0] = 1;
    REQUIRE(frames.publish());
    auto* second = frames.tryWriteBuffer();
    second[0] = 2;
    REQUIRE(frames.publish());

    auto* taken = frames.take();
    CHECK(taken[0] == 1);
    frames.done();
    taken = frames.take();
    CHECK(taken[0] == 2);
    frames.done();
}

TEST_CASE("no-change rendering need not publish or consume frame capacity")
{
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{}, c{};
    REQUIRE(frames.begin(a.data(), b.data(), c.data()));
    // 公開しない限り同じ枚を返し続ける。書きかけを別の枚へ移さない。
    for (int i = 0; i < 10; ++i)
    {
        CHECK(frames.tryWriteBuffer() == a.data());
        CHECK(frames.take() == nullptr);
    }
    REQUIRE(frames.publish());
    CHECK(frames.take() == a.data());
    frames.done();
    CHECK(frames.tryWriteBuffer() != nullptr);
}

TEST_CASE("借りていないのに公開しても何も起きない")
{
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{}, c{};
    REQUIRE(frames.begin(a.data(), b.data(), c.data()));
    CHECK_FALSE(frames.publish());
    CHECK(frames.take() == nullptr);
}

TEST_CASE("3枚すべてが埋まったら貸せない")
{
    x68k_platform::FrameChannel frames;
    std::array<x68k::u16, 4> a{}, b{}, c{};
    REQUIRE(frames.begin(a.data(), b.data(), c.data()));

    // 1 枚を転送中に、残り 2 枚を公開待ちにする。
    REQUIRE(frames.tryWriteBuffer() != nullptr);
    REQUIRE(frames.publish());
    REQUIRE(frames.take() != nullptr);  // InTransfer
    REQUIRE(frames.tryWriteBuffer() != nullptr);
    REQUIRE(frames.publish());  // Ready
    REQUIRE(frames.tryWriteBuffer() != nullptr);
    REQUIRE(frames.publish());  // Ready

    // 空きが無いので貸せない。
    CHECK(frames.tryWriteBuffer() == nullptr);

    frames.done();
    CHECK(frames.tryWriteBuffer() != nullptr);
}
