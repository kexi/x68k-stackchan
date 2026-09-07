// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "sprite.h"

namespace x68k
{

void Sprite::reset()
{
    reg_.fill(0);
    vram_.fill(0);
    visibleCount_ = 0;
    damage_.all();
}

u16 Sprite::read(u32 offset) const
{
    const u32 word = offset / 2u;
    if (word >= kRegWords)
    {
        return 0;
    }
    return reg_[word];
}

void Sprite::write(u32 offset, u16 value)
{
    const u32 word = offset / 2u;
    if (word >= kRegWords)
    {
        return;
    }

    const bool unchanged = reg_[word] == value;
    if (unchanged)
    {
        return;
    }
    const bool isSpriteReg = word < kSpriteCount * 4u;
    const u32 index = word / 4u;
    const bool eraseOld = isSpriteReg && spriteVisible(index);
    if (eraseOld)
    {
        damage_.rect(spriteX(index), spriteY(index), 16, 16);
    }
    const bool isPriorityWord = (word & 3u) == 3u;
    if (isSpriteReg && isPriorityWord)
    {
        // 全128個の再走査は同値の優先度書込みでも発生するため行わない。
        // byte RMWもその時点のword旧値/新値を比べれば、途中状態まで正確に保てる。
        const bool wasVisible = (reg_[word] & 3u) != 0;
        const bool isVisible = (value & 3u) != 0;
        if (wasVisible != isVisible)
        {
            if (isVisible)
            {
                ++visibleCount_;
            }
            else
            {
                --visibleCount_;
            }
        }
    }
    reg_[word] = value;
    const bool drawNew = isSpriteReg && spriteVisible(index);
    if (drawNew)
    {
        damage_.rect(spriteX(index), spriteY(index), 16, 16);
    }
    const bool changesBg = word >= 0x800u / 2u;
    if (changesBg)
    {
        damage_.all();
    }
}

}  // namespace x68k
