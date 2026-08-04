// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// デバイスへ時間を渡す粒度が、ゲストから観測できるずれを生まないこと。
//
// 速度のために「デバイスの tick を数サイクルまとめて渡す」最適化を 3 度
// 入れて、3 度とも観測可能なずれを作った。レビューで指摘されるまで
// 気づかなかったのは、この性質を確かめるテストが無かったため。
//
// ここで守るのは「まとめない」ことではなく **「まとめた結果がゲストから
// 見えない」こと**。将来また quantum を入れるなら、このテストが通る形で
// なければならない。

#include <utility>

#include "dev/mfp.h"
#include "dev/rtc.h"
#include "dev/video.h"
#include "doctest.h"

namespace
{

// 同じ総サイクル数を「細かく」と「粗く」渡し、状態が変わる瞬間を比べる。
// 戻り値は (細かい方で変化したサイクル, 粗い方で変化したサイクル)。
template <typename Device, typename Setup, typename Changed>
std::pair<x68k::u32, x68k::u32> compareGranularity(Setup setup, Changed changed, x68k::u32 fineStep,
                                                   x68k::u32 coarseStep, x68k::u32 limit,
                                                   x68k::u32 warmup)
{
    Device fine;
    Device coarse;
    setup(fine);
    setup(coarse);

    // 分周器やフレーム位置を途中まで進めてから比べる。位相が 0 のときだけ
    // 一致しても意味が無い (実際、位相 0 では差が出ずに見逃した)。
    if (warmup != 0)
    {
        fine.tick(warmup);
        coarse.tick(warmup);
    }

    x68k::u32 fineAt = 0;
    x68k::u32 coarseAt = 0;
    x68k::u32 pending = 0;
    for (x68k::u32 cyc = fineStep; cyc <= limit; cyc += fineStep)
    {
        fine.tick(fineStep);
        if (fineAt == 0 && changed(fine))
        {
            fineAt = cyc;
        }

        pending += fineStep;
        if (pending >= coarseStep)
        {
            coarse.tick(pending);
            pending = 0;
        }
        if (coarseAt == 0 && changed(coarse))
        {
            coarseAt = cyc;
        }
    }
    return {fineAt, coarseAt};
}

// かつて入れて、いずれも観測可能なずれを作った quantum。
// 粗い側にこれを使うことで「その粒度でまとめても見えないか」を問う。
// 現在は quantum を使っていないので、Machine が渡すのは命令ごとの
// サイクル数そのもの。ここが通らない粒度は入れてはいけない。
constexpr x68k::u32 kPastMfpQuantum = 8;
constexpr x68k::u32 kPastCrtcQuantum = 64;
constexpr x68k::u32 kPastRtcQuantum = 10000;

}  // namespace

TEST_SUITE("device-timing")
{
    // MFP のタイマ割り込みは、分周器が途中まで埋まっていても遅れない。
    //
    // 8 サイクル単位でまとめていたとき、分周器に 4 サイクル溜まった状態から
    // だと割り込みが 4 サイクル遅れた。「最小分周が 8 サイクルだから 1 周期
    // 以内に必ず見る」という理屈は、位相を考えていなかった。
    TEST_CASE("MFP タイマ割り込みは分周器の位相によらず同じ時点で上がる")
    {
        const auto setup = [](x68k::Mfp& m)
        {
            m.reset();
            m.write(0x10, 1);     // TBDR = 1 (毎回タイムアウト)
            m.write(0x0D, 0x01);  // TBCR = 分周 4 (最小)
            m.write(0x03, 0x01);  // IERA: タイマ B 許可
            m.write(0x09, 0x01);  // IMRA
        };
        const auto changed = [](const x68k::Mfp& m) { return m.hasPendingInterrupt(); };

        // 現在の Machine は quantum を使わず、命令ごとのサイクル数を
        // そのまま渡す。同じ粒度なら当然ずれない。
        for (const x68k::u32 warmup : {0u, 4u})
        {
            const auto [fineAt, coarseAt] =
                compareGranularity<x68k::Mfp>(setup, changed, 4, 4, 400, warmup);
            CAPTURE(warmup);
            REQUIRE(fineAt != 0);
            CHECK(coarseAt == fineAt);
        }

        // かつて入れていた 8 サイクル quantum では、分周器の位相が
        // 半分埋まった状態で割り込みが遅れた。この性質を固定しておく
        // (再び quantum を入れるなら、まずここが失敗する)。
        const auto [fineAt, coarseAt] =
            compareGranularity<x68k::Mfp>(setup, changed, 4, kPastMfpQuantum, 400, 4);
        CHECK(coarseAt > fineAt);
    }

    // 垂直帰線の開始は、ゲストが GPIP4 として命令単位でポーリングできる。
    // 64 サイクル単位でまとめていたときは 24 サイクル遅れて見えた。
    TEST_CASE("垂直帰線の開始が遅れて観測されない")
    {
        const auto setup = [](x68k::Crtc& c) { c.reset(); };
        const auto changed = [](const x68k::Crtc& c) { return c.inVerticalBlank(); };

        const auto [fineAt, coarseAt] =
            compareGranularity<x68k::Crtc>(setup, changed, 4, 4, x68k::Crtc::kCyclesPerFrame, 0);
        REQUIRE(fineAt != 0);
        CHECK(coarseAt == fineAt);

        // 64 サイクル単位でまとめると 24 サイクル遅れて見えた。
        const auto [_, delayedAt] = compareGranularity<x68k::Crtc>(
            setup, changed, 4, kPastCrtcQuantum, x68k::Crtc::kCyclesPerFrame, 0);
        CHECK(delayedAt > fineAt);
    }

    // RTC の秒が進む瞬間も、ポーリングループの反復回数として観測できる。
    // 10000 サイクル単位でまとめていたときは最大 1998 サイクル遅れた。
    TEST_CASE("RTC の秒境界が遅れて観測されない")
    {
        const auto setup = [](x68k::Rtc& r) { r.reset(); };
        // 秒の 1 桁目が変わったか。reset 直後の値を基準にする。
        x68k::Rtc reference;
        reference.reset();
        const x68k::u8 base = reference.read(0);
        const auto changed = [base](const x68k::Rtc& r) { return r.read(0) != base; };

        const auto [fineAt, coarseAt] =
            compareGranularity<x68k::Rtc>(setup, changed, 6, 6, 10100000, 0);
        REQUIRE(fineAt != 0);
        CHECK(coarseAt == fineAt);

        // 10000 サイクル単位でまとめると秒境界が最大 1998 サイクル遅れた。
        const auto [_, delayedAt] =
            compareGranularity<x68k::Rtc>(setup, changed, 6, kPastRtcQuantum, 10100000, 0);
        CHECK(delayedAt > fineAt);
    }
}
