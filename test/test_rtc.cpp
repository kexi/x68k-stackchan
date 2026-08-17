// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: RTC が Human68k の期待する形で日付を返すこと。
//
// Human68k は起動時に日付と時刻を読む。全部 0 を返すと「0 年 0 月 0 日」に
// なり、受け取った側が想定外の動きをすることがある。桁ごとに 4bit で返す
// という RP5C15 の作法も含めて押さえておく。

#include "dev/rtc.h"
#include "doctest.h"

TEST_CASE("リセット直後は妥当な日付になる")
{
    x68k::Rtc rtc;
    rtc.reset();

    // 月と日が 0 だと Human68k 側で不正な日付として扱われる。
    const x68k::u32 month =
        static_cast<x68k::u32>(rtc.read(x68k::Rtc::kMonth10)) * 10u + rtc.read(x68k::Rtc::kMonth1);
    const x68k::u32 day =
        static_cast<x68k::u32>(rtc.read(x68k::Rtc::kDay10)) * 10u + rtc.read(x68k::Rtc::kDay1);
    CHECK(month >= 1);
    CHECK(month <= 12);
    CHECK(day >= 1);
    CHECK(day <= 31);
}

TEST_CASE("各レジスタは 4bit 幅で読まれる")
{
    x68k::Rtc rtc;
    rtc.reset();
    rtc.setDateTime(26, 12, 31, 23, 59, 58);

    // RP5C15 のレジスタは 4bit。上位は 0 として読まれる。
    for (x68k::u32 reg = 0; reg <= x68k::Rtc::kYear10; ++reg)
    {
        CHECK((rtc.read(reg) & 0xF0u) == 0);
    }
}

TEST_CASE("設定した日時が桁ごとに読み出せる")
{
    x68k::Rtc rtc;
    rtc.reset();
    rtc.setDateTime(26, 8, 3, 14, 25, 36);

    CHECK(rtc.read(x68k::Rtc::kYear10) == 2);
    CHECK(rtc.read(x68k::Rtc::kYear1) == 6);
    CHECK(rtc.read(x68k::Rtc::kMonth10) == 0);
    CHECK(rtc.read(x68k::Rtc::kMonth1) == 8);
    CHECK(rtc.read(x68k::Rtc::kDay10) == 0);
    CHECK(rtc.read(x68k::Rtc::kDay1) == 3);
    CHECK(rtc.read(x68k::Rtc::kHour10) == 1);
    CHECK(rtc.read(x68k::Rtc::kHour1) == 4);
    CHECK(rtc.read(x68k::Rtc::kMinute10) == 2);
    CHECK(rtc.read(x68k::Rtc::kMinute1) == 5);
    CHECK(rtc.read(x68k::Rtc::kSecond10) == 3);
    CHECK(rtc.read(x68k::Rtc::kSecond1) == 6);
}

TEST_CASE("時間が進むと秒が繰り上がる")
{
    x68k::Rtc rtc;
    rtc.reset();
    rtc.setDateTime(26, 1, 1, 0, 0, 0);

    // X68000 の CPU は 10MHz。1 秒ぶん進める。
    rtc.tickFast<true>(10000000);
    CHECK(rtc.read(x68k::Rtc::kSecond1) == 1);

    rtc.tickFast<true>(10000000 * 59);
    CHECK(rtc.read(x68k::Rtc::kSecond1) == 0);
    CHECK(rtc.read(x68k::Rtc::kSecond10) == 0);
    CHECK(rtc.read(x68k::Rtc::kMinute1) == 1);
}

TEST_CASE("月末で日が繰り上がる")
{
    x68k::Rtc rtc;
    rtc.reset();
    // 1 月 31 日 23:59:59 の 1 秒後は 2 月 1 日。
    rtc.setDateTime(26, 1, 31, 23, 59, 59);
    rtc.tickFast<true>(10000000);

    CHECK(rtc.read(x68k::Rtc::kMonth1) == 2);
    CHECK(rtc.read(x68k::Rtc::kDay10) == 0);
    CHECK(rtc.read(x68k::Rtc::kDay1) == 1);
}

TEST_CASE("うるう年の 2 月は 29 日まである")
{
    x68k::Rtc rtc;
    rtc.reset();
    // 2024 年 (下 2 桁 24) はうるう年。2 月 28 日の翌日は 29 日。
    rtc.setDateTime(24, 2, 28, 23, 59, 59);
    rtc.tickFast<true>(10000000);
    CHECK(rtc.read(x68k::Rtc::kMonth1) == 2);
    CHECK(rtc.read(x68k::Rtc::kDay10) == 2);
    CHECK(rtc.read(x68k::Rtc::kDay1) == 9);

    // 平年 (下 2 桁 26) の 2 月 28 日の翌日は 3 月 1 日。
    x68k::Rtc rtc2;
    rtc2.reset();
    rtc2.setDateTime(26, 2, 28, 23, 59, 59);
    rtc2.tickFast<true>(10000000);
    CHECK(rtc2.read(x68k::Rtc::kMonth1) == 3);
    CHECK(rtc2.read(x68k::Rtc::kDay1) == 1);
}

TEST_CASE("年末で年が繰り上がる")
{
    x68k::Rtc rtc;
    rtc.reset();
    rtc.setDateTime(26, 12, 31, 23, 59, 59);
    rtc.tickFast<true>(10000000);

    CHECK(rtc.read(x68k::Rtc::kYear10) == 2);
    CHECK(rtc.read(x68k::Rtc::kYear1) == 7);
    CHECK(rtc.read(x68k::Rtc::kMonth1) == 1);
    CHECK(rtc.read(x68k::Rtc::kDay1) == 1);
}

TEST_CASE("書き込んだ値が読み戻せる")
{
    x68k::Rtc rtc;
    rtc.reset();

    // Human68k が日付を設定する場合の経路。
    rtc.write(x68k::Rtc::kMonth10, 1);
    rtc.write(x68k::Rtc::kMonth1, 2);
    CHECK(rtc.read(x68k::Rtc::kMonth10) == 1);
    CHECK(rtc.read(x68k::Rtc::kMonth1) == 2);
}

TEST_CASE("バンク 1 を選ぶとアラーム側のレジスタになる")
{
    x68k::Rtc rtc;
    rtc.reset();
    rtc.setDateTime(26, 8, 3, 0, 0, 0);

    // バンク 0 では日付が読める。
    CHECK(rtc.read(x68k::Rtc::kMonth1) == 8);

    // モードレジスタの bit0 でバンク 1 へ。
    rtc.write(x68k::Rtc::kModeRegister, 0x01);
    rtc.write(0, 0x0A);
    CHECK(rtc.read(0) == 0x0A);

    // バンク 0 へ戻すと日付が見える。
    rtc.write(x68k::Rtc::kModeRegister, 0x00);
    CHECK(rtc.read(x68k::Rtc::kMonth1) == 8);
}
