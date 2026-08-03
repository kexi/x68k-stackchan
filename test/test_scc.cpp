// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: Z8530 SCC のチャネル B に繋がるマウスが、IOCS から見て
// 期待どおりに振る舞うこと。
//
// SCC は SX-Window (Human68k の純正 GUI) の前提条件で、壊れると
// 「カーソルが出ない」「動かない」「勝手に飛ぶ」という形でしか現れない。
// レジスタポインタ方式・3 バイトのレポート framing・RR0 の状態ビットの
// どれが原因かは症状から切り分けられないので、個別に押さえておく。
//
// プロトコルの根拠はすべて rom/iplrom.dat (EXPERT 用 v1.0) の実コード。
// 出典のアドレスは src/x68k/core/dev/scc.h の冒頭にまとめてある。

#include "dev/scc.h"
#include "doctest.h"
#include "machine.h"

namespace
{

// マウスを使える状態にした SCC を作る。
//
// IOCS が実際に行う手順をなぞる: WR9 でマスタ割り込み許可、WR1 で
// 受信割り込み (全文字)、WR3 で受信有効、WR5 で RTS。
x68k::Scc makeEnabledScc()
{
    x68k::Scc scc;
    scc.reset();

    // WR9 はチップ共通なのでチャネル A 側の実体に入る。
    scc.writeControl(x68k::Scc::kChannelA, 9);
    scc.writeControl(x68k::Scc::kChannelA, x68k::Scc::kWr9Mie);

    // WR2 = 割り込みベクタのベース。IOCS は $70 を書く。
    scc.writeControl(x68k::Scc::kChannelA, 2);
    scc.writeControl(x68k::Scc::kChannelA, 0x70);

    // WR1 = $10 (全文字で受信割り込み)。
    scc.writeControl(x68k::Scc::kChannelB, 1);
    scc.writeControl(x68k::Scc::kChannelB, 0x10);

    // WR3 = $C1 (Rx 8bit + 受信有効)。
    scc.writeControl(x68k::Scc::kChannelB, 3);
    scc.writeControl(x68k::Scc::kChannelB, 0xC1);

    // WR5 = $62 (RTS on)。IPL-ROM $FF1486 と同じ値。
    scc.writeControl(x68k::Scc::kChannelB, 5);
    scc.writeControl(x68k::Scc::kChannelB, 0x62);

    return scc;
}

// レポート 3 バイトを読み出す。
struct MouseReport
{
    x68k::u8 buttons;
    x68k::u8 dx;
    x68k::u8 dy;
};

MouseReport readReport(x68k::Scc& scc)
{
    MouseReport report{};
    report.buttons = scc.readData(x68k::Scc::kChannelB);
    report.dx = scc.readData(x68k::Scc::kChannelB);
    report.dy = scc.readData(x68k::Scc::kChannelB);
    return report;
}

}  // namespace

// --- レジスタポインタ方式 ----------------------------------------------------

TEST_CASE("レジスタ番号を書いてから値を書くと、そのレジスタに入る")
{
    // 保証すること: Z8530 の 2 段階書き込み (WR0 でレジスタ選択 → 値) が
    // 効くこと。IPL-ROM のマウス有効化 ($FF147E) がこの形をとる。
    //
    // 壊れると: WR5 への $62 が WR0 に入り、RTS が立たない。
    // マウスが 1 バイトもレポートを返さなくなる。
    x68k::Scc scc;
    scc.reset();

    scc.writeControl(x68k::Scc::kChannelB, 5);
    scc.writeControl(x68k::Scc::kChannelB, 0x62);

    CHECK(scc.peek(x68k::Scc::kChannelB, 5) == 0x62);
}

