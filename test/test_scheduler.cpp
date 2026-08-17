// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// イベント駆動 (docs/knowledge/event-driven-implementation.md) が、
// 毎命令 tick と **1 サイクルもずれない** こと。
//
// このファイルが守るのは速さではない。「次に状態が変わる時点まで飛ばす」は、
// 飛ばす先を 1 つでも間違えると quantum とまったく同じ観測可能なずれを
// 作る。test_device_timing.cpp が 3 度撤回させたのと同じ誤りを、
// 飛ばす側の実装で 4 度目に犯さないためのテスト。
//
// 各テストには「これを壊すと落ちる変異」を書いてある。落ちない変異しか
// 書けないテストは価値が無い。

#include <iterator>
#include <type_traits>
#include <vector>

#include "dev/mfp.h"
#include "dev/rtc.h"
#include "dev/video.h"
#include "machine.h"
#include "scheduler.h"
#include "doctest.h"

namespace
{

// NOP を敷き詰めた RAM を用意して Machine を立てる補助。
//
// Why not デバイスを直接叩かないか: イベント駆動は Machine の run() 経路に
// しかない。デバイス単体を叩くテストは、飛ばす配線が丸ごと抜けていても通る。
struct Fixture
{
    static constexpr x68k::u32 kCodeBegin = 0x400;
    static constexpr x68k::u32 kCodeEnd = 0x8000;

    std::vector<x68k::u8>& ram()
    {
        static std::vector<x68k::u8> storage(x68k::kMainRamSize, 0);
        return storage;
    }

    void poke16(x68k::u32 addr, x68k::u16 v)
    {
        ram()[addr] = static_cast<x68k::u8>(v >> 8);
        ram()[addr + 1] = static_cast<x68k::u8>(v & 0xFF);
    }

    // code は kCodeBegin から並べる命令語。残りは NOP で埋め、末尾の BRA で
    // 先頭へ戻す。PC が NOP の海から出ないので、いくら回しても安全。
    void build(const std::vector<x68k::u16>& code)
    {
        std::fill(ram().begin(), ram().end(), 0);
        poke16(0, 0x0000);
        poke16(2, 0x8000);  // SSP
        poke16(4, 0x0000);
        poke16(6, kCodeBegin);  // PC
        x68k::u32 at = kCodeBegin;
        for (const x68k::u16 word : code)
        {
            poke16(at, word);
            at += 2;
        }
        for (x68k::u32 a = at; a < kCodeEnd; a += 2)
        {
            poke16(a, 0x4E71);  // NOP (4 サイクル)
        }
        const x68k::u32 braAt = kCodeEnd - 4;
        poke16(braAt, 0x6000);
        poke16(braAt + 2, static_cast<x68k::u16>(kCodeBegin - (braAt + 2)));
    }

    void attach(x68k::Machine& m, bool eventDriven)
    {
        x68k::MemoryMap map{};
        map.mainRam = ram().data();
        m.setMemory(map);
        m.setEventDriven(eventDriven);
        m.reset();
    }
};

// 外から見えるデバイスの状態をまとめて取る。
//
// 「一致した」の中身をここに集約するのは、比較対象を増やし忘れる事故を
// 1 箇所に閉じ込めるため。rasterNumber() を入れてあるのが要点で、CRTC を
// 飛ばしにかかると 317 サイクル粒度のずれがここで出る。
std::vector<x68k::u32> observe(x68k::Machine& m)
{
    return {
        m.mfp().read(x68k::Mfp::kTadr),
        m.mfp().read(x68k::Mfp::kTbdr),
        m.mfp().read(x68k::Mfp::kTcdr),
        m.mfp().read(x68k::Mfp::kTddr),
        m.mfp().peek(x68k::Mfp::kIpra),
        m.mfp().peek(x68k::Mfp::kIprb),
        m.mfp().peek(x68k::Mfp::kGpip),
        static_cast<x68k::u32>(m.crtc().inVerticalBlank() ? 1 : 0),
        m.crtc().rasterNumber(),
        m.rtc().read(x68k::Rtc::kSecond1),
        m.rtc().read(x68k::Rtc::kSecond10),
        static_cast<x68k::u32>(m.cpu().state().a[7]),
        static_cast<x68k::u32>(m.cpu().state().pc),
        static_cast<x68k::u32>(m.cpu().state().interruptMask()),
        // ゲストが読み取った値の累積。
        //
        // **これが無いと実体化のテストが素通りする。** 最後の状態だけを
        // 比べると、ゲストが途中で読んだ値がどれだけ違っていても、
        // スライスの終わりに全部 settle されて最終状態は一致してしまう。
        // 実際に materialize() を外す変異を入れて 1 件も落ちなかった。
        // ゲストが「見た」ものをゲスト自身に記録させ、それを比べる。
        static_cast<x68k::u32>(m.cpu().state().d[0]),
        static_cast<x68k::u32>(m.cpu().state().d[1]),
    };
}

// MOVE.B $E88021,D0 (TBDR を読む) の命令語。ゲストがカウンタをポーリングする
// 形をそのまま作る。$1039 = MOVE.B (xxx).L, D0。
constexpr x68k::u16 kMoveTbdrToD0[3] = {0x1039, 0x00E8, 0x8021};

// observe() が返した 2 つを要素ごとに比べる。
//
// Why not vector 同士の == だけにしないか: doctest は vector の中身を
// 表示できないので「{?} == {?}」としか出ず、どの観測点がずれたのか分から
// ない。ずれた場所が分からない同値テストは、落ちたときに直せない。
void checkSame(const std::vector<x68k::u32>& evented, const std::vector<x68k::u32>& plain)
{
    static const char* const kNames[] = {
        "TADR",
        "TBDR",
        "TCDR",
        "TDDR",
        "IPRA",
        "IPRB",
        "GPIP",
        "VBL",
        "RASTER",
        "SEC1",
        "SEC10",
        "SSP",
        "PC",
        "IMASK",
        "D0",
        "D1",
        // 以降はテストごとに足す追加の観測点。
        "EXTRA0",
        "EXTRA1",
        "EXTRA2",
        "EXTRA3",
    };
    REQUIRE(evented.size() == plain.size());
    // 名前の表が短いと、observe() を増やしたときに範囲外を読む。
    REQUIRE(plain.size() <= std::size(kNames));
    for (std::size_t i = 0; i < plain.size(); ++i)
    {
        CAPTURE(i);
        CAPTURE(kNames[i]);
        CHECK(evented[i] == plain[i]);
    }
}

}  // namespace

