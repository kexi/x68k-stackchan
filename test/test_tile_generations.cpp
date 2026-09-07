// SPDX-License-Identifier: MIT
#include "doctest.h"
#include "video/tile_generations.h"

#include <limits>
#include <vector>

namespace
{
template <typename G>
void renderAll(x68k::TileGenerations<G>& tiles, std::uint32_t buffer)
{
    for (std::uint32_t tile = 0; tile < tiles.kTiles; ++tile)
    {
        tiles.rendered(buffer, tile);
    }
}
template <typename G>
unsigned dirtyCount(const x68k::TileGenerations<G>& tiles, unsigned buffer)
{
    unsigned count = 0;
    for (std::uint32_t tile = 0; tile < tiles.kTiles; ++tile)
    {
        count += tiles.needsRender(buffer, tile) ? 1u : 0u;
    }
    return count;
}
}  // namespace

TEST_CASE("tile generations retain all changes independently for both output buffers")
{
    x68k::TileGenerations<> tiles;
    CHECK(dirtyCount(tiles, 0) == 300);
    CHECK(dirtyCount(tiles, 1) == 300);
    renderAll(tiles, 0);
    renderAll(tiles, 1);
    tiles.invalidateRect(0, 0, 16, 16);
    tiles.rendered(0, 0);
    tiles.invalidateRect(16, 0, 16, 16);
    CHECK(dirtyCount(tiles, 0) == 1);
    CHECK(dirtyCount(tiles, 1) == 2);
    // Publishing buffer 0 can fail without losing its already-composited tile.
    CHECK_FALSE(tiles.needsRender(0, 0));
    CHECK(tiles.needsRender(1, 0));
    tiles.invalidateAll();
    CHECK(dirtyCount(tiles, 0) == 300);
    CHECK(dirtyCount(tiles, 1) == 300);
}

TEST_CASE("tile damage clips negative, empty, outside and overflowing rectangles")
{
    x68k::TileGenerations<> tiles;
    renderAll(tiles, 0);
    tiles.invalidateRect(15, 15, 2, 2);
    CHECK(dirtyCount(tiles, 0) == 4);
    renderAll(tiles, 0);
    tiles.invalidateRect(-15, -15, 16, 16);
    CHECK(dirtyCount(tiles, 0) == 1);
    renderAll(tiles, 0);
    tiles.invalidateRect(320, 240, 16, 16);
    tiles.invalidateRect(0, 0, 0, 1);
    tiles.invalidateRect(std::numeric_limits<std::int32_t>::max(), 0, 100, 100);
    CHECK(dirtyCount(tiles, 0) == 0);
    tiles.invalidateRect(319, 239, std::numeric_limits<std::int32_t>::max(), 100);
    CHECK(dirtyCount(tiles, 0) == 1);
    CHECK(tiles.needsRender(0, 299));
}

TEST_CASE("generation wrap invalidates both buffers instead of accepting stale pixels")
{
    x68k::TileGenerations<std::uint8_t> tiles;
    renderAll(tiles, 0);
    renderAll(tiles, 1);
    for (unsigned i = 0; i < 254; ++i)
    {
        tiles.invalidateRect(0, 0, 1, 1);
        tiles.rendered(0, 0);
    }
    CHECK(dirtyCount(tiles, 0) == 0);
    tiles.invalidateRect(0, 0, 1, 1);
    CHECK(dirtyCount(tiles, 0) == 300);
    CHECK(dirtyCount(tiles, 1) == 300);
    tiles.rendered(0, 0);
    CHECK(dirtyCount(tiles, 0) == 299);
}

TEST_CASE("tile generations reproduce the complete pixel model across alternating buffers")
{
    x68k::TileGenerations<> tiles;
    std::vector<std::uint16_t> expected(320 * 240, 0);
    std::vector<std::uint16_t> buffers[2] = {std::vector<std::uint16_t>(320 * 240, 0xffff),
                                             std::vector<std::uint16_t>(320 * 240, 0xffff)};
    std::uint32_t random = 0x18273645u;
    for (unsigned frame = 0; frame < 96; ++frame)
    {
        random = random * 1664525u + 1013904223u;
        const int x = static_cast<int>(random % 420u) - 50;
        const int y = static_cast<int>((random >> 9) % 340u) - 50;
        const int width = static_cast<int>((random >> 18) % 50u) + 1;
        const int height = static_cast<int>((random >> 24) % 50u) + 1;
        tiles.invalidateRect(x, y, width, height);
        for (int py = std::max(0, y); py < std::min(240, y + height); ++py)
        {
            for (int px = std::max(0, x); px < std::min(320, x + width); ++px)
            {
                expected[static_cast<std::size_t>(py * 320 + px)] =
                    static_cast<std::uint16_t>(frame + 1);
            }
        }
        const bool modeChanged = frame % 17 == 0;
        if (modeChanged)
        {
            std::fill(expected.begin(), expected.end(), static_cast<std::uint16_t>(frame));
            tiles.invalidateAll();
        }
        const unsigned buffer = frame % 3 == 0 ? 0u : frame % 2;
        for (unsigned tile = 0; tile < tiles.kTiles; ++tile)
        {
            const bool needsRender = tiles.needsRender(buffer, tile);
            if (!needsRender)
            {
                continue;
            }
            for (unsigned row = 0; row < 16; ++row)
            {
                const auto start = (tile / 20 * 16 + row) * 320 + tile % 20 * 16;
                std::copy_n(expected.data() + start, 16, buffers[buffer].data() + start);
            }
            tiles.rendered(buffer, tile);
        }
        CHECK(buffers[buffer] == expected);
    }
}