TEST_CASE("レジスタポインタは 1 回のアクセスで 0 へ戻る")
{
    // 保証すること: レジスタを選んで値を書いた後、次の書き込みは
    // 再び WR0 (レジスタ選択) として解釈されること。
    //
    // 壊れると: 選択が残り続け、連続した初期化表の書き込みが
    // すべて同じレジスタへ入る。IPL-ROM $FF0DDC の 17 組が
    // 最後の 1 組しか効かなくなる。
    x68k::Scc scc;
    scc.reset();

    scc.writeControl(x68k::Scc::kChannelB, 5);
    scc.writeControl(x68k::Scc::kChannelB, 0x62);

    // ポインタが 0 へ戻っているので、これはレジスタ 3 の選択になる。
    scc.writeControl(x68k::Scc::kChannelB, 3);
    scc.writeControl(x68k::Scc::kChannelB, 0xC1);

    CHECK(scc.peek(x68k::Scc::kChannelB, 5) == 0x62);
    CHECK(scc.peek(x68k::Scc::kChannelB, 3) == 0xC1);
}

TEST_CASE("Point High ($08) でレジスタ 8-15 を選べる")
{
    // 保証すること: WR0 の bit3 を立てた選択が +8 されること。
    //
    // 壊れると: X68000 の初期化表が使う WR9/WR11/WR12/WR13/WR14/WR15 に
    // 一切届かず、代わりに WR1/WR3/WR4/WR5/WR6/WR7 が壊される。
    // 特に WR9 (マスタ割り込み許可) が入らないと割り込みが上がらない。
    x68k::Scc scc;
    scc.reset();

    // $0B = Point High + レジスタ 3 → WR11。
    scc.writeControl(x68k::Scc::kChannelA, 0x0B);
    scc.writeControl(x68k::Scc::kChannelA, 0x56);
    CHECK(scc.peek(x68k::Scc::kChannelA, 11) == 0x56);

    // $09 = Point High + レジスタ 1 → WR9。
    scc.writeControl(x68k::Scc::kChannelA, 0x09);
    scc.writeControl(x68k::Scc::kChannelA, 0x40);
    CHECK(scc.peek(x68k::Scc::kChannelA, 9) == 0x40);
}

TEST_CASE("A と B のレジスタファイルは独立している")
{
    // 保証すること: 片方のチャネルへの書き込みが他方に漏れないこと。
    //
    // 壊れると: RS-232C (A) の初期化がマウス (B) の設定を上書きする。
    // IPL-ROM は A を先に初期化する ($FF0DBC) ので、マウスの設定が
    // 必ず壊れることになる。
    x68k::Scc scc;
    scc.reset();

    scc.writeControl(x68k::Scc::kChannelB, 5);
    scc.writeControl(x68k::Scc::kChannelB, 0x62);
    scc.writeControl(x68k::Scc::kChannelA, 5);
    scc.writeControl(x68k::Scc::kChannelA, 0xE8);

    CHECK(scc.peek(x68k::Scc::kChannelB, 5) == 0x62);
    CHECK(scc.peek(x68k::Scc::kChannelA, 5) == 0xE8);
}

// --- RR0 の状態ビット --------------------------------------------------------

TEST_CASE("RR0 の送信バッファ空きは常に立っている")
{
    // 保証すること: RR0 bit2 (Tx Buffer Empty) が常にアクティブなこと。
    //
    // 壊れると: IPL-ROM $FF8042 の「RR0 を読んで bit2 を待つ」ループから
    // 抜けられず、起動が止まる。本エミュレータは送信先を持たないので
    // 待たせる理由が無い。
    x68k::Scc scc;
    scc.reset();

    // ポインタは既に 0 なので、そのまま読めば RR0。
    const x68k::u8 rr0 = scc.readControl(x68k::Scc::kChannelB);
    CHECK((rr0 & x68k::Scc::kRr0TxEmpty) != 0);
}

TEST_CASE("受信データが無ければ RR0 の受信可能ビットは落ちている")
{
    // 保証すること: データを積んでいない状態で bit0 が 0 であること。
    //
    // 壊れると: IOCS が常に「データがある」と判断してデータポートを
    // 読み続け、0 をマウスレポートとして解釈する。
    x68k::Scc scc = makeEnabledScc();

    const x68k::u8 rr0 = scc.readControl(x68k::Scc::kChannelB);
    CHECK((rr0 & x68k::Scc::kRr0RxAvailable) == 0);
}

