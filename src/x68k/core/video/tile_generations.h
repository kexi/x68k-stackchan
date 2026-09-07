// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_VIDEO_TILE_GENERATIONS_H
#define X68K_CORE_VIDEO_TILE_GENERATIONS_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <type_traits>

namespace x68k
{

// Core1所有。invalidateと描画commitの間にゲストを進めない。
// 二重バッファは2回前の画素を持ち得るため、直近のdirtyだけでは不足する。
template <typename Generation = std::uint32_t>
class TileGenerations
{
    static_assert(std::is_unsigned_v<Generation>);

public:
    static constexpr std::uint32_t kWidth = 320;
    static constexpr std::uint32_t kHeight = 240;
    static constexpr std::uint32_t kTileSize = 16;
    static constexpr std::uint32_t kColumns = kWidth / kTileSize;
    static constexpr std::uint32_t kRows = kHeight / kTileSize;
    static constexpr std::uint32_t kTiles = kColumns * kRows;
    // Why 3 枚か: LCD への転送に 1 枚 32.2ms かかる。その間 Core0 は
    // 1 枚を掴んだままなので、2 枚だと Core1 は残り 1 枚に書いた後、
    // 転送が終わるまで publish できず止まる (実測 backpressure 490回/5秒)。
    // 3 枚なら転送中でも書いて渡せるので、生産が転送で途切れない。
    static constexpr std::uint32_t kBuffers = 3;

    TileGenerations()
    {
        scene_.fill(generation_);
    }

    void invalidateAll()
    {
        advance();
        scene_.fill(generation_);
    }

    void invalidateRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
    {
        const bool empty = width <= 0 || height <= 0;
        if (empty)
        {
            return;
        }
        const auto left = std::max<std::int64_t>(0, x);
        const auto top = std::max<std::int64_t>(0, y);
        const auto right = std::min<std::int64_t>(kWidth, std::int64_t{x} + width);
        const auto bottom = std::min<std::int64_t>(kHeight, std::int64_t{y} + height);
        const bool outside = left >= right || top >= bottom;
        if (outside)
        {
            return;
        }
        advance();
        for (auto row = top / kTileSize; row <= (bottom - 1) / kTileSize; ++row)
        {
            for (auto column = left / kTileSize; column <= (right - 1) / kTileSize; ++column)
            {
                scene_[static_cast<std::size_t>(row * kColumns + column)] = generation_;
            }
        }
    }

    [[nodiscard]] bool needsRender(std::uint32_t buffer, std::uint32_t tile) const
    {
        const bool valid = buffer < kBuffers && tile < kTiles;
        return valid && buffers_[buffer][tile] != scene_[tile];
    }

    // 全レイヤーを再合成したtileだけcommit。公開失敗では取り消さない。
    void rendered(std::uint32_t buffer, std::uint32_t tile)
    {
        const bool valid = buffer < kBuffers && tile < kTiles;
        if (valid)
        {
            buffers_[buffer][tile] = scene_[tile];
        }
    }

private:
    void advance()
    {
        generation_ = static_cast<Generation>(generation_ + 1u);
        const bool wrapped = generation_ == 0;
        if (!wrapped)
        {
            return;
        }
        // 古い世代と一致させない。wrap時は全bufferを無効にしてから再採番する。
        generation_ = 1;
        scene_.fill(generation_);
        for (auto& buffer : buffers_)
        {
            buffer.fill(0);
        }
    }

    Generation generation_ = 1;
    std::array<Generation, kTiles> scene_{};
    std::array<std::array<Generation, kTiles>, kBuffers> buffers_{};
};

}  // namespace x68k
#endif
