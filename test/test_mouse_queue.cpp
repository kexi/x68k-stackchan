// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: MouseQueue が SCC に断られたレポートを取りこぼさないこと。
//
// SCC の受信 FIFO は 8 段で、3 バイトのレポートを 2 つしか保持できない
// (src/x68k/core/dev/scc.h の kRxFifoSize)。CPU がレベル 5 を止めている間に
// 入力が続くと FIFO が埋まり、SCC はレポートを丸ごと捨てる。捨てるのは
// バイト境界を守るための正しい判断だが、送り主が「送った」ことにして
// 自分の状態を進めると、その 1 回ぶんの入力が永久に消える。
//
// ボタンと移動量では消え方が違う。
//   ボタン: 状態 (レベル)。押下と解放は 1 度ずつしか流れないので、解放が
//           消えるとゲストはボタンを押しっぱなしと見なし続ける。
//           SX-Window ではドラッグが終わらず、ウィンドウが指に貼り付く。
//   移動量: 増分。相対値なので、捨てたぶんの距離は後から取り返せない。
//           指を滑らせた距離とカーソルの移動量がずれ続ける。
// どちらも「差し戻して次の drain() で送り直す」で直る。ここではその両方を
// FIFO を実際に埋めて確かめる。
//
// FIFO の状態は core/ のテスト (test_scc.cpp) でも見ているが、あちらは
// SCC 単体の all-or-nothing しか検査しない。捨てられた後に送り直すのは
// 呼び出し側 (MouseQueue) の責任なので、platform 層まで含めて回す必要がある。

#include <cstdint>

#include "dev/scc.h"
#include "doctest.h"
#include "key_queue.h"
#include "machine.h"

namespace
{

// IOCS がマウスを有効化した状態の Machine を作る。
//
// WR3 bit0 (受信有効) と WR5 bit1 (RTS) の両方が要る。IPL-ROM は
// $FF147E で WR5 を選び $FF1486 で $62 (RTS on) を書く。
void enableMouse(x68k::Machine& machine)
{
    machine.bus().write16(x68k::kSccBase, 0x0003);
    machine.bus().write16(x68k::kSccBase, 0x00C1);
    machine.bus().write16(x68k::kSccBase, 0x0005);
    machine.bus().write16(x68k::kSccBase, 0x0062);
}

// 受信 FIFO を 6/8 バイトまで埋める。残り 2 バイトなので、次のレポートは
// 3 バイト入らず丸ごと捨てられる。
//
// Why not FIFO を直接触るヘルパを足さないか: 実機で埋まるのは
// 「CPU が引き取る前に次のレポートが来た」ときで、経路は moveMouse しかない。
// 同じ経路で埋めておかないと、テストだけが通る状態を作れてしまう。
void fillFifoToTwoReports(x68k::Machine& machine)
{
    CHECK(machine.moveMouse(1, 1, false, false));
    CHECK(machine.moveMouse(2, 2, false, false));
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 6);
}

struct MouseReport
{
    x68k::u8 buttons;
    x68k::u8 dx;
    x68k::u8 dy;
};

MouseReport readReport(x68k::Machine& machine)
{
    MouseReport report{};
    report.buttons = machine.scc().readData(x68k::Scc::kChannelB);
    report.dx = machine.scc().readData(x68k::Scc::kChannelB);
    report.dy = machine.scc().readData(x68k::Scc::kChannelB);
    return report;
}

// レポートの X/Y は 1 バイト符号付き。比較しやすいよう符号付きへ戻す。
int toSigned(x68k::u8 value)
{
    return static_cast<int>(static_cast<std::int8_t>(value));
}

}  // namespace

// --- 受理の可否を返すこと ----------------------------------------------------

TEST_CASE("Scc::moveMouse は積めたかどうかを返す")
{
    // 保証すること: レポートを捨てた事実が呼び出し側へ伝わること。
    //
    // 壊れると: 呼び出し側は捨てられたことを知る手段を失い、
    // 「送った」前提で自分の状態を進める。以下のテストが検査している
    // 送り直しは、どれもこの戻り値の上に成り立っている。
    x68k::Machine machine;
    machine.reset();

    // 有効化前は積めない。
    CHECK(!machine.moveMouse(1, 1, false, false));

    enableMouse(machine);
    CHECK(machine.moveMouse(1, 1, false, false));
    CHECK(machine.moveMouse(2, 2, false, false));

    // 残り 2 バイトでは 3 バイトのレポートが入らない。
    CHECK(!machine.moveMouse(3, 3, false, false));

    // 1 レポートぶん読めばまた入る。
    (void)readReport(machine);
    CHECK(machine.moveMouse(3, 3, false, false));
}

