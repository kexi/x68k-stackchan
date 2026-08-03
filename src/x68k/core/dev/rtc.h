// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// RTC RP5C15 ($E8A000)。
//
// Human68k は起動時に日付と時刻を読む。全部 0 を返すと「0 年 0 月 0 日」に
// なり、月や日として不正な値を受け取った側が想定外の動きをすることがある。
// 妥当な日付を返せるようにしておく。
//
// レジスタは 4bit 幅で、$E8A001 から 2 バイトおきに 13 個並ぶ。
// バンク切り替えがあり、バンク 0 が時計、バンク 1 がアラームと設定。

#ifndef X68K_CORE_DEV_RTC_H
#define X68K_CORE_DEV_RTC_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Rtc
{
public:
    // レジスタ番号 (ベースからのオフセット / 2)。
    enum Reg : u32
    {
        kSecond1 = 0x0,   // 秒の 1 の位
        kSecond10 = 0x1,  // 秒の 10 の位
        kMinute1 = 0x2,
        kMinute10 = 0x3,
        kHour1 = 0x4,
        kHour10 = 0x5,
        kWeekday = 0x6,
        kDay1 = 0x7,
        kDay10 = 0x8,
        kMonth1 = 0x9,
        kMonth10 = 0xA,
        kYear1 = 0xB,
        kYear10 = 0xC,
        kModeRegister = 0xD,  // バンク選択
        kTestRegister = 0xE,
        kResetRegister = 0xF,
        kRegCount = 0x10,
    };

    void reset();

    [[nodiscard]] u8 read(u32 regIndex) const;
    void write(u32 regIndex, u8 value);

    // CPU サイクルぶん時間を進める。秒が繰り上がる。
    void tick(u32 cycles);

    // 起点となる日時を設定する。エミュレータの起動時にホストの時刻を渡す。
    // year は西暦の下 2 桁 (RP5C15 は 2 桁しか持たない)。
    void setDateTime(u32 year, u32 month, u32 day, u32 hour, u32 minute, u32 second);

private:
    void advanceOneSecond();

    // 時計の値。BCD ではなく 10 進の各桁として持つ (レジスタが 4bit 幅のため)。
    u32 second_ = 0;
    u32 minute_ = 0;
    u32 hour_ = 0;
    u32 day_ = 1;
    u32 month_ = 1;
    u32 year_ = 26;  // 西暦の下 2 桁
    u32 weekday_ = 0;

    // 1 秒ぶんの CPU サイクルを数える。
    u32 cycleAccumulator_ = 0;

    // バンク 1 のレジスタ。アラームなど。読み書きできれば足りる。
    std::array<u8, kRegCount> bank1_{};
    u8 mode_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_RTC_H
