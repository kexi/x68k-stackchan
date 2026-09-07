// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// flash の storage パーティションから ROM とディスクイメージを読む。
//
// なぜ SD と別に用意するか:
//   既定は microSD から読む。差し替えが楽なのが利点だが、
//   「カードを挿さずに動かしたい」「配る相手にカードを用意させたくない」
//   ときに困る。パーティション表には storage (12MB) が空けてあり、
//   コメントにも「ROM を SD ではなく flash に焼きたくなった場合」に
//   備えて確保してある、と書いてある。それを使う。
//
// SD が使えるならそちらを優先する。焼き直さずに中身を変えられる方が
// 開発中は都合がよいため。flash はフォールバック。
//
// イメージの形式は tools/mkflashimage.py が作る。ディスクは疎で持つ
// (SASI のイメージは最低 10MB 要るが中身はほとんどゼロなので、
// 非ゼロのセクタだけ並べて索引で引く)。

#ifndef X68K_PLATFORM_STORAGE_FLASH_H
#define X68K_PLATFORM_STORAGE_FLASH_H

#include <cstddef>
#include <cstdint>

#include "machine.h"

namespace x68k_platform
{

// storage パーティションを開いて、中身が使えるかを確かめる。
//
// マジックとバージョンが合わなければ false。焼かれていない (全部 $FF) 場合も
// ここで弾かれる。
bool mountFlashData();

// flash 上の IPL-ROM を buffer へ写す。読めたバイト数を返す。
std::size_t loadFlashIplRom(std::uint8_t* buffer, std::size_t bufferSize);

// flash 上の CGROM を buffer へ写す。無ければ 0。
std::size_t loadFlashCgRom(std::uint8_t* buffer, std::size_t bufferSize);

// flash 上のディスクイメージ。
//
// 疎で持つので、索引に無いセクタはゼロを返す。書き込みは受け付けない
// (flash を毎回消去するのは寿命にも速度にも見合わない)。
// 書き込みが要るソフトは SD を使う。
class FlashDisk final : public x68k::DiskImage
{
public:
    bool open();

    bool readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount) override;
    bool writeSector(x68k::u32 lba, const x68k::u8* buffer, x68k::u32 sectorCount) override;
    [[nodiscard]] bool isPresent() const override;
    [[nodiscard]] std::int64_t readTimeUs() const
    {
        return readTimeUs_;
    }

private:
    bool present_ = false;
    std::int64_t readTimeUs_ = 0;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_STORAGE_FLASH_H