TEST_CASE("マウスを動かすと RR0 の受信可能ビットが立つ")
{
    // 保証すること: レポートが積まれたことを RR0 bit0 で知れること。
    //
    // 壊れると: 割り込みを使わずポーリングする経路 (SX-Window が使う
    // ことがある) でマウスの動きが一切拾えない。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(5, -3, false, false);

    const x68k::u8 rr0 = scc.readControl(x68k::Scc::kChannelB);
    CHECK((rr0 & x68k::Scc::kRr0RxAvailable) != 0);
}

TEST_CASE("3 バイトすべて読み切ると受信可能ビットが落ちる")
{
    // 保証すること: FIFO を空にしたら bit0 が下りること。
    //
    // 壊れると: IOCS が読み終えた後も「まだある」と見え、
    // 同じレポートを何度も処理してカーソルが加速する。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(1, 1, false, false);

    (void)readReport(scc);

    const x68k::u8 rr0 = scc.readControl(x68k::Scc::kChannelB);
    CHECK((rr0 & x68k::Scc::kRr0RxAvailable) == 0);
}

// --- マウスレポートの framing ------------------------------------------------

TEST_CASE("マウスのレポートはちょうど 3 バイト")
{
    // 保証すること: 1 回の moveMouse が 3 バイトだけ積むこと。
    //
    // 根拠: IPL-ROM $FF153A が残りバイト数を 3 で初期化し、$FF1554 で
    // MOVE.B を 3 回展開してワークへ写す。
    //
    // 壊れると: バイト数がずれ、IOCS の 3 バイトカウンタと同期が外れる。
    // ボタンと移動量の対応が入れ替わり、動かすたびにクリック扱いになる。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(10, 20, false, false);

    CHECK(scc.pendingBytes(x68k::Scc::kChannelB) == 3);
}

TEST_CASE("レポートの順序はボタン・X・Y")
{
    // 保証すること: 3 バイトが「ボタン状態, X 移動量, Y 移動量」の順で
    // 出てくること。
    //
    // 壊れると: X と Y が入れ替わってカーソルが斜めに動く、あるいは
    // 移動量がボタン状態として読まれて動かすだけでクリックが暴発する。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(0x12, 0x34, false, false);

    const MouseReport report = readReport(scc);
    CHECK(report.buttons == 0x00);
    CHECK(report.dx == 0x12);
    CHECK(report.dy == 0x34);
}

TEST_CASE("左ボタンは bit1、右ボタンは bit0")
{
    // 保証すること: X68000 のマウスのボタン割り当てに従うこと。
    //
    // 壊れると: SX-Window で左クリックとコンテキストメニューが
    // 入れ替わり、操作が成立しない。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(0, 0, true, false);
    CHECK(readReport(scc).buttons == 0x02);

    scc.moveMouse(0, 0, false, true);
    CHECK(readReport(scc).buttons == 0x01);

    scc.moveMouse(0, 0, true, true);
    CHECK(readReport(scc).buttons == 0x03);
}

TEST_CASE("負の移動量は 2 の補数で入る")
{
    // 保証すること: 移動量が 1 バイト符号付きとして表現されること。
    //
    // 壊れると: 左や上へ動かしたときにカーソルが右下へ大きく飛ぶ。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(-1, -2, false, false);

    const MouseReport report = readReport(scc);
    CHECK(report.dx == 0xFF);
    CHECK(report.dy == 0xFE);
}

TEST_CASE("移動量は -128..127 に飽和する")
{
    // 保証すること: 範囲外の移動量が折り返さずに頭打ちになること。
    //
    // 壊れると: 素早く動かしたときに符号が反転し、カーソルが逆方向へ
    // 飛ぶ。ホスト側のタッチ操作では大きな dx が普通に出るので、
    // 切り捨てにすると実用上すぐ踏む。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(1000, -1000, false, false);

    const MouseReport report = readReport(scc);
    CHECK(report.dx == 0x7F);  // +127
    CHECK(report.dy == 0x80);  // -128
}

