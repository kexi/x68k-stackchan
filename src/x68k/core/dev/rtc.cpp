// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "rtc.h"

namespace x68k
{
namespace
{

// kCyclesPerSecond は Rtc のメンバへ移した。tick() の速い側がヘッダで
// 閾値を比較するため、ヘッダから見える場所に無いと inline できない。

// うるう年を考慮した各月の日数。
u32 daysInMonth(u32 month, u32 year)
{
    switch (month)
    {
        case 2:
            // RP5C15 は西暦下 2 桁しか持たないので、2000 年代として扱う。
            // 2000 年は 400 で割り切れるうるう年なので、下 2 桁の 4 の倍数判定で足りる。
            return (year % 4 == 0) ? 29 : 28;
        case 4:
        case 6:
        case 9:
        case 11:
            return 30;
        default:
            return 31;
    }
}

}  // namespace

void Rtc::reset()
{
    // Human68k が「不正な日付」と判断しないよう、妥当な初期値を入れる。
    // 全部 0 だと 0 月 0 日になり、受け取った側が想定外の動きをすることがある。
    second_ = 0;
    minute_ = 0;
    hour_ = 0;
    day_ = 1;
    month_ = 1;
    year_ = 26;
    weekday_ = 0;
    cycleAccumulator_ = 0;
    bank1_.fill(0);
    mode_ = 0;
}

void Rtc::setDateTime(u32 year, u32 month, u32 day, u32 hour, u32 minute, u32 second)
{
    year_ = year % 100;
    month_ = (month >= 1 && month <= 12) ? month : 1;
    // 月の日数に収める。
    //
    // 31 で一律に丸めると 2 月 31 日のような日付が通り、Human68k が
    // 「不正な日付」と判断しうる。daysInMonth は閏年も見る。
    const u32 maxDay = daysInMonth(month_, year_);
    day_ = (day >= 1 && day <= maxDay) ? day : 1;
    hour_ = hour % 24;
    minute_ = minute % 60;
    second_ = second % 60;
}

u8 Rtc::read(u32 regIndex) const
{
    // モードレジスタでバンク 1 が選ばれていれば、そちらを返す。
    const bool bank1Selected = (mode_ & 0x01u) != 0;
    if (bank1Selected && regIndex < kModeRegister)
    {
        return static_cast<u8>(bank1_[regIndex] & 0x0Fu);
    }

    // レジスタは 4bit 幅。上位は 0 として読まれる。
    switch (regIndex)
    {
        case kSecond1:
            return static_cast<u8>(second_ % 10);
        case kSecond10:
            return static_cast<u8>(second_ / 10);
        case kMinute1:
            return static_cast<u8>(minute_ % 10);
        case kMinute10:
            return static_cast<u8>(minute_ / 10);
        case kHour1:
            return static_cast<u8>(hour_ % 10);
        case kHour10:
            return static_cast<u8>(hour_ / 10);
        case kWeekday:
            return static_cast<u8>(weekday_);
        case kDay1:
            return static_cast<u8>(day_ % 10);
        case kDay10:
            return static_cast<u8>(day_ / 10);
        case kMonth1:
            return static_cast<u8>(month_ % 10);
        case kMonth10:
            return static_cast<u8>(month_ / 10);
        case kYear1:
            return static_cast<u8>(year_ % 10);
        case kYear10:
            return static_cast<u8>(year_ / 10);
        case kModeRegister:
            return static_cast<u8>(mode_ & 0x0Fu);
        default:
            return 0u;
    }
}

void Rtc::write(u32 regIndex, u8 value)
{
    const u8 nibble = static_cast<u8>(value & 0x0Fu);

    if (regIndex == kModeRegister)
    {
        mode_ = nibble;
        return;
    }

    const bool bank1Selected = (mode_ & 0x01u) != 0;
    if (bank1Selected && regIndex < kModeRegister)
    {
        bank1_[regIndex] = nibble;
        return;
    }

    // 時計の書き換え。桁ごとに入るので、該当の桁だけを差し替える。
    switch (regIndex)
    {
        case kSecond1:
            second_ = (second_ / 10) * 10 + nibble;
            return;
        case kSecond10:
            second_ = nibble * 10 + (second_ % 10);
            return;
        case kMinute1:
            minute_ = (minute_ / 10) * 10 + nibble;
            return;
        case kMinute10:
            minute_ = nibble * 10 + (minute_ % 10);
            return;
        case kHour1:
            hour_ = (hour_ / 10) * 10 + nibble;
            return;
        case kHour10:
            hour_ = nibble * 10 + (hour_ % 10);
            return;
        case kWeekday:
            weekday_ = nibble % 7;
            return;
        case kDay1:
            day_ = (day_ / 10) * 10 + nibble;
            return;
        case kDay10:
            day_ = nibble * 10 + (day_ % 10);
            return;
        case kMonth1:
            month_ = (month_ / 10) * 10 + nibble;
            return;
        case kMonth10:
            month_ = nibble * 10 + (month_ % 10);
            return;
        case kYear1:
            year_ = (year_ / 10) * 10 + nibble;
            return;
        case kYear10:
            year_ = nibble * 10 + (year_ % 10);
            return;
        default:
            return;
    }
}

void Rtc::advanceOneSecond()
{
    if (++second_ < 60)
    {
        return;
    }
    second_ = 0;

    if (++minute_ < 60)
    {
        return;
    }
    minute_ = 0;

    if (++hour_ < 24)
    {
        return;
    }
    hour_ = 0;

    weekday_ = (weekday_ + 1) % 7;

    if (++day_ <= daysInMonth(month_, year_))
    {
        return;
    }
    day_ = 1;

    if (++month_ <= 12)
    {
        return;
    }
    month_ = 1;
    year_ = (year_ + 1) % 100;
}

void Rtc::tickCarry()
{
    // 累算は呼び出し側 (ヘッダの tick) が済ませてある。ここへ来た時点で
    // 1 秒ぶん以上溜まっていることが確定している。
    //
    // while なのは、1 回の呼び出しで 1 秒を超えるサイクルを渡されうるため
    // (ホストのランナーは --cycles をまとめて渡せる)。
    while (cycleAccumulator_ >= kCyclesPerSecond)
    {
        cycleAccumulator_ -= kCyclesPerSecond;
        advanceOneSecond();
    }
}

}  // namespace x68k
