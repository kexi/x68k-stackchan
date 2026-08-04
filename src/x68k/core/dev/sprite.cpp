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

    reg_[word] = value;

    // 表示中のスプライト数は、プライオリティのワードが変わったときだけ数え直す。
    //
    // スプライトレジスタは 1 個 4 ワードで、プライオリティは 4 ワード目
    // (バイトオフセットの下位 3bit が 6)。座標やパターンの書き換えでは
    // 表示/非表示は変わらないので、数え直す必要がない。
    //
    // Why not 書き込みのたびに 128 個を数え直さないか: スプライトを動かす
    // ソフトは毎フレーム 128 個ぶんの座標を書く。そのたびに 128 回の走査を
    // 足すと、ESP32-S3 の実効 3MHz では書き込み側が描画より重くなる。
    //
    // Why not 差分だけで増減を追わないか (旧値と新値を比べる): それでも
    // 正しく数えられるが、バイト単位の read-modify-write が絡むと
    // 「上位バイトだけ書いた途中の値」で増減が起きる。数え直しは
    // プライオリティの書き込みという稀な場合にしか走らないので、
    // 状態を持たない素直なほうを選ぶ。
    const bool isSpriteReg = offset < kSpriteCount * kSpriteStride;
    const bool isPriorityWord = (offset & (kSpriteStride - 1u)) == 6u;
    if (isSpriteReg && isPriorityWord)
    {
        recountVisible();
    }
}

void Sprite::recountVisible()
{
    u32 count = 0;
    for (u32 i = 0; i < kSpriteCount; ++i)
    {
        if (spritePriority(i) != 0)
        {
            ++count;
        }
    }
    visibleCount_ = count;
}

}  // namespace x68k