TEST_CASE("複数のレポートは順番を保って積まれる")
{
    // 保証すること: 連続した動きが混ざらないこと。
    //
    // 壊れると: FIFO の取り出し順が狂い、あるレポートのボタン状態が
    // 別のレポートの移動量と組み合わさる。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(1, 2, false, false);
    scc.moveMouse(3, 4, true, false);

    const MouseReport first = readReport(scc);
    CHECK(first.buttons == 0x00);
    CHECK(first.dx == 1);
    CHECK(first.dy == 2);

    const MouseReport second = readReport(scc);
    CHECK(second.buttons == 0x02);
    CHECK(second.dx == 3);
    CHECK(second.dy == 4);
}

// --- 有効化の条件 ------------------------------------------------------------

TEST_CASE("受信有効と RTS の両方が立って初めてマウスが有効になる")
{
    // 保証すること: WR3 bit0 (受信有効) と WR5 bit1 (RTS) の
    // 両方が必要なこと。
    //
    // 根拠: IPL-ROM $FF147E がマウス有効化で WR5 に $62 (RTS on)、
    // $FF144A が無効化で $60 (RTS off) を書く。RTS だけで切り替えている
    // ということは、RTS がマウスへの「送ってよい」信号になっている。
    x68k::Scc scc;
    scc.reset();

    CHECK(!scc.isMouseEnabled());

    // 受信有効だけでは足りない。
    scc.writeControl(x68k::Scc::kChannelB, 3);
    scc.writeControl(x68k::Scc::kChannelB, 0xC1);
    CHECK(!scc.isMouseEnabled());

    // RTS を立てて初めて有効。
    scc.writeControl(x68k::Scc::kChannelB, 5);
    scc.writeControl(x68k::Scc::kChannelB, 0x62);
    CHECK(scc.isMouseEnabled());
}

TEST_CASE("無効化中の動きは積まれない")
{
    // 保証すること: マウスが無効な間の moveMouse が捨てられること。
    //
    // 壊れると: IOCS がマウスを有効化した瞬間に、それまでに溜まった
    // 古い動きが一気に流れ込む。SX-Window の起動直後にカーソルが
    // 画面外へ飛ぶ形で出る。
    x68k::Scc scc;
    scc.reset();

    scc.moveMouse(50, 50, true, true);

    CHECK(scc.pendingBytes(x68k::Scc::kChannelB) == 0);
}

TEST_CASE("RTS を落とすとマウスが無効になる")
{
    // 保証すること: IPL-ROM $FF144A の無効化 (WR5 へ $60) が効くこと。
    x68k::Scc scc = makeEnabledScc();
    CHECK(scc.isMouseEnabled());

    scc.writeControl(x68k::Scc::kChannelB, 5);
    scc.writeControl(x68k::Scc::kChannelB, 0x60);

    CHECK(!scc.isMouseEnabled());
}

TEST_CASE("受信を無効にすると溜まっていたデータが捨てられる")
{
    // 保証すること: WR3 bit0 を落とすと FIFO が空になること。
    //
    // 壊れると: 受信を止めた後も古いレポートが残り、再開時に
    // 触っていないのにカーソルが動く。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(9, 9, false, false);
    CHECK(scc.pendingBytes(x68k::Scc::kChannelB) == 3);

    scc.writeControl(x68k::Scc::kChannelB, 3);
    scc.writeControl(x68k::Scc::kChannelB, 0xC0);  // 受信有効を落とす

    CHECK(scc.pendingBytes(x68k::Scc::kChannelB) == 0);
}

// --- 割り込み ----------------------------------------------------------------

TEST_CASE("マウスが動くと受信割り込みが保留になる")
{
    // 保証すること: WR1 で受信割り込みが許可されているとき、
    // レポートが割り込みを上げること。
    //
    // 壊れると: IOCS のマウス受信ハンドラ ($FF150A) が一度も呼ばれず、
    // カーソルが動かない。
    x68k::Scc scc = makeEnabledScc();
    CHECK(!scc.hasPendingInterrupt());

    scc.moveMouse(1, 1, false, false);

    CHECK(scc.hasPendingInterrupt());
}