// --- ボタン (状態) の取りこぼし ----------------------------------------------

TEST_CASE("FIFO が埋まっている間のボタン解放は捨てられず、空いてから届く")
{
    // 保証すること: これが本丸。捨てられたボタンの変化が送り直されること。
    //
    // 再現する状況: FIFO に 2 レポート (6/8 バイト) 溜まっている状態で
    // 左ボタンの解放が来る。空きは 2 バイトしか無いので SCC は 3 バイトの
    // レポートを丸ごと捨てる。
    //
    // 壊れると: MouseQueue が送る前に sentLeftButton_ を false にしてしまい、
    // 次の drain() では「変化なし」と判断されて解放が二度と送られない。
    // ゲストは左ボタンが押されたままと見なし続け、SX-Window の
    // ドラッグが終わらない (ウィンドウが指に貼り付いたままになる)。
    x68k::Machine machine;
    machine.reset();
    enableMouse(machine);

    x68k_platform::MouseQueue mouse;
    REQUIRE(mouse.begin());

    // まず左ボタンの押下を通す (FIFO は空なので必ず入る)。
    mouse.push(0, 0, true, false);
    mouse.drain(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);
    CHECK(readReport(machine).buttons == 0x02);

    // FIFO を埋める。CPU が割り込みを止めている間に相当する。
    fillFifoToTwoReports(machine);

    // ここで指を離す。SCC は空きが足りず、このレポートを捨てる。
    mouse.push(0, 0, false, false);
    mouse.drain(machine);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 6);

    // ゲストが溜まっていたぶんを引き取る。
    (void)readReport(machine);
    (void)readReport(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);

    // 空いたので、解放が送り直される。
    mouse.drain(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);
    CHECK(readReport(machine).buttons == 0x00);
}

TEST_CASE("断られている間に何度 drain しても、空けば解放が届く")
{
    // 保証すること: 送り直しが 1 回きりの仕掛けではないこと。
    //
    // drain() はエミュレーションのスライスごとに呼ばれるので、FIFO が
    // 埋まっている間は何度も断られる。
    //
    // 壊れると: 2 回目以降の drain() で状態が壊れ、FIFO が空いても
    // 解放が届かない。上のテストは drain() が 1 回だけの経路しか通らない。
    x68k::Machine machine;
    machine.reset();
    enableMouse(machine);

    x68k_platform::MouseQueue mouse;
    REQUIRE(mouse.begin());

    mouse.push(0, 0, true, false);
    mouse.drain(machine);
    (void)readReport(machine);

    fillFifoToTwoReports(machine);

    mouse.push(0, 0, false, false);
    for (int i = 0; i < 5; ++i)
    {
        mouse.drain(machine);
        CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 6);
    }

    (void)readReport(machine);
    (void)readReport(machine);

    mouse.drain(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);
    CHECK(readReport(machine).buttons == 0x00);
}

TEST_CASE("マウスが無効な間のボタン解放も、有効化されてから届く")
{
    // 保証すること: 断られる理由が FIFO 満杯でなくても送り直されること。
    //
    // Scc::moveMouse は WR3/WR5 が落ちている間もレポートを捨てる。IOCS が
    // マウスを一時的に無効化する ($FF144A の WR5 = $60) 場面がこれにあたる。
    //
    // 壊れると: 無効化を挟むたびにボタンの状態がずれ、再有効化した後の
    // 最初のクリックが効かない、あるいは押しっぱなしになる。
    x68k::Machine machine;
    machine.reset();
    enableMouse(machine);

    x68k_platform::MouseQueue mouse;
    REQUIRE(mouse.begin());

    mouse.push(0, 0, true, false);
    mouse.drain(machine);
    (void)readReport(machine);

    // WR5 へ $60 (RTS off)。IPL-ROM $FF1452 と同じ値。
    machine.bus().write16(x68k::kSccBase, 0x0005);
    machine.bus().write16(x68k::kSccBase, 0x0060);
    REQUIRE(!machine.scc().isMouseEnabled());

    mouse.push(0, 0, false, false);
    mouse.drain(machine);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);

    enableMouse(machine);
    mouse.drain(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);
    CHECK(readReport(machine).buttons == 0x00);
}