TEST_SUITE("イベント駆動")
{
    // T6: Settled は偽造できない。
    //
    // 検出できる変異: Settled() {} を Settled() = default; へ変える。
    //
    // なぜ必要か: この設計で最も静かな失敗モード。private な defaulted
    // default ctor は m.read({}, reg) のような値初期化を通してしまうので、
    // 「型で守ったつもりが守れていない」状態がコンパイル成功のせいで
    // 誰にも気づかれない。
    TEST_CASE("実体化の証明は既定構築できない")
    {
        CHECK_FALSE(std::is_default_constructible_v<x68k::Settled>);
        // コピーはできる。証明を受け取った側が持ち回れないと、
        // read/write へ渡す形にならない。
        CHECK(std::is_copy_constructible_v<x68k::Settled>);
    }

    // T1: 遅延中に TBDR を読むと実体化される。
    //
    // 実測設定と同じにする: タイマ B は分周 4 (8 CPU サイクルに 1 減る) で
    // 動かし、**IERA bit0 は落としたまま**にする。割り込みを上げないので
    // 期限には入らないが、ゲストは 8 サイクルごとに減るカウンタとして
    // 読める。ここが設計の核心で、実体化を落とすと quantum を入れたときと
    // まったく同じずれが読み出し側から再発する。
    //
    // 検出できる変異:
    //   - ioRead8 の kMfpBase から materialize() を外す
    //   - Machine::materialize() の中身を空にする
    //   - Mfp::cyclesUntilNextRaise から IER の判定を外す
    //     (タイマ B が期限に入り、飛ばさなくなるので偽陽性で通ってしまう)
    TEST_CASE("割り込みを上げないタイマも読み出しはサイクル単位で正しい")
    {
        Fixture fx;
        // ゲスト: TBDR を読んで D0 へ入れ、D1 へ足し込むのを繰り返す。
        //
        // 足し込むのが要点。読んだ値をどこにも残さないと、途中でどれだけ
        // 違う値を読んでも最終状態はスライス終端の settle で一致してしまい、
        // 実体化を外す変異が 1 件も落ちない (実際に踏んだ)。
        // $D280 = ADD.L D0,D1。
        fx.build({kMoveTbdrToD0[0], kMoveTbdrToD0[1], kMoveTbdrToD0[2], 0xD280});

        const auto runOnce = [&](bool eventDriven)
        {
            x68k::Machine m;
            fx.attach(m, eventDriven);

            // タイマ B: 分周 4、データ 200。
            //
            // 順序が要点。データレジスタの書き込みがメインカウンタへ即座に
            // 入るのは **タイマが停止しているときだけ** (mfp.cpp の
            // loadTimerIfStopped)。先に TBCR で走らせると timerValue_ は
            // reset 直後の 0 のままになり、以後の比較が意味を失う。
            m.mfp().write(x68k::Mfp::kTbdr, 200);
            m.mfp().write(x68k::Mfp::kTbcr, 0x01);

            // IERA bit0 (タイマ B) は落としたまま。代わりにタイマ C を
            // 割り込み源として動かし、期限が「タイマ B より遥かに遠い」
            // 状況を作る。これが無いと期限が短くて飛ばず、素通りになる。
            m.mfp().write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
            m.mfp().write(x68k::Mfp::kTcdr, 200);
            m.mfp().write(x68k::Mfp::kTcdcr, 0x70);  // C=分周 200

            // IERA bit0 が落ちていることを明示的に確かめる。ここが 1 だと
            // タイマ B が本物のイベント源になり、テストの前提が崩れる。
            REQUIRE((m.mfp().peek(x68k::Mfp::kIera) & x68k::Mfp::kIntTimerB) == 0);

            x68k::u64 spent = 0;
            while (spent < 300000)
            {
                spent += m.run(9973);
            }
            return observe(m);
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // 素通りでないこと。TBDR が動いていなければ何も守れていない。
        REQUIRE(plain.size() >= 2);
        CHECK(plain[1] != 200);
        CHECK(plain[1] != 0);
    }

    // T2: TCDCR の書き換えで、溜まった時間が **旧分周で** 消化される。
    //
    // 分周 200 で走らせて時間を溜め、分周 4 へ書き替える。溜まったぶんが
    // 新分周で消化されると 50 倍の速さで減り、偽の割り込みが連発する。
    //
    // 検出できる変異:
    //   - ioWrite8 の kMfpBase から materialize() を外す (1 段目)
    //   - Rearm を受け取らない形にする / requestRearm を空にする (3 段目)
    //   - Scheduler::wake() から span_ = pending() を外す
    //     (実体化済みのぶんをもう一度デバイスへ渡す)
    TEST_CASE("タイマ制御の書き換えは溜まった時間を旧設定へ適用してから効く")
    {
        // ゲスト: 分周 200 で時間を溜めてから分周 64 へ書き替え、TCDR を
        // 読んで積分し、分周 200 へ戻す。これを延々と繰り返す。
        //
        //   NOP x 512                 ← ここで時間が溜まる (1 周目は分周 200、以後 100)
        //   MOVE.B #$57,$E8801D       ← C を分周 64 へ (D は分周 200 のまま)
        //   MOVE.B $E88023,D0         ← TCDR を読む
        //   ADD.L D0,D1               ← 積分する
        //   MOVE.B #$67,$E8801D       ← C を分周 100 へ戻す
        //   BRA.W 先頭
        //
        // 戻し先を初期設定 ($77) と違う値 ($67) にしてある。走り終えた
        // 時点の TCDCR がどちらでもないなら、ゲストの書き込みが 1 度も
        // 届いていないことになる (下の REQUIRE がそれを見る)。
        //
        // **書き込みの手前で時間を溜めるのが要点**。書き込みが走る時点で
        // 未実体化ぶんが 0 だと、settle を外す変異が何も変えない。実際、
        // 最初の版は書き込みを命令 1 個目に置いていて変異が生き残った。
        //
        // **分周 4 を使わないのも要点**。分周 4 は期限を 8 サイクルへ縮め、
        // kMinDeadline 縮退が働いて毎命令 settle になる。溜まる量が 1 命令
        // ぶんになり、これも変異を素通りさせる。分周 64 なら期限は
        // 200 x 64 x 2 = 25,600 サイクルで、縮退しない。
        //
        // settle を外すと、溜まった 2,048 サイクル (= 1,024 MFP サイクル) が
        // 分周 200 ではなく分周 64 で消化され、16 回ぶん余分に減る。
        // ゲストが読む TCDR にそのまま出て D1 が変わる。
        std::vector<x68k::u16> code(512, 0x4E71);  // NOP x 512 = 2,048 サイクル
        const std::vector<x68k::u16> tail = {
            0x13FC, 0x0057, 0x00E8, 0x801D,  // MOVE.B #$57,$E8801D (TCDCR)
            0x1039, 0x00E8, 0x8023,          // MOVE.B $E88023,D0   (TCDR)
            0xD280,                          // ADD.L D0,D1
            0x13FC, 0x0067, 0x00E8, 0x801D,  // MOVE.B #$67,$E8801D (TCDCR)
        };
        code.insert(code.end(), tail.begin(), tail.end());
        // 先頭へ戻る BRA.W。変位は「BRA の置き場所 + 2」から数える。
        //
        // Why not BRA.S にしないか: ループが 154 バイトあり、8bit の変位
        // (-128..127) に収まらない。収まらない変位を BRA.S へ詰めると、
        // 下位 8bit だけが残って別の場所へ飛ぶ。
        const x68k::u32 braAt = Fixture::kCodeBegin + static_cast<x68k::u32>(code.size()) * 2;
        code.push_back(0x6000);
        code.push_back(static_cast<x68k::u16>(Fixture::kCodeBegin - (braAt + 2)));

        const auto runOnce = [&](bool eventDriven)
        {
            Fixture fx;
            fx.build(code);

            x68k::Machine m;
            fx.attach(m, eventDriven);

            m.mfp().write(x68k::Mfp::kTcdr, 200);
            m.mfp().write(x68k::Mfp::kTddr, 200);
            m.mfp().write(x68k::Mfp::kIerb,
                          static_cast<x68k::u8>(x68k::Mfp::kIntTimerC | x68k::Mfp::kIntTimerD));
            m.mfp().write(x68k::Mfp::kTcdcr, 0x77);  // C=分周 200, D=分周 200

            // 1 回のスライスで回す。分割すると armDeadline がスライス終端で
            // 期限を切り詰め、溜まる量が短くなって感度が落ちる。
            const x68k::u32 spent = m.run(500000);

            // **ゲストの書き込みが本当に届いたことを確かめる。**
            //
            // ここを見ないと、命令語を 1 ビット間違えて「実は何も書いて
            // いない」テストが緑のまま通る。実際に踏んだ: MOVE.B #imm,(xxx).L
            // を $11FC (= (xxx).W) と書いており、$00E8 番地へ書いていた。
            // 書き込み経路を確かめるテストが、書き込みを一度もしていなかった。
            //
            // 初期設定は $77。ゲストが書くのは $57 と $67 だけなので、
            // 走り終えた時点でどちらかになっていなければ 1 度も届いていない。
            const x68k::u8 tcdcr = m.mfp().peek(x68k::Mfp::kTcdcr);
            REQUIRE((tcdcr == 0x57 || tcdcr == 0x67));

            std::vector<x68k::u32> out = observe(m);
            out.push_back(spent);
            return out;
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // 素通りでないこと。D1 (読んだ TCDR の積分) が 0 なら、ゲストが
        // 一度も読んでいないか、読んだ値が全部 0 で、何も守っていない。
        CHECK(plain[15] != 0);
        // 書き替えが実際に効いて、タイマが動いた証拠。
        CHECK(plain[2] != 200);
    }

    // T7: IERA bit0 を立てて期限を 8 サイクルへ縮めても破綻しない。
    //
    // タイマ B を分周 4 (8 CPU サイクル) で **割り込み付き** に動かすと、
    // 期限が kMinDeadline (256) を割る。ここで縮退が効かないと、期限を
    // 張り直すコストが利得を上回って現行より遅くなる。速度は実機で測るが、
    // **状態が毎命令 tick 版と一致すること**はここで固定できる。
    //
    // 検出できる変異: Scheduler::armDeadline の delta < kMinDeadline の
    // 判定を消す (縮退しなくなる)。
    TEST_CASE("期限が近すぎるときの縮退が状態を変えない")
    {
        Fixture fx;
        fx.build({});

        const auto runOnce = [&](bool eventDriven)
        {
            x68k::Machine m;
            fx.attach(m, eventDriven);

            m.mfp().write(x68k::Mfp::kTbdr, 2);
            m.mfp().write(x68k::Mfp::kIera, x68k::Mfp::kIntTimerB);
            m.mfp().write(x68k::Mfp::kTbcr, 0x01);  // 分周 4 = 8 CPU サイクル

            // 期限は 2 * 4 * 2 = 16 サイクル。kMinDeadline (256) を大きく
            // 下回るので、必ず縮退側へ落ちる。
            REQUIRE(m.mfp().cyclesUntilNextRaise() < x68k::Scheduler::kMinDeadline);

            x68k::u64 spent = 0;
            while (spent < 200000)
            {
                spent += m.run(9973);
            }
            return observe(m);
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // IPRA にタイマ B が立っていること (素通りでない証拠)。
        CHECK((plain[4] & x68k::Mfp::kIntTimerB) != 0);
    }

    // T8: halt しても未実体化の時間が失われない。
    //
    // 検出できる変異: runEventDriven の used == 0 経路から settle() を
    // 外す。halt した瞬間だけデバイスの時刻が巻き戻る。
    TEST_CASE("CPU が halt してもデバイスの時間が失われない")
    {
        Fixture fx;
        // NOP を 500 個ほど並べたあと未実装命令 ($4AFC = ILLEGAL) で止める。
        // 期限まで飛べる距離を稼いでから halt させたいので、手前に間を置く。
        std::vector<x68k::u16> code(500, 0x4E71);
        code.push_back(0x8140);  // 未実装の組み合わせ (OR opmode 5 / mode 0) で halt する

        const auto runOnce = [&](bool eventDriven)
        {
            x68k::Machine m;
            fx.build(code);
            fx.attach(m, eventDriven);

            m.mfp().write(x68k::Mfp::kTcdr, 200);
            m.mfp().write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
            m.mfp().write(x68k::Mfp::kTcdcr, 0x70);

            const x68k::u32 spent = m.run(100000);
            REQUIRE(m.isHalted());
            std::vector<x68k::u32> out = observe(m);
            out.push_back(spent);
            return out;
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // 消費サイクルが 0 でないこと。halt が最初の命令で起きていたら
        // 「未実体化の時間」が存在せず、このテストは何も守らない。
        CHECK(plain.back() > 1000);
    }

    // T9: STOP 中もタイマが進み、割り込みで抜ける。
    //
    // STOP は 0 でなく 4 を返す (m68k.cpp)。ホットループが「used == 0 で
    // 抜ける」形だと STOP 中に call8 を延々と回して劣化するので、
    // reachSlow の中で stopped を見て期限まで一括で飛ばしている。
    //
    // 検出できる変異:
    //   - reachSlow の STOP 一括飛ばしを消す (状態は一致するが、これは
    //     下の「飛ばした証拠」の CHECK が落ちる)
    //   - skipStopped が settle を通さずに now_ だけ進める (タイマが
    //     止まり、STOP から永久に抜けない = テストがタイムアウトする)
    //   - settle の kMaxSettleChunk の刻みを外す (CRTC が 1 フレーム以上を
    //     一度に受け取り、垂直帰線エッジが消える)
    TEST_CASE("STOP 中もタイマが進み、割り込みで抜ける")
    {
        // タイマ C の割り込みベクタ。MFP は VR の上位 4bit と割り込み番号
        // (グループ B のタイマ C は 5) を組み合わせた番号を返す。
        // VR=$40 なので $45 = 69、ベクタテーブルの位置は 69*4 = $114。
        constexpr x68k::u32 kTimerCVector = 0x114;
        // ハンドラの置き場所。NOP の海の外へ出す。
        constexpr x68k::u32 kHandler = 0x9000;

        // STOP 中に一括で飛び越したサイクル数。飛ばしたこと自体を
        // 押さえるための出口 (状態は飛ばしても同じになるので、同値だけでは
        // 「飛び越しを消す」変異が捕まらない)。
        x68k::u64 skipped = 0;

        // スライスの長さを 2 通りで試す。
        //
        // 短い方 (9,973) は普段の実機と同じ形。長い方 (400,003) は
        // **1 フレーム (180,342 サイクル) を超える飛び越し**を起こす。
        // settle の刻み (kMaxSettleChunk) を外すと、CRTC が 1 フレーム
        // 以上をまとめて受け取り、その間の垂直帰線の開始と終了を
        // 丸ごと落とす (video.h の tickFast のコメント)。
        // 短い方だけだと期限がスライス終端で切り詰められ、飛び越しが
        // 常にフレーム未満に収まってこの経路を一度も通らない。
        //
        // どちらも素数にして、閾値をちょうど跨ぐ位相と跨がない位相の
        // 両方を通す。
        x68k::u32 slice = 0;
        for (const x68k::u32 sliceLength : {9973u, 400003u})
        {
            slice = sliceLength;
            CAPTURE(slice);

            const auto runOnce = [&](bool eventDriven)
            {
                Fixture fx;
                // ゲスト: STOP #$2000 (スーパーバイザ・割り込み許可) と、
                // その手前へ戻る BRA。割り込みで抜けてハンドラへ飛び、RTE で
                // STOP の次 (BRA) へ戻り、また STOP へ入る。
                // 実機の「何もしないで割り込みを待つ」形をそのまま作る。
                //
                // Why not STOP 1 つだけ置かないか: RTE の戻り先は STOP の
                // **次の命令** なので、そのままだと NOP の海へ出て二度と
                // STOP へ入らない。STOP の飛び越しが 1 度しか通らず、
                // 「毎回の飛び越しが正しい」を確かめられない。
                //
                // $4E72 が STOP、次のワードが SR へ入れる値。
                // $60FA が BRA.S -6。変位は「BRA の置き場所 + 2」からなので、
                // 0x406 - 6 = 0x400 = STOP の先頭へ戻る。
                fx.build({0x4E72, 0x2000, 0x60FA});

                // ハンドラは配送回数を D3 で数え、MFP の IPRB を落として RTE。
                // 落とさないと保留が残り続け、抜けたそばから再入する。
                //
                //   $5283              ADDQ.L #1,D3     (配送回数)
                //   $13FC $00DF $00E8 $800D  MOVE.B #$DF,$E8800D (IPRB。書いた
                //                            ビットが 0 になる = ~kIntTimerC)
                //   $4E73              RTE
                //
                // 回数を数えるのが要点。デバイスの最終状態だけを見ると、
                // タイマの周期の整数倍で走り終えたときに「reset 直後と同じ値」に
                // 戻り、素通りのテストになる (実際に踏んだ: 1,600,080 サイクルは
                // 周期 400 のちょうど 4000 倍で TCDR が 200 へ戻った)。
                const std::vector<x68k::u16> handler = {0x5283, 0x13FC, 0x00DF,
                                                        0x00E8, 0x800D, 0x4E73};
                x68k::u32 at = kHandler;
                for (const x68k::u16 word : handler)
                {
                    fx.poke16(at, word);
                    at += 2;
                }

                fx.poke16(kTimerCVector + 0, 0x0000);
                fx.poke16(kTimerCVector + 2, kHandler);

                x68k::Machine m;
                fx.attach(m, eventDriven);

                m.mfp().write(x68k::Mfp::kVr, 0x40);
                // タイマ C で抜ける。分周 200 x データ 200 x 2 = 80,000 サイクル。
                m.mfp().write(x68k::Mfp::kTcdr, 200);
                m.mfp().write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
                m.mfp().write(x68k::Mfp::kImrb, x68k::Mfp::kIntTimerC);
                m.mfp().write(x68k::Mfp::kTcdcr, 0x70);

                x68k::u64 spent = 0;
                // 80,000 サイクル周期の割り込みを 15 回以上跨ぐ。
                // 長いスライスでも 3 回は回るようにして、「スライスの入口で
                // 既に STOP」という経路を必ず通す。
                while (spent < 1300000)
                {
                    spent += m.run(slice);
                }
                std::vector<x68k::u32> out = observe(m);
                out.push_back(static_cast<x68k::u32>(m.cpu().state().stopped ? 1 : 0));
                out.push_back(static_cast<x68k::u32>(m.cpu().state().d[3]));
                skipped = m.stopSkippedCycles();
                return out;
            };

            const std::vector<x68k::u32> plain = runOnce(false);
            const x68k::u64 plainSkipped = skipped;
            const std::vector<x68k::u32> evented = runOnce(true);
            const x68k::u64 eventedSkipped = skipped;
            checkSame(evented, plain);

            // 素通りでないこと。
            //
            // STOP へ実際に入り、タイマが回って割り込みで抜けたことを
            // **回数** で見る。
            //
            // Why not 最終状態で見ないか: タイマの周期の整数倍で走り終えると
            // TCDR が reset 直後と同じ値へ戻り、SSP も RTE で戻る。実際に
            // 1,600,080 サイクル (周期 400 のちょうど 4000 倍) で全部が
            // 初期値と一致し、素通りのテストになった。
            //
            // 1,300,000 サイクル / 80,000 サイクル周期 = 16 回以上。
            CHECK(plain.back() >= 15);

            // **飛ばしていること自体を固定する。**
            //
            // 状態は飛ばしても飛ばさなくても同じになるので、同値テストでは
            // 「飛び越しを消す」変異が捕まらない (実際に生き残った)。
            // 飛ばしたサイクル数を数えて、そこを直接押さえる。
            //
            // ゲストは 1,300,000 サイクルのうちほとんどを STOP で過ごすので、
            // その大半が飛び越しで消化されているはず。
            CHECK(eventedSkipped > 600000);
            // 毎命令 tick 版は 1 サイクルも飛ばさない。
            CHECK(plainSkipped == 0);
        }
    }

    // 1 フレームを超える飛び越しでも、垂直帰線のエッジが消えないこと。
    //
    // Crtc::tickFast は 1 フレーム (180,342 サイクル) 以上をまとめて
    // 渡されると、その間に通過した垂直帰線の開始と終了を **報告しない**
    // (video.h の tickFast のコメントが明言している)。だから settle は
    // kMaxSettleChunk (65,536) で刻んでから渡す。
    //
    // その刻みが実際に効く経路は STOP の一括飛び越しだけ。通常の実行では
    // armDeadline が kFallbackSpan で刻むので 1 フレームを超える値が
    // 渡らない。skipStopped は nextEventCycle とスライス終端から直接
    // target を出すので、**割り込み源が 1 つも無い STOP** で長いスライスを
    // 回すと、1 回の飛び越しがフレームを超える。
    //
    // 検出できる変異:
    //   - settle の kMaxSettleChunk の刻みを外す
    //   - kMaxSettleChunk を kCyclesPerFrame 以上にする
    TEST_CASE("フレームを超える飛び越しでも垂直帰線が消えない")
    {
        const auto runOnce = [&](bool eventDriven)
        {
            Fixture fx;
            // ゲスト: STOP #$2000 に入って二度と出てこない。
            // タイマを 1 本も動かさないので、割り込みは永久に来ない。
            fx.build({0x4E72, 0x2000});

            x68k::Machine m;
            fx.attach(m, eventDriven);

            // MFP のタイマは全部止める。期限は CRTC の垂直帰線と RTC だけ。
            m.mfp().write(x68k::Mfp::kTcdcr, 0x00);

            // **1 スライス 400,003 サイクル**。1 フレーム (180,342) の
            // 2 倍を超えるので、刻まないと帰線の開始と終了が丸ごと消える。
            // 素数にして、フレーム境界とスライス境界の位相をずらす。
            x68k::u64 spent = 0;
            while (spent < 2000000)  // 11 フレームぶん
            {
                spent += m.run(400003);
            }

            std::vector<x68k::u32> out = observe(m);
            out.push_back(static_cast<x68k::u32>(m.cpu().state().stopped ? 1 : 0));
            return out;
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        // GPIP4 (帰線の状態) とフレーム内位置が 1 サイクルも違わないこと。
        // 刻まないと、帰線に入ったまま出てこない / 出たまま入らない、が起きる。
        checkSame(evented, plain);

        // 素通りでないこと。STOP に入ったまま走り終えていること。
        CHECK(plain.back() == 1);
    }

    // 期限そのものの精度。**割り込みが配送された瞬間**をゲストが数える。
    //
    // これがこのファイルで最も感度の高いテスト。ゲストは NOP を回りながら
    // D2 を数え続け、割り込みハンドラがそのときの D2 を D3 へ足し込む。
    // 期限が 1 命令ぶんでもずれると、配送の瞬間の D2 が変わり D3 が変わる。
    //
    // Why not レジスタのポーリングで測らないか: ゲストが $E88000 台を読むと
    // materialize() が必ず走るので、**期限がどれだけずれていても読み値は
    // 正しい**。読み出しで測ろうとした版は、期限の計算を壊す変異を 1 件も
    // 捕まえられなかった。期限の精度が単独で効くのは割り込みの配送だけ。
    //
    // **スライスを 1 回にするのも要点**。9973 サイクルのスライスを繰り返すと
    // armDeadline がスライス終端で期限を切り詰めるので、期限の誤差が
    // 終端に隠れる。
    //
    // 検出できる変異:
    //   - Mfp::cyclesUntilNextRaise から prescaleCounter_ の差し引きを外す
    //   - decrements の「0 は 256」を value そのままにする
    //   - kCpuToMfpShift のシフト (MFP → CPU の 2 倍) を落とす
    //   - reachSlow から serviceInterrupts() を外す
    TEST_CASE("割り込みが配送された瞬間をゲストが数えて期限の精度を確かめる")
    {
        constexpr x68k::u32 kTimerCVector = 0x114;  // VR=$40, タイマ C は番号 5
        constexpr x68k::u32 kTimerDVector = 0x110;  // タイマ D は番号 4
        constexpr x68k::u32 kHandler = 0x9000;

        const auto runOnce = [&](bool eventDriven)
        {
            Fixture fx;
            // ゲスト: 割り込みマスクを下げてから D2 を数え続けるループ。
            //   $46FC $2000  MOVE.W #$2000,SR (スーパーバイザ・マスク 0)
            //   $5282        ADDQ.L #1,D2
            //   $60FC        BRA.S -4 (ADDQ へ戻る)
            //
            // SR を下げるのが要点。reset 直後のマスクは 7 で、下げないと
            // MFP のレベル 6 が一度も受理されない。
            fx.build({0x46FC, 0x2000, 0x5282, 0x60FC});

            // ハンドラ: そのときの D2 を D3 へ足し込み、IPRB を落として RTE。
            //   $D682              ADD.L D2,D3
            //   $11FC $0000 $00E8 $800D   MOVE.B #0,$E8800D (IPRB を全部落とす)
            //   $4E73              RTE
            fx.poke16(kHandler + 0, 0xD682);
            fx.poke16(kHandler + 2, 0x13FC);
            fx.poke16(kHandler + 4, 0x0000);
            fx.poke16(kHandler + 6, 0x00E8);
            fx.poke16(kHandler + 8, 0x800D);
            fx.poke16(kHandler + 10, 0x4E73);

            for (const x68k::u32 vec : {kTimerCVector, kTimerDVector})
            {
                fx.poke16(vec + 0, 0x0000);
                fx.poke16(vec + 2, kHandler);
            }

            x68k::Machine m;
            fx.attach(m, eventDriven);

            m.mfp().write(x68k::Mfp::kVr, 0x40);
            // タイマ C (分周 200 x データ **256** x 2 = 102,400 サイクル周期) と
            // タイマ D (分周 100 x データ 37 x 2 = 7,400 サイクル周期)。
            // 周期を素な関係にして、片方の期限だけ正しくても通らないようにする。
            //
            // **タイマ C のデータレジスタを 0 にするのが要点**。MC68901 では
            // データレジスタの 0 は 256 を意味し、tickTimerCounted は 0 から
            // のデクリメントを 0xFF へ巻き戻して数え続ける。期限の計算が
            // これを「0 回で満了」と読むと、needMfp が桁溢れして期限が
            // 遥か先を指し、タイムアウトを丸ごと飛び越す。1 本を 0 に
            // しておかないと、その分岐を一度も通らないまま通る。
            const x68k::u8 timerCD =
                static_cast<x68k::u8>(x68k::Mfp::kIntTimerC | x68k::Mfp::kIntTimerD);
            m.mfp().write(x68k::Mfp::kTcdr, 0);
            m.mfp().write(x68k::Mfp::kTddr, 37);
            m.mfp().write(x68k::Mfp::kIerb, timerCD);
            m.mfp().write(x68k::Mfp::kImrb, timerCD);
            m.mfp().write(x68k::Mfp::kTcdcr, 0x76);  // C=分周 200, D=分周 100

            // **1 回のスライスで回す。** 分割すると armDeadline が
            // スライス終端で期限を切り詰め、期限の誤差がそこに隠れる。
            const x68k::u32 spent = m.run(500000);

            std::vector<x68k::u32> out = observe(m);
            out.push_back(static_cast<x68k::u32>(m.cpu().state().d[2]));
            out.push_back(static_cast<x68k::u32>(m.cpu().state().d[3]));
            out.push_back(spent);
            return out;
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // 素通りでないこと。
        //
        // D3 が 0 なら割り込みが一度も配送されておらず、期限の精度を
        // 何も確かめていない。
        CHECK(plain[17] != 0);
        // ループが十分回っていること。
        CHECK(plain[16] > 10000);
    }

    // T3: マスク中に立った保留が、マスクを下げた直後に配送される。
    //
    // **これが設計変更 1 (保留中フォールバック) を固定する唯一のテスト。**
    //
    // 配送の失敗は状態を変えない。serviceMfpInterrupt はマスク中に
    // acknowledgeInterrupt を呼ばずに false を返すので、IPR は立ったまま、
    // 新しいエッジは二度と来ない。エッジだけを頼りに期限を張ると、
    // マスクを下げても次のタイムアウト (80,000 サイクル後) まで配送されない。
    //
    // reachSlow の末尾で「保留があるのに 1 つも配送できなかったら期限を
    // 張らずに debt_ = 0 のままにする」ことで、次命令から毎命令リトライへ
    // 縮退させている。マスクが下りた瞬間の命令境界で配送される。
    //
    // 検出できる変異:
    //   - reachSlow の blocked 分岐を消して無条件に再 arm する
    //   - Scheduler::holdForPendingInterrupt() を空にする
    //   - blocked の判定から mfp_.hasPendingInterrupt() を落とす
    TEST_CASE("マスク中に立った割り込みがマスク解除の直後に配送される")
    {
        constexpr x68k::u32 kTimerCVector = 0x114;  // VR=$40, タイマ C は番号 5
        constexpr x68k::u32 kHandler = 0x9000;

        const auto runOnce = [&](bool eventDriven)
        {
            Fixture fx;
            // ゲスト:
            //   MOVEQ #1,D2        ← 1 から数え始める (0 だと「配送された」と
            //                        「D2 が 0 のとき配送された」を区別できない)
            //   MOVE.W #$2700,SR   ← マスクする (レベル 7)
            //   NOP x 4096         ← この間にタイマ C がタイムアウトする
            //   MOVE.W #$2000,SR   ← マスクを下げる
            //   ADDQ.L #1,D2       ← ここから数え始める
            //   BRA.S -4
            //
            // マスクを下げた **直後の命令境界** で配送されるはずなので、
            // ハンドラが記録する D2 はごく小さい値になる。フォールバックが
            // 無いと次のタイムアウトまで待たされ、D2 が桁違いに大きくなる。
            std::vector<x68k::u16> code = {0x7401, 0x46FC,
                                           0x2700};  // MOVEQ #1,D2 / MOVE.W #$2700,SR
            code.insert(code.end(), 4096, 0x4E71);   // NOP x 4096 = 16,384 サイクル
            const std::vector<x68k::u16> tail = {0x46FC, 0x2000, 0x5282, 0x60FC};
            code.insert(code.end(), tail.begin(), tail.end());

            fx.build(code);

            // ハンドラ: そのときの D2 を D3 へ足し、**タイマを止めて**
            // IPRB を落として RTE。
            //
            //   $D682              ADD.L D2,D3
            //   $13FC $0000 $00E8 $801D  MOVE.B #0,$E8801D (TCDCR = 停止)
            //   $13FC $0000 $00E8 $800D  MOVE.B #0,$E8800D (IPRB)
            //   $4E73              RTE
            //
            // タイマを止めるのが要点。止めないと 10,000 サイクルごとに
            // 配送され、D3 が回数ぶん積み上がる。「マスクを下げた直後に
            // 配送されたか」を見たいので、配送は 1 回だけにする。
            const std::vector<x68k::u16> handler = {0xD682, 0x13FC, 0x0000, 0x00E8, 0x801D,
                                                    0x13FC, 0x0000, 0x00E8, 0x800D, 0x4E73};
            x68k::u32 at = kHandler;
            for (const x68k::u16 word : handler)
            {
                fx.poke16(at, word);
                at += 2;
            }
            fx.poke16(kTimerCVector + 0, 0x0000);
            fx.poke16(kTimerCVector + 2, kHandler);

            x68k::Machine m;
            fx.attach(m, eventDriven);

            m.mfp().write(x68k::Mfp::kVr, 0x40);
            // タイマ C: 分周 50 x データ 100 x 2 = 10,000 サイクル周期。
            // NOP 4096 個 (16,384 サイクル) の途中で必ずタイムアウトする。
            m.mfp().write(x68k::Mfp::kTcdr, 100);
            m.mfp().write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
            m.mfp().write(x68k::Mfp::kImrb, x68k::Mfp::kIntTimerC);
            m.mfp().write(x68k::Mfp::kTcdcr, 0x40);  // C=分周 50, D=停止

            // タイマ C の周期 (10,000) の中間で終わる長さにする。ちょうど
            // 周期の境目で切ると、配送が「このスライスの最後」か「次の
            // スライスの最初」かで分かれ、境界の丸めだけで IPRB が食い違う。
            const x68k::u32 spent = m.run(95000);

            std::vector<x68k::u32> out = observe(m);
            out.push_back(static_cast<x68k::u32>(m.cpu().state().d[2]));
            out.push_back(static_cast<x68k::u32>(m.cpu().state().d[3]));
            out.push_back(spent);
            return out;
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // 素通りでないこと。
        //
        // 配送されたこと (D3 != 0)。0 なら、マスクを下げても一度も
        // 配送されておらず、フォールバックを何も確かめていない。
        CHECK(plain[17] != 0);
        // **マスクを下げた直後に配送されたこと。** D3 は「配送時点の D2」の
        // 積分なので、最初の 1 回が小さければ全体も小さい。ここが緩いと
        // 「80,000 サイクル後にようやく配送された」でも通ってしまう。
        CHECK(plain[17] < 20);
        // ループそのものは十分回っていること (配送が早いのは、待たされて
        // いないからであって、ループが回っていないからではない)。
        CHECK(plain[16] > 3000);
    }

    // 期限の張り直し (3 段目の rearm)。
    //
    // 割り込みハンドラが TCDCR で分周を書き替える。書き替えると **今
    // 走っているカウントダウンの速さが変わる** ので、期限も変わる。
    // 張り直さないと、古い分周で計算した期限まで割り込みが遅れる。
    //
    // Why not T2 (溜まった時間の消化) と同じテストで見ないか: T2 の
    // ゲストは書き替えの直後に TCDR を読むので、materialize() が走って
    // 状態が追いつく。期限が古いままでも読み値は正しく、rearm を外す変異が
    // 生き残った (実際に踏んだ)。**期限の古さが単独で効くのは、書き替えの
    // あと一度もレジスタを読まずに割り込みを待つ経路だけ。**
    //
    // 検出できる変異:
    //   - Scheduler::requestRearm() を空にする
    //   - ioWrite8 から sched_.rearm() を外す
    //   - Scheduler::wake() の debt_ = 0 を消す
    TEST_CASE("制御レジスタの書き換えで期限が張り直される")
    {
        constexpr x68k::u32 kTimerCVector = 0x114;  // VR=$40, タイマ C は番号 5
        constexpr x68k::u32 kTimerDVector = 0x110;  // タイマ D は番号 4
        constexpr x68k::u32 kHandler = 0x9000;

        const auto runOnce = [&](bool eventDriven)
        {
            Fixture fx;
            // ゲスト: マスクを下げ、D4 に最初の TCDCR 値を入れて数え続ける。
            //   $46FC $2000  MOVE.W #$2000,SR
            //   $7856        MOVEQ #$56,D4      (C=分周 64, D=分周 100)
            //   $5282        ADDQ.L #1,D2
            //   $60FC        BRA.S -4
            fx.build({0x46FC, 0x2000, 0x7856, 0x5282, 0x60FC});

            // ハンドラ: D2 を D3 へ積み、TCDCR を D4 の値で書き替え、
            // D4 を反転して次回に備え、IPRB を落として RTE。
            //   $D682              ADD.L D2,D3
            //   $13C4 $00E8 $801D  MOVE.B D4,$E8801D   (TCDCR)
            //   $0A04 $0033        EORI.B #$33,D4      ($56 <-> $65)
            //   $13FC $0000 $00E8 $800D  MOVE.B #0,$E8800D (IPRB)
            //   $4E73              RTE
            //
            // TCDCR を書き替えるのが要点。**今走っているカウントダウンの
            // 速さが変わる**ので、期限を張り直さないと次の割り込みが
            // 古い分周で計算した時点まで遅れる。
            const std::vector<x68k::u16> handler = {0xD682, 0x13C4, 0x00E8, 0x801D, 0x0A04, 0x0033,
                                                    0x13FC, 0x0000, 0x00E8, 0x800D, 0x4E73};
            x68k::u32 at = kHandler;
            for (const x68k::u16 word : handler)
            {
                fx.poke16(at, word);
                at += 2;
            }

            for (const x68k::u32 vec : {kTimerCVector, kTimerDVector})
            {
                fx.poke16(vec + 0, 0x0000);
                fx.poke16(vec + 2, kHandler);
            }

            x68k::Machine m;
            fx.attach(m, eventDriven);

            m.mfp().write(x68k::Mfp::kVr, 0x40);
            const x68k::u8 timerCD =
                static_cast<x68k::u8>(x68k::Mfp::kIntTimerC | x68k::Mfp::kIntTimerD);
            m.mfp().write(x68k::Mfp::kTcdr, 200);
            m.mfp().write(x68k::Mfp::kTddr, 37);
            m.mfp().write(x68k::Mfp::kIerb, timerCD);
            m.mfp().write(x68k::Mfp::kImrb, timerCD);
            m.mfp().write(x68k::Mfp::kTcdcr, 0x77);  // C=分周 200, D=分周 200

            const x68k::u32 spent = m.run(500000);

            // ゲストの書き替えが届いたこと。初期値 $77 のままなら 1 度も
            // 割り込みが来ていないか、ハンドラの書き込みが飛んでいる。
            const x68k::u8 tcdcr = m.mfp().peek(x68k::Mfp::kTcdcr);
            REQUIRE((tcdcr == 0x56 || tcdcr == 0x65));

            std::vector<x68k::u32> out = observe(m);
            out.push_back(static_cast<x68k::u32>(m.cpu().state().d[2]));
            out.push_back(static_cast<x68k::u32>(m.cpu().state().d[3]));
            out.push_back(spent);
            return out;
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // 素通りでないこと。D3 が 0 なら割り込みが一度も配送されていない。
        CHECK(plain[17] != 0);
        CHECK(plain[16] > 10000);
    }

    // 期限の計算式そのものを、値を決め打ちして確かめる。
    //
    // Why not 走らせて確かめるだけにしないか: 期限が遠すぎても、他の
    // デバイスの期限や kFallbackSpan (65,536) が先に settle を起こし、
    // 「次の settle で拾われた」形で誤りが隠れる。実際、データレジスタ 0
    // (= 256) の扱いを壊す変異は、Machine を走らせるテストでは 3 通り
    // 試して 3 通りとも生き残った。**式は式として直接押さえる。**
    //
    // 検出できる変異:
    //   - decrements の「0 は 256」を value そのままにする
    //   - prescaleCounter_ の差し引きを外す
    //   - kCpuToMfpShift のシフト (MFP → CPU の 2 倍) を落とす
    //   - timerRaises の IER 判定を外す (止まっているはずの本数が入る)
    TEST_CASE("次のタイムアウトまでのサイクル数の計算")
    {
        x68k::Mfp mfp;
        mfp.reset();

        // 期限が無い状態。走っているタイマが 1 本も無い。
        CHECK(mfp.cyclesUntilNextRaise() == x68k::Mfp::kNoDeadline);

        // タイマ C: 分周 200、データ 100。
        // 100 回減って満了するので 100 x 200 = 20,000 MFP サイクル。
        // CPU サイクルはその 2 倍で 40,000。
        mfp.write(x68k::Mfp::kTcdr, 100);
        mfp.write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
        mfp.write(x68k::Mfp::kTcdcr, 0x70);
        CHECK(mfp.cyclesUntilNextRaise() == 40000);

        // 分周器を途中まで進める。150 MFP サイクルぶん進めると、
        // 残りは 20,000 - 150 = 19,850 MFP サイクル = 39,700 CPU サイクル。
        mfp.tickFast<true>(300);  // 300 CPU = 150 MFP
        CHECK(mfp.cyclesUntilNextRaise() == 39700);

        // **データレジスタの 0 は 256。**
        // MC68901 の仕様で、0 は 256 を意味する (tickTimerCounted が
        // 0 からのデクリメントを 0xFF へ巻き戻して数え続ける)。
        // 256 x 200 x 2 = 102,400 CPU サイクル。
        //
        // ここを value そのままにすると 0 回で満了と読み、
        // 「0 x 200 - 分周器の残り」が桁溢れして期限が遥か先を指す。
        x68k::Mfp zero;
        zero.reset();
        zero.write(x68k::Mfp::kTcdr, 0);
        zero.write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
        zero.write(x68k::Mfp::kTcdcr, 0x70);
        CHECK(zero.cyclesUntilNextRaise() == 102400);

        // **IER が落ちているタイマは期限に入らない。** これが段 4 の核心。
        // X68000 はタイマ B を IERA bit0 = 0 のまま分周 4 で走らせるので、
        // ここを見ないと期限が 8 サイクルまで縮み、イベント駆動が成立しない。
        x68k::Mfp masked;
        masked.reset();
        masked.write(x68k::Mfp::kTbdr, 1);
        masked.write(x68k::Mfp::kTbcr, 0x01);  // 分周 4、IERA bit0 は 0 のまま
        CHECK(masked.cyclesUntilNextRaise() == x68k::Mfp::kNoDeadline);

        // IERA bit0 を立てると、途端に 8 サイクルの期限になる。
        masked.write(x68k::Mfp::kIera, x68k::Mfp::kIntTimerB);
        CHECK(masked.cyclesUntilNextRaise() == 8);
    }

    // 段 3: RTC の実体化。ゲストが秒レジスタをポーリングする。
    //
    // RTC は割り込みを持たないので、期限がずれても外からは見えない。
    // 見えるのは **読み出しの値** だけ。だから RTC については
    // 「期限の精度」ではなく「読み出しで実体化されること」を守る。
    //
    // 秒が繰り上がるのは 10,000,000 サイクルに 1 度。実体化を落とすと、
    // 秒の境界が期限ぶん遅れて見える。ゲストはポーリングループの反復回数
    // としてそれを観測できるので、「分解能より細かいから見えない」は
    // 成り立たない (quantum を 3 度撤回させたのと同じ理屈)。
    //
    // 検出できる変異:
    //   - ioRead8 の kRtcBase から materialize() を外す
    //   - Machine::materialize() の中身を空にする
    TEST_CASE("RTC の秒レジスタもサイクル単位で正しく読める")
    {
        const auto runOnce = [&](bool eventDriven)
        {
            Fixture fx;
            // ゲスト: 秒の 1 の位 ($E8A001) を読んで D1 へ足し込む。
            //   $1039 $00E8 $A001  MOVE.B $E8A001,D0
            //   $D280              ADD.L D0,D1
            //   $5282              ADDQ.L #1,D2
            //   $60F4              BRA.S -12
            fx.build({0x1039, 0x00E8, 0xA001, 0xD280, 0x5282, 0x60F4});

            x68k::Machine m;
            fx.attach(m, eventDriven);

            // MFP のタイマは全部止める。RTC の期限だけを効かせたい。
            m.mfp().write(x68k::Mfp::kTcdcr, 0x00);

            // 1 秒境界 (10,000,000 サイクル) を 1 回跨ぐ。
            //
            // **1 回のスライスで回す。** 9973 サイクルのスライスへ分けると、
            // 期限がスライス終端で切り詰められて溜まる量が 9973 で頭打ちに
            // なる。RTC は 10,000,000 サイクルに 1 度しか動かないので、
            // 溜まる量が小さいと実体化を外しても差が出にくい。
            const x68k::u32 spent = m.run(10500000);
            REQUIRE(spent > 10000000);

            std::vector<x68k::u32> out = observe(m);
            out.push_back(static_cast<x68k::u32>(m.cpu().state().d[2]));
            return out;
        };

        const std::vector<x68k::u32> plain = runOnce(false);
        const std::vector<x68k::u32> evented = runOnce(true);
        checkSame(evented, plain);

        // 素通りでないこと。1 秒ぶん進んでいること。
        const x68k::u32 seconds = plain[10] * 10 + plain[9];
        CHECK(seconds >= 1);
        // D1 は「読んだ秒の値」の積分。境界の前は 0 を足し続けるので、
        // 0 でないことが「境界を跨いだあとも読み続けた」証拠になる。
        CHECK(plain[15] != 0);
    }

    // 垂直帰線 (GPIP4) の 2 つのエッジが両方とも期限に入っていること。
    //
    // X68000 の垂直帰線割り込みは MFP の GPIP4 に入り、AER の値で
    // 「立ち上がりで上げる」か「立ち下がりで上げる」かが変わる
    // (mfp.cpp の setVerticalBlank)。つまり期限には **両方のエッジ** が
    // 要る。片方しか入れないと、帰線の開始か終了のどちらかを飛び越す。
    //
    // Human68k はこの割り込みでカーソル点滅とキー入力をさばくので、
    // ここが遅れると実機で「反応が鈍い」形の不具合になる。
    //
    // 検出できる変異:
    //   - Crtc::cyclesUntilVBlankEdge が inVBlank_ を見ずに常に
    //     表示期間の終わりだけを返す (帰線の **終わり** を飛び越す)
    //   - cyclesUntilVBlankEdge を kNoDeadline にする
    //   - nextEventCycle から CRTC の項を落とす
    TEST_CASE("垂直帰線の両方のエッジが期限に入っている")
    {
        // GPIP4 は IERB bit6。VR=$40 なので割り込み番号 6、ベクタ $46 = 70。
        constexpr x68k::u32 kGpip4Vector = 70 * 4;
        constexpr x68k::u32 kHandler = 0x9000;

        // AER で「どちらのエッジで上げるか」を切り替えて 2 通り試す。
        // 片方だけだと、入っていないエッジの期限が抜けていても通る。
        //
        // GPIP4 は **bit 4** (mfp.cpp の kGpipVDisp = $10)。bit 6 ($40) だと
        // 両方とも「立ち下がりで上げる」になり、帰線の終わりのエッジを
        // 一度も試さないまま通る (実際に踏んだ)。
        for (const x68k::u8 aer : {static_cast<x68k::u8>(0x00), static_cast<x68k::u8>(0x10)})
        {
            CAPTURE(aer);

            const auto runOnce = [&](bool eventDriven)
            {
                Fixture fx;
                // ゲスト: マスクを下げて D2 を数え続ける。
                fx.build({0x46FC, 0x2000, 0x5282, 0x60FC});

                // ハンドラ: D2 を D3 へ積み、IPRB を落として RTE。
                const std::vector<x68k::u16> handler = {0xD682, 0x13FC, 0x0000,
                                                        0x00E8, 0x800D, 0x4E73};
                x68k::u32 at = kHandler;
                for (const x68k::u16 word : handler)
                {
                    fx.poke16(at, word);
                    at += 2;
                }
                fx.poke16(kGpip4Vector + 0, 0x0000);
                fx.poke16(kGpip4Vector + 2, kHandler);

                x68k::Machine m;
                fx.attach(m, eventDriven);

                m.mfp().write(x68k::Mfp::kVr, 0x40);
                m.mfp().write(x68k::Mfp::kAer, aer);
                m.mfp().write(x68k::Mfp::kIerb, x68k::Mfp::kIntGpip4);
                m.mfp().write(x68k::Mfp::kImrb, x68k::Mfp::kIntGpip4);
                // タイマは全部止める。CRTC の期限だけを効かせたい。
                m.mfp().write(x68k::Mfp::kTcdcr, 0x00);

                // 1 フレーム = 180,342 サイクル。3 フレームぶん回して、
                // 帰線の開始と終了を 3 回ずつ跨ぐ。
                const x68k::u32 spent = m.run(550000);

                std::vector<x68k::u32> out = observe(m);
                out.push_back(static_cast<x68k::u32>(m.cpu().state().d[2]));
                out.push_back(static_cast<x68k::u32>(m.cpu().state().d[3]));
                out.push_back(spent);
                return out;
            };

            const std::vector<x68k::u32> plain = runOnce(false);
            const std::vector<x68k::u32> evented = runOnce(true);
            checkSame(evented, plain);

            // 素通りでないこと。配送が 1 度も無ければ何も守っていない。
            CHECK(plain[17] != 0);
            CHECK(plain[16] > 10000);
        }
    }

    // 段 1 の shadow 検証。期限を計算するが飛ばさない。
    //
    // 予測した「次に外から見える状態が変わるまでのサイクル数」が、実際に
    // 最初に変わったサイクルを **超えない** ことを 1 サイクル単位で確かめる。
    // 超えていたら、飛ばす実装はその変化を飛び越していた。
    //
    // 検出できる変異:
    //   - Crtc::cyclesUntilVBlankEdge が inVBlank_ を見ずに常に
    //     kCyclesPerFrame - frameCycles_ を返す (帰線の開始を飛び越す)
    //   - Mfp::cyclesUntilAnyTimerChange から prescaleCounter_ の
    //     差し引きを外す (分周器の途中経過を無視して遠くを指す)
    //   - Rtc::cyclesUntilCarry を kCyclesPerSecond の定数にする
    TEST_CASE("期限の予測が実際の変化を飛び越さない")
    {
        Fixture fx;
        fx.build({});

        x68k::Machine m;
        fx.attach(m, false);  // 飛ばさない。毎命令 tick のまま裏で予測する
        m.setShadowVerify(true);

        // 3 つの期限生成元をすべて動かす。1 つでも止まっていると、その
        // デバイスの予測式が間違っていても検証を素通りする。
        m.mfp().write(x68k::Mfp::kTbdr, 200);
        m.mfp().write(x68k::Mfp::kTbcr, 0x01);  // B=分周 4、IER は落としたまま
        m.mfp().write(x68k::Mfp::kTcdr, 200);
        m.mfp().write(x68k::Mfp::kIerb, x68k::Mfp::kIntTimerC);
        m.mfp().write(x68k::Mfp::kTcdcr, 0x70);  // C=分周 200

        x68k::u64 spent = 0;
        while (spent < 10500000)  // RTC の 1 秒境界と CRTC の垂直帰線を跨ぐ
        {
            spent += m.run(9973);
        }

        // 飛び越しが 1 度も起きていないこと。
        CHECK(m.shadowMismatches() == 0);
        // 素通りでないこと。突き合わせが 0 件なら何も検証していない。
        CHECK(m.shadowChecks() > 1000);
        // 予測が保守的すぎないこと。ここが 0 なら「常に手前を指す」実装でも
        // 上の 2 つが通ってしまい、飛ばす利得がまったく無い。
        CHECK(m.shadowExact() > 0);
    }

    // 縮退の判定そのもの。armDeadline が近い期限で degraded_ を立てること。
    //
    // 検出できる変異: kMinDeadline の判定を消す / kMinDeadline を 0 にする。
    TEST_CASE("近すぎる期限では毎命令へ縮退する")
    {
        x68k::Scheduler s;
        s.reset();
        s.beginSlice(100000);

        // 十分遠い期限は張られる。
        s.armDeadline(x68k::Scheduler::kMinDeadline * 4);
        CHECK_FALSE(s.degraded());
        CHECK(s.debt() < 0);

        // kMinDeadline を割る期限は縮退する。
        s.armDeadline(x68k::Scheduler::kMinDeadline - 1);
        CHECK(s.degraded());
        CHECK(s.debt() == 0);
        // 縮退中も未実体化ぶんは 0 のまま。ここが 0 でないと、次の settle が
        // 存在しない時間をデバイスへ渡す。
        CHECK(s.pending() == 0);
    }

    // 命令のサイクル数が常に偶数であること。
    //
    // MFP は CPU サイクルを 1 ビット右シフトして自分のサイクルにする
    // (mfp.h の kCpuToMfpShift)。奇数が混ざると、その半サイクルが切り捨て
    // られる。命令ごとに渡すか、まとめて渡すかで **切り捨ての回数が変わる**
    // ので、イベント駆動と毎命令 tick が一致しなくなる。
    //
    // つまりこの不変条件は、イベント駆動の正しさが乗っている土台そのもの。
    // 将来 3 サイクルの命令が 1 つ足されただけで静かに壊れる。
    //
    // 検出できる変異: 任意の命令の戻り値を奇数にする。
    TEST_CASE("命令が消費するサイクル数は常に偶数")
    {
        Fixture fx;
        // NOP / MOVE / 分岐 / TRAP など、経路の違う命令を混ぜて回す。
        // $4E71 NOP, $7001 MOVEQ #1,D0, $D041 ADD.W D1,D0,
        // $1039... MOVE.B (xxx).L,D0 (バスアクセスを含む)
        fx.build({0x4E71, 0x7001, 0xD041, kMoveTbdrToD0[0], kMoveTbdrToD0[1], kMoveTbdrToD0[2],
                  0x4E71, 0x5340});

        x68k::Machine m;
        fx.attach(m, false);

        for (int i = 0; i < 200000; ++i)
        {
            const x68k::u32 used = m.step();
            if (used == 0)
            {
                break;
            }
            REQUIRE((used % 2) == 0);
        }
    }
    // run() の外から立った割り込みが、次の命令境界で配送される。
    //
    // pressKey / moveMouse は run() の外から呼ばれるので、IPR が時間と
    // 無関係に立つ。期限を張ったまま飛ばし続けると、次のデバイスイベント
    // (最大 80,000 サイクル先) までキー入力が届かない。
    //
    // pressKey 側の sched_.wake() がこれを防いでいる。ここが抜けると
    // 配送が遅れ、ゲストからはポーリングループの反復回数として見える。
    TEST_CASE("run() の外から立った割り込みが期限を待たずに配送される")
    {
        Fixture fx;
        fx.build({
            0x5482,  // ADDQ.L #2,D2
            0x60FC,  // BRA.S -4
        });
        fx.poke16(0x0100, 0x0000);
        fx.poke16(0x0102, 0x0900);
        fx.poke16(0x0900, 0xD682);  // ADD.L D2,D3  配送時の D2 を記録
        fx.poke16(0x0902, 0x4E73);  // RTE

        const auto deliveryDelay = [&](bool eventDriven, x68k::u32 preRun)
        {
            x68k::Machine m;
            fx.attach(m, eventDriven);
            m.mfp().write(x68k::Mfp::kVr, 0x40);
            m.mfp().write(x68k::Mfp::kIera, x68k::Mfp::kIntRecvFull);
            m.mfp().write(x68k::Mfp::kImra, x68k::Mfp::kIntRecvFull);
            m.cpu().setSr(0x2000);
            m.run(preRun);
            const x68k::u32 before = m.cpu().state().d[2];
            m.pressKey(0x1E);
            m.run(20000);
            const x68k::u32 atDelivery = m.cpu().state().d[3];
            return atDelivery >= before ? atDelivery - before : 0u;
        };

        // 位相を変えても毎命令 tick 版と一致すること。
        for (const x68k::u32 preRun : {1000u, 1001u, 1234u, 40000u})
        {
            CAPTURE(preRun);
            CHECK(deliveryDelay(true, preRun) == deliveryDelay(false, preRun));
        }
    }

    // 遅い側へ入っていない状態で wake() を呼んでも、溜まった時間が消えない。
    //
    // unsettled_ を更新するのは syncUnsettled() だけで、それを呼ぶのは
    // settle() と materialize() に限られる (毎命令の advance() は debt_ しか
    // 触らない。ホットパスを 4 命令に保つため)。wake() が unsettled_ を
    // 引き直さずに deadlineAt_ へ代入すると、前回の遅い側からの経過が消える。
    //
    // 現在の呼び出し元は materialize() と組で使われるので偶然助かるが、
    // それは C++ の full-expression 内のデストラクタ順序に依存している。
    // Settled / Rearm で型に固定した規律の外側の規則なので、ここで直接守る。
    TEST_CASE("遅い側を通らずに wake しても溜まった時間が消えない")
    {
        x68k::Scheduler s;
        s.reset();
        s.beginSlice(100000);
        s.armDeadline(50000);

        // 毎命令ぶん進める。いずれも期限に届かないので遅い側へは入らない。
        for (int i = 0; i < 100; ++i)
        {
            CHECK_FALSE(s.advance(4));
        }

        // wake() が先頭で syncUnsettled() を呼ぶので、ここで pending() が
        // 積んだぶんと一致する。呼ばないと 0 になって 400 サイクルが消える。
        s.wake();
        CHECK(s.pending() == 400);
    }
    // fetch の窓判定が境界で誤らない。
    //
    // 命令フェッチは「RAM の窓に収まるか」「ROM の窓に収まるか」を見てから
    // 直接読む。窓の境界をまたぐワードは配列の外を触るので、必ず遅い経路
    // (バス) へ落とさなければならない。
    //
    // Why これを先に書くか: fetch をポインタキャッシュへ置き換える予定で、
    // その実装は `ptr < end` の 1 比較に縮める。**u32 の加算がラップすると
    // 判定が偽陽性になる**境界があり、そこを踏むと窓の外を読む。
    // 実装より先に、守るべき性質をテストで固定しておく。
    TEST_CASE("命令フェッチは窓の境界をまたぐと遅い経路へ落ちる")
    {
        // メイン RAM の末尾ちょうどに PC を置き、そこから 1 ワード読む。
        // 末尾の 1 バイトだけが窓に入る位置なので、直接経路は使えない。
        static std::vector<x68k::u8> ram(x68k::kMainRamSize, 0);
        std::fill(ram.begin(), ram.end(), 0);

        // リセットベクタ。SSP と PC を置く。
        const auto poke16 = [](x68k::u32 a, x68k::u16 v)
        {
            ram[a] = static_cast<x68k::u8>(v >> 8);
            ram[a + 1] = static_cast<x68k::u8>(v & 0xFF);
        };
        poke16(0, 0x0000);
        poke16(2, 0x8000);
        poke16(4, 0x0000);
        poke16(6, 0x0400);
        for (x68k::u32 a = 0x400; a < 0x8000; a += 2)
        {
            poke16(a, 0x4E71);  // NOP
        }

        x68k::Machine m;
        x68k::MemoryMap map{};
        map.mainRam = ram.data();
        m.setMemory(map);
        m.reset();

        // RAM の最終ワードに NOP を置き、そこから実行させる。
        // ここを直接経路が読めること自体は正しい (窓に収まる)。
        const x68k::u32 lastWord = x68k::kMainRamSize - 2;
        poke16(lastWord, 0x4E71);
        x68k::M68kState st = m.cpu().state();
        st.pc = lastWord;
        m.cpu().loadStateForTest(st);
        const x68k::u32 spent = m.run(8);

        // 窓の内側なので実行できる。落ちたり halt したりしないこと。
        CHECK(spent > 0);
        CHECK_FALSE(m.isHalted());
    }
    // ゲスト RAM の書き換えが世代に記録される。
    //
    // デコード済みブロックは「この番地の命令列はこう」という前提を持つ。
    // ゲストが書き換えたら前提が崩れるので、世代でそれを知る
    // (src/x68k/core/cpu/code_gen_map.h)。
    //
    // **書き込みの経路は 3 つある。** どれか 1 つでも世代を上げ損なうと、
    // 古い前提のまま走り続ける。3 つとも個別に確かめる。
    //
    // Why これを先に書くか: 命令フェッチの窓をポインタでキャッシュしたとき、
    // 無効化の経路を 1 つ漏らす変異を既存テストが検出できなかった。
    // 同じ失敗を繰り返さないため、機構より先に検出できることを固定する。
    TEST_CASE("ゲスト RAM の書き換えが 3 つの経路すべてで世代に載る")
    {
        static std::vector<x68k::u8> ram(x68k::kMainRamSize, 0);
        static std::vector<std::uint16_t> gens(x68k::kMainRamSize / x68k::CodeGenMap::kPageSize, 0);
        std::fill(ram.begin(), ram.end(), 0);
        std::fill(gens.begin(), gens.end(), 0);

        x68k::Machine m;
        x68k::MemoryMap map{};
        map.mainRam = ram.data();
        m.setMemory(map);
        m.reset();
        m.cpu().codeGenMap().setStorage(gens.data(), static_cast<x68k::u32>(gens.size()));

        const x68k::u32 target = 0x1000;

        SUBCASE("CPU の直行路 (fastRam)")
        {
            const std::uint16_t before = m.cpu().codeGenMap().generation(target);
            m.cpu().writeForTest(target, 0x1234);
            CHECK(m.cpu().codeGenMap().generation(target) != before);
        }

        SUBCASE("バス経由 (CPU の遅い経路と DMA が通る)")
        {
            const std::uint16_t before = m.cpu().codeGenMap().generation(target);
            m.bus().write16(target, 0x5678);
            CHECK(m.cpu().codeGenMap().generation(target) != before);
        }

        SUBCASE("バス経由のバイト書き込み")
        {
            const std::uint16_t before = m.cpu().codeGenMap().generation(target);
            m.bus().write8(target, 0x9A);
            CHECK(m.cpu().codeGenMap().generation(target) != before);
        }

        SUBCASE("実体の差し替えは全ページを無効にする")
        {
            const std::uint16_t before = m.cpu().codeGenMap().generation(target);
            const std::uint16_t beforeFar = m.cpu().codeGenMap().generation(x68k::kMainRamSize - 1);
            m.cpu().codeGenMap().touchAll();
            CHECK(m.cpu().codeGenMap().generation(target) != before);
            CHECK(m.cpu().codeGenMap().generation(x68k::kMainRamSize - 1) != beforeFar);
        }
    }
}