TEST_CASE("受信割り込みが禁止されていれば保留にならない")
{
    // 保証すること: WR1 の bit4-3 が 00 (禁止) のとき割り込みを上げないこと。
    //
    // 壊れると: IOCS がハンドラを張る前に割り込みが上がり、
    // 未初期化のベクタへ飛ぶ。
    x68k::Scc scc = makeEnabledScc();

    scc.writeControl(x68k::Scc::kChannelB, 1);
    scc.writeControl(x68k::Scc::kChannelB, 0x00);

    scc.moveMouse(1, 1, false, false);

    CHECK(!scc.hasPendingInterrupt());
}

TEST_CASE("マスタ割り込み許可が無ければ保留にならない")
{
    // 保証すること: WR9 bit3 (MIE) が必要なこと。
    //
    // 壊れると: チップ全体の割り込みを止めているつもりの区間でも
    // 割り込みが飛び、IOCS の排他が崩れる。
    x68k::Scc scc = makeEnabledScc();

    scc.writeControl(x68k::Scc::kChannelA, 9);
    scc.writeControl(x68k::Scc::kChannelA, 0x00);

    scc.moveMouse(1, 1, false, false);

    CHECK(!scc.hasPendingInterrupt());
}

TEST_CASE("割り込みの受理でベクタ番号が返り、保留が落ちる")
{
    // 保証すること: SCC が自分のベクタ番号を返すこと。
    //
    // 根拠: WR2 に書かれた値がベース。IOCS は $70 を書き、
    // チャネル B の受信は +1 ($71)。
    //
    // 壊れると: 自動ベクタになり、IOCS が張ったマウス用ハンドラへ
    // 届かない。MFP を自動ベクタにしたときと同じ壊れ方をする。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(1, 1, false, false);

    const x68k::u32 vector = scc.acknowledgeInterrupt();
    CHECK(vector == 0x71u);
    CHECK(!scc.hasPendingInterrupt());
}

TEST_CASE("保留が無ければ受理は 0 を返す")
{
    // 保証すること: 割り込みが無いときに偽のベクタを返さないこと。
    //
    // 壊れると: Machine が毎命令 0 番地のベクタへ飛ばそうとする。
    x68k::Scc scc = makeEnabledScc();

    CHECK(scc.acknowledgeInterrupt() == 0u);
}

TEST_CASE("Reset Highest IUS で保留が落ちる")
{
    // 保証すること: WR0 へ $38 を書くと割り込みが取り下げられること。
    //
    // 根拠: IPL-ROM $FF1566 が受信ハンドラの最後に
    // MOVE.W #$0038,$E98000 を実行する。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(1, 1, false, false);
    CHECK(scc.hasPendingInterrupt());

    scc.writeControl(x68k::Scc::kChannelB, x68k::Scc::kWr0CmdResetHighestIus);

    CHECK(!scc.hasPendingInterrupt());
}

TEST_CASE("データを全部読み切ると割り込みの保留も落ちる")
{
    // 保証すること: FIFO が空になったら割り込み要因が消えること。
    //
    // 壊れると: IOCS が読み切った後も割り込みが上がり続け、
    // ハンドラを無限に呼んで先へ進まなくなる。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(1, 1, false, false);

    (void)readReport(scc);

    CHECK(!scc.hasPendingInterrupt());
}

// --- Machine 経由のアドレスデコード -----------------------------------------

TEST_CASE("SCC のレジスタは奇数アドレスに現れる")
{
    // 保証すること: MOVE.W で $E98000 へ書いたとき、下位バイト
    // ($E98001) の値がレジスタへ届くこと。
    //
    // 根拠: Z8530 は 8bit デバイスで 16bit バスの下位側に繋がる。
    // IPL-ROM は MOVE.W #$0062,$E98000 と書くので、$62 は $E98001 側。
    //
    // 壊れると: 上位バイトの $00 がレジスタへ入り、すべての設定が 0 になる。
    x68k::Machine machine;
    machine.reset();

    // WR5 を選んで $62 を書く。IPL-ROM $FF147E/$FF1486 と同じ形。
    machine.bus().write16(x68k::kSccBase, 0x0005);
    machine.bus().write16(x68k::kSccBase, 0x0062);

    CHECK(machine.scc().peek(x68k::Scc::kChannelB, 5) == 0x62);
}