// --- 移動量 (増分) の取りこぼし ----------------------------------------------

TEST_CASE("断られた移動量は次のレポートへ足し込まれる")
{
    // 保証すること: 捨てられたレポートの dx/dy が失われないこと。
    //
    // dx/dy は相対量なので、1 レポート捨てるとその距離は永久に消える。
    // ボタンと違って後から辻褄の合う値が来ることは無い。差し戻せば
    // 「1 スライスぶん遅れる」だけで距離は保たれる。
    //
    // 壊れると: 指を速く滑らせるたびにカーソルの移動距離が目減りする。
    // FIFO が埋まるのは指を速く動かしたときなので、いちばん効いてほしい
    // 場面でいちばん狂う。
    x68k::Machine machine;
    machine.reset();
    enableMouse(machine);

    x68k_platform::MouseQueue mouse;
    REQUIRE(mouse.begin());

    fillFifoToTwoReports(machine);

    // 断られる。10 と 20 は差し戻される。
    mouse.push(10, 20, false, false);
    mouse.drain(machine);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 6);

    // 断られている間にも指は動く。差し戻したぶんへ合流する。
    mouse.push(5, 7, false, false);

    (void)readReport(machine);
    (void)readReport(machine);

    mouse.drain(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);

    const MouseReport report = readReport(machine);
    CHECK(toSigned(report.dx) == 15);
    CHECK(toSigned(report.dy) == 27);
}

TEST_CASE("受理されたら移動量は差し戻されない")
{
    // 保証すること: 送れたぶんを二重に送らないこと。
    //
    // 壊れると: 同じ移動量が毎スライス送られ続け、カーソルが指を離しても
    // 一方向へ走り続ける。差し戻しの条件を取り違えると必ずこうなる。
    x68k::Machine machine;
    machine.reset();
    enableMouse(machine);

    x68k_platform::MouseQueue mouse;
    REQUIRE(mouse.begin());

    mouse.push(3, 4, false, false);
    mouse.drain(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);

    const MouseReport first = readReport(machine);
    CHECK(toSigned(first.dx) == 3);
    CHECK(toSigned(first.dy) == 4);

    // 積むものが無いので、次の drain() は何も送らない。
    mouse.drain(machine);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);
}

TEST_CASE("動きもボタンの変化も無ければレポートを積まない")
{
    // 保証すること: 中身の無いレポートで割り込みを起こさないこと。
    //
    // 1 レポートごとに SCC が受信割り込みを上げ、IOCS のハンドラが走る。
    // 実効 3MHz では触っていない間の無駄な割り込みが体感に効く。
    //
    // 壊れると: 差し戻しを入れた副作用で「毎スライス送る」に退行しても
    // 上のテストは通ってしまう。ここで止める。
    x68k::Machine machine;
    machine.reset();
    enableMouse(machine);

    x68k_platform::MouseQueue mouse;
    REQUIRE(mouse.begin());

    mouse.drain(machine);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);

    // ボタンを押していない状態を積んでも、前回送った状態と同じなら送らない。
    mouse.push(0, 0, false, false);
    mouse.drain(machine);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);
}

TEST_CASE("ボタンと移動量が同時に断られたら両方まとめて送り直す")
{
    // 保証すること: 差し戻しがボタンと移動量のどちらか片方に偏らないこと。
    //
    // 壊れると: 実際の操作 (押したまま指を滑らせる = ドラッグ) で、
    // ボタンだけ届いて距離が消える、あるいはその逆になる。
    x68k::Machine machine;
    machine.reset();
    enableMouse(machine);

    x68k_platform::MouseQueue mouse;
    REQUIRE(mouse.begin());

    fillFifoToTwoReports(machine);

    mouse.push(-6, -9, true, false);
    mouse.drain(machine);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 6);

    (void)readReport(machine);
    (void)readReport(machine);

    mouse.drain(machine);
    REQUIRE(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);

    const MouseReport report = readReport(machine);
    CHECK(report.buttons == 0x02);
    CHECK(toSigned(report.dx) == -6);
    CHECK(toSigned(report.dy) == -9);
}
