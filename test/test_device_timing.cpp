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

#include <algorithm>
#include <utility>
#include <vector>

#include "dev/mfp.h"
#include "dev/rtc.h"
#include "dev/video.h"
#include "machine.h"
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
        fine.template tickFast<true>(warmup);
        coarse.template tickFast<true>(warmup);
    }

    x68k::u32 fineAt = 0;
    x68k::u32 coarseAt = 0;
    x68k::u32 pending = 0;
    for (x68k::u32 cyc = fineStep; cyc <= limit; cyc += fineStep)
    {
        fine.template tickFast<true>(fineStep);
        if (fineAt == 0 && changed(fine))
        {
            fineAt = cyc;
        }

        pending += fineStep;
        if (pending >= coarseStep)
        {
            coarse.template tickFast<true>(pending);
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
        const auto [pastFineAt, delayedAt] =
            compareGranularity<x68k::Mfp>(setup, changed, 4, kPastMfpQuantum, 400, 4);
        REQUIRE(pastFineAt != 0);
        REQUIRE(delayedAt != 0);
        CHECK(delayedAt > pastFineAt);
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
        // 比較は同じ呼び出しが返す組の中で行う (別の呼び出しの fine と
        // 比べると、setup が状態を持つようになったとき誤判定する)。
        const auto [pastFineAt, delayedAt] = compareGranularity<x68k::Crtc>(
            setup, changed, 4, kPastCrtcQuantum, x68k::Crtc::kCyclesPerFrame, 0);
        REQUIRE(pastFineAt != 0);
        REQUIRE(delayedAt != 0);
        CHECK(delayedAt > pastFineAt);
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
        const auto [pastFineAt, delayedAt] =
            compareGranularity<x68k::Rtc>(setup, changed, 6, kPastRtcQuantum, 10100000, 0);
        REQUIRE(pastFineAt != 0);
        REQUIRE(delayedAt != 0);
        CHECK(delayedAt > pastFineAt);
    }

    // Machine が quantum を再導入したら落ちるテスト。
    //
    // 最初は「デバイスの状態を外から比べる」形で 4 通り試して、どれも
    // quantum を戻した状態で通ってしまった:
    //   1. run() と step() の最終状態を比べる (終了時の flush で一致)
    //   2. 総サイクルを quantum の倍数から外す (止めた位置で保留 0 なら一致)
    //   3. ラスタが変わるまでの run() 呼び出し回数 (あり/なしとも 80)
    //   4. 1 フレームのラスタ遷移回数 (累算器が受け取る総量が同じ)
    //
    // CRTC も RTC も MFP も加算アキュムレータなので、渡す総量が同じなら
    // 最終状態は一致する。それが 4 通りとも失敗した理由。
    //
    // 見落としていたのは **CPU が途中の状態を観測する** 経路だった。
    // Machine::run は「割り込み判定 -> 1 命令 -> tick」を繰り返すので、
    // tick が遅れると次の serviceInterrupts() が割り込みを見逃す。
    // 受理されたかどうかは SR とスタックに残るので、外から確かめられる。
    TEST_CASE("run() の tick 遅れで割り込みの受理が遅れない")
    {
        static std::vector<x68k::u8> ram(0x10000, 0);
        const auto poke16 = [](x68k::u32 addr, x68k::u16 v)
        {
            ram[addr] = static_cast<x68k::u8>(v >> 8);
            ram[addr + 1] = static_cast<x68k::u8>(v & 0xFF);
        };
        std::fill(ram.begin(), ram.end(), 0);
        poke16(0, 0x0000);
        poke16(2, 0x8000);  // SSP
        poke16(4, 0x0000);
        poke16(6, 0x0400);  // PC = $400
        for (x68k::u32 a = 0x400; a < 0x8000; a += 2)
        {
            poke16(a, 0x4E71);  // NOP (4 サイクル)
        }

        x68k::Machine m;
        x68k::MemoryMap map{};
        map.mainRam = ram.data();
        m.setMemory(map);
        m.reset();

        // タイマ B を分周 4 (最小)・データ 1 で動かし、割り込みを許可する。
        m.mfp().write(0x10, 1);     // TBDR = 1 (毎回タイムアウト)
        m.mfp().write(0x0D, 0x01);  // TBCR = 分周 4
        m.mfp().write(0x03, x68k::Mfp::kIntTimerB);
        m.mfp().write(0x09, x68k::Mfp::kIntTimerB);
        // MFP はレベル 6。CPU 側のマスクを下げて受理できるようにする。
        m.cpu().setSr(0x2000);

        // 分周器を半分 (2 MFP サイクル = 4 CPU サイクル) 進めておく。
        // ここが要点で、位相 0 だと quantum があっても差が出ない。
        m.mfp().tickFast<true>(4);

        const x68k::u32 sspBefore = m.cpu().state().a[7];

        // 2 命令ぶん (8 サイクル) 回す。
        //
        // 命令ごとに tick していれば、1 命令目の後にタイマ B がタイムアウト
        // して割り込みが上がり、2 回目の serviceInterrupts() で受理される。
        // 8 サイクルの quantum があると、tick は 2 命令目の後に 1 度だけ
        // 走るので、run(8) を抜けるまで受理されない。
        m.run(8);

        // 受理されていれば例外フレーム (SR + PC = 6 バイト) が積まれ、
        // SR の割り込みマスクが 6 へ上がる。
        const x68k::u32 sspAfter = m.cpu().state().a[7];
        CHECK(sspAfter < sspBefore);
        CHECK(m.cpu().state().interruptMask() == 6);
    }

    // 毎命令通る経路の最適化 (perf_switch.h) は、速さだけを変えて
    // 状態を変えないという約束で入れてある。実機で焼き直さずに効果を
    // 測るためのスイッチなので、両側で同じ答えが出ないと比較の意味が無い。
    //
    // ここが落ちるときは、速い側が遅い側と違う計算をしている。
    // 速度の差ではなく **正しさの差** なので、実機の数字を見る前に直す。
    TEST_CASE("最適化スイッチの両側で状態が一致する")
    {
        // NOP で埋めた RAM を Machine::run() で回す。
        //
        // Why not デバイスの tick を直接呼ばないか: それだと
        // Machine::tickDevices() がスイッチを取り違えていても、あるいは
        // 片方のデバイスへ渡し忘れていても通ってしまう。実機が通るのは
        // run() 経路なので、配線ごと確かめる。
        // MemoryMap::mainRam は常に kMainRamSize (2MB) として読まれる。
        // 短いバッファを渡すと、PC が伸びた先で確保範囲外を読む。
        // 1050 万サイクル回すので、ここは実寸で用意する。
        static std::vector<x68k::u8> ram(x68k::kMainRamSize, 0);
        const auto poke16 = [](x68k::u32 addr, x68k::u16 v)
        {
            ram[addr] = static_cast<x68k::u8>(v >> 8);
            ram[addr + 1] = static_cast<x68k::u8>(v & 0xFF);
        };

        // NOP を並べる範囲。末尾から先頭へ戻して PC を循環させる。
        constexpr x68k::u32 kCodeBegin = 0x400;
        constexpr x68k::u32 kCodeEnd = 0x8000;

        const auto runWith = [&](bool fastPath)
        {
            std::fill(ram.begin(), ram.end(), 0);
            poke16(0, 0x0000);
            poke16(2, 0x8000);  // SSP
            poke16(4, 0x0000);
            poke16(6, kCodeBegin);  // PC
            for (x68k::u32 a = kCodeBegin; a < kCodeEnd; a += 2)
            {
                poke16(a, 0x4E71);  // NOP (4 サイクル)
            }
            // 末尾は BRA で先頭へ戻す。$6000 が BRA.w で、次のワードが
            // 「BRA の置き場所 + 2」からの変位。PC が範囲外へ出ないので、
            // どれだけ長く回しても NOP の海の中に居続ける。
            const x68k::u32 braAt = kCodeEnd - 4;
            poke16(braAt, 0x6000);
            poke16(braAt + 2, static_cast<x68k::u16>(kCodeBegin - (braAt + 2)));

            x68k::Machine m;
            x68k::MemoryMap map{};
            map.mainRam = ram.data();
            m.setMemory(map);

            x68k::PerfSwitch sw;
            sw.inlineRtcTick = fastPath;
            sw.inlineCrtcTick = fastPath;
            sw.inlineMfpTimer = fastPath;
            m.setPerfSwitch(sw);
            m.reset();

            // タイマ C と D を動かす (X68000 が実際に使う 2 本)。
            // 分周は別々にして、片方だけの一致で通らないようにする。
            // 割り込みも許可する。IPRB が立たないと、下の「素通りでは
            // ない」確認が効かなくなる (IER が 0 だと IPR も立たない)。
            const x68k::u8 timerCD =
                static_cast<x68k::u8>(x68k::Mfp::kIntTimerC | x68k::Mfp::kIntTimerD);
            m.mfp().write(x68k::Mfp::kIerb, timerCD);
            m.mfp().write(x68k::Mfp::kImrb, timerCD);
            m.mfp().write(x68k::Mfp::kTcdcr, 0x51);  // C=分周 50, D=分周 4
            m.mfp().write(x68k::Mfp::kTcdr, 200);
            m.mfp().write(x68k::Mfp::kTddr, 3);

            // RTC の 1 秒境界 (10,000,000) と CRTC の垂直帰線
            // (162,342) を両方跨ぐだけ回す。ここに届かないと、
            // 遅い側の繰り上がりが壊れていても気づけない。
            //
            // 1 スライスを素数にして、閾値をちょうど跨ぐ位相と跨がない
            // 位相の両方を通す。
            x68k::u64 spent = 0;
            while (spent < 10500000)
            {
                spent += m.run(9973);
            }

            return std::vector<x68k::u32>{
                m.mfp().read(x68k::Mfp::kTcdr),
                m.mfp().read(x68k::Mfp::kTddr),
                m.mfp().peek(x68k::Mfp::kIprb),
                static_cast<x68k::u32>(m.crtc().inVerticalBlank() ? 1 : 0),
                m.crtc().rasterNumber(),
                m.rtc().read(x68k::Rtc::kSecond1),
                m.rtc().read(x68k::Rtc::kSecond10),
                static_cast<x68k::u32>(spent),
            };
        };

        const std::vector<x68k::u32> fast = runWith(true);
        const std::vector<x68k::u32> slow = runWith(false);
        CHECK(fast == slow);

        // 素通りのテストになっていないこと。全部 0 同士の一致では何も
        // 守れないので、3 つのデバイスが**それぞれ**動いた証拠を確かめる。
        //
        // ここを 1 つでも省くと、そのデバイスへ FastPath を渡し忘れる
        // 配線ミスを見逃す (両側とも既定の経路を通って一致してしまう)。
        REQUIRE(fast.size() == 8);

        // MFP: タイマの割り込みが保留になっている。
        CHECK(fast[2] != 0);

        // CRTC: ラスタ番号が動いている。reset 直後は 0。
        CHECK(fast[4] != 0);

        // RTC: 1 秒ぶん進んでいる。reset 直後は 0 秒。
        const x68k::u32 seconds = fast[6] * 10 + fast[5];
        CHECK(seconds >= 1);
    }
}