TEST_CASE("$E98000 がチャネル B、$E98004 がチャネル A")
{
    // 保証すること: チャネルの割り当てが実機と一致すること。
    //
    // 根拠: IPL-ROM $FF0DDC が $E98000 を基点にチャネル B の初期化表を、
    // $FF0DBC が $E98004 を基点にチャネル A の表を流し込む。
    //
    // 壊れると: マウスの設定が RS-232C 側へ行き、レポートが
    // 一切届かない。資料によって A/B が逆に書かれていることがあるので、
    // ここは実機の ROM を根拠に固定する。
    x68k::Machine machine;
    machine.reset();

    machine.bus().write16(x68k::kSccBase, 0x0005);
    machine.bus().write16(x68k::kSccBase, 0x0062);
    machine.bus().write16(x68k::kSccBase + 4, 0x0005);
    machine.bus().write16(x68k::kSccBase + 4, 0x00E8);

    CHECK(machine.scc().peek(x68k::Scc::kChannelB, 5) == 0x62);
    CHECK(machine.scc().peek(x68k::Scc::kChannelA, 5) == 0xE8);
}

TEST_CASE("データポートは制御ポートの +2 にある")
{
    // 保証すること: $E98002 がチャネル B のデータポートであること。
    //
    // 根拠: IPL-ROM $FF1512 が MOVE.W $E98002,D0 でマウスの受信データを読む。
    //
    // 壊れると: 受信ハンドラが RR0 の状態ビットをマウスの
    // ボタン状態として読み、押していないボタンが押されたことになる。
    x68k::Machine machine;
    machine.reset();

    // マウスを有効化する。
    machine.bus().write16(x68k::kSccBase + 4, 0x0009);
    machine.bus().write16(x68k::kSccBase + 4, x68k::Scc::kWr9Mie);
    machine.bus().write16(x68k::kSccBase, 0x0003);
    machine.bus().write16(x68k::kSccBase, 0x00C1);
    machine.bus().write16(x68k::kSccBase, 0x0005);
    machine.bus().write16(x68k::kSccBase, 0x0062);

    machine.moveMouse(0x11, 0x22, false, false);

    // データポートを 3 回読む。$E98002 の下位バイト = $E98003。
    CHECK(machine.bus().read8(x68k::kSccBase + 3) == 0x00);
    CHECK(machine.bus().read8(x68k::kSccBase + 3) == 0x11);
    CHECK(machine.bus().read8(x68k::kSccBase + 3) == 0x22);
}

TEST_CASE("Machine::moveMouse が SCC までつながっている")
{
    // 保証すること: ホストや platform 層から入力を注入する口が
    // 実際に SCC を動かすこと。pressKey と同じ役割。
    //
    // 壊れると: 実機でタッチしてもカーソルが動かない。core/ の
    // テストで捕まえられないと、実機に焼くまで気付けない。
    x68k::Machine machine;
    machine.reset();

    // 有効化していない状態では何も起きない。
    machine.moveMouse(1, 1, false, false);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);

    machine.bus().write16(x68k::kSccBase, 0x0003);
    machine.bus().write16(x68k::kSccBase, 0x00C1);
    machine.bus().write16(x68k::kSccBase, 0x0005);
    machine.bus().write16(x68k::kSccBase, 0x0062);

    machine.moveMouse(7, 8, true, false);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);
}

TEST_CASE("リセットで SCC の設定と溜まったレポートが消える")
{
    // 保証すること: reset() が SCC を初期状態へ戻すこと。
    //
    // 壊れると: リセット後もマウスが有効なままで、IOCS が初期化する
    // 前のレポートが残る。
    x68k::Machine machine;
    machine.reset();

    machine.bus().write16(x68k::kSccBase, 0x0003);
    machine.bus().write16(x68k::kSccBase, 0x00C1);
    machine.bus().write16(x68k::kSccBase, 0x0005);
    machine.bus().write16(x68k::kSccBase, 0x0062);
    machine.moveMouse(1, 1, false, false);
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);

    machine.reset();

    CHECK(!machine.scc().isMouseEnabled());
    CHECK(machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);
}
