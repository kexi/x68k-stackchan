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

// IPL-ROM のチャネル A 初期化表 ($FF0E24 の 20 組)。
//
// $FF0DBC が LEA $FF0E24,A0 / LEA $E98004,A1 / MOVE.W #$13,D1 のループで
// 「レジスタ番号, 値」の組を 20 回そのまま流し込む。ここは ROM の実バイト列。
constexpr x68k::u8 kInitTableA[][2] = {
    {0x09, 0xC0}, {0x09, 0x80}, {0x04, 0x45}, {0x01, 0x00}, {0x02, 0x50},
    {0x03, 0xC0}, {0x05, 0xE2}, {0x09, 0x01}, {0x0B, 0x56}, {0x0C, 0x0E},
    {0x0D, 0x00}, {0x0E, 0x02}, {0x03, 0xC1}, {0x05, 0xEA}, {0x00, 0x80},
    {0x0E, 0x03}, {0x0F, 0x00}, {0x00, 0x10}, {0x00, 0x10}, {0x01, 0x10},
};

// IPL-ROM のチャネル B 初期化表 ($FF0E4C の 18 組)。
//
// $FF0DDC が LEA $FF0E4C,A0 / LEA $E98000,A1 / MOVE.W #$11,D1 で流し込む。
// 末尾の WR1=$10 / WR9=$09 ($FF0E6C-$FF0E6F) が割り込みを開くところ。
constexpr x68k::u8 kInitTableB[][2] = {
    {0x09, 0x40}, {0x04, 0x4C}, {0x01, 0x00}, {0x03, 0xC0}, {0x05, 0x60}, {0x0B, 0x56},
    {0x0C, 0x1F}, {0x0D, 0x00}, {0x0E, 0x02}, {0x03, 0xC1}, {0x05, 0xE8}, {0x00, 0x80},
    {0x0E, 0x03}, {0x0F, 0x00}, {0x00, 0x10}, {0x00, 0x10}, {0x01, 0x10}, {0x09, 0x09},
};

// IPL-ROM が実際に行う初期化を、表の順序どおり再生する。
//
// Why not MIE を直接書いて済ませないか: WR9 がチャネル別の実体だと、
// 「A へ MIE を書く」人工的な手順では動いてしまい、実 ROM が
// B の表の末尾でしか MIE を立てない事実を取りこぼす。表をそのまま
// 流すことでしか、チップ共通レジスタの取り違えは検出できない。
void replayRomInit(x68k::Scc& scc)
{
    for (const auto& entry : kInitTableA)
    {
        scc.writeControl(x68k::Scc::kChannelA, entry[0]);
        scc.writeControl(x68k::Scc::kChannelA, entry[1]);
    }
    for (const auto& entry : kInitTableB)
    {
        scc.writeControl(x68k::Scc::kChannelB, entry[0]);
        scc.writeControl(x68k::Scc::kChannelB, entry[1]);
    }
}

// マウスを使える状態にした SCC を作る。
//
// IPL-ROM の初期化表をそのまま流したうえで、マウス有効化 ($FF147E の
// WR5 へ $62 = RTS on) を行う。ROM の初期化は WR5 に $E8 を書いて
// 終わる ($FF0E60) ので、RTS は別途立てないと立たない。
x68k::Scc makeEnabledScc()
{
    x68k::Scc scc;
    scc.reset();
    replayRomInit(scc);

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

    // WR9 はチップ共通なので、どちらのチャネルから禁止しても効く。
    scc.writeControl(x68k::Scc::kChannelA, 0x09);
    scc.writeControl(x68k::Scc::kChannelA, 0x00);

    scc.moveMouse(1, 1, false, false);

    CHECK(!scc.hasPendingInterrupt());
}

TEST_CASE("WR9 はチップ共通で、A から書いても B から書いても同じ実体に入る")
{
    // 保証すること: WR9 が A/B 別々のコピーになっていないこと。
    //
    // 根拠: Z8530 の WR9 はチップに 1 つしかない。IPL-ROM の初期化表は
    // チャネル A 側が WR9=$01 で終わり ($FF0E32)、MIE ($08) を立てるのは
    // チャネル B の表の末尾 WR9=$09 ($FF0E6E) だけ。
    //
    // 壊れると: A 側の実体を見る実装では MIE が 0 のままになり、
    // 実 ROM の初期化を終えた後にマウス割り込みが一度も上がらない。
    x68k::Scc scc;
    scc.reset();

    // B 側から $09 (MIE + VIS) を書く。
    scc.writeControl(x68k::Scc::kChannelB, 0x09);
    scc.writeControl(x68k::Scc::kChannelB, 0x09);

    // A 側から覗いても同じ値が見える。
    CHECK(scc.peek(x68k::Scc::kChannelA, 9) == 0x09);
    CHECK(scc.peek(x68k::Scc::kChannelB, 9) == 0x09);
}

TEST_CASE("IPL-ROM の初期化表を流しただけでマウス割り込みが上がる")
{
    // 保証すること: 人工的な手順を一切挟まず、実 ROM が書く順序と値
    // ($FF0E24 の 20 組 → $FF0E4C の 18 組) をそのまま再生した後に、
    // マウスの動きが割り込みとして観測できること。
    //
    // これがこのファイルで最も重要な回帰テスト。以前は「A へ MIE を
    // 直接書く」という ROM が決して行わない手順で有効化していたため、
    // WR9 をチャネル別に持つ実装の誤りが表に出なかった。
    //
    // 壊れると: 実機で Human68k が起動してもマウスが完全に無反応になる。
    x68k::Scc scc;
    scc.reset();
    replayRomInit(scc);

    // ROM の初期化直後は WR5=$E8 で RTS が落ちている ($FF0E60)。
    CHECK(!scc.isMouseEnabled());

    // IOCS のマウス有効化 ($FF147E: WR5 へ $62)。
    scc.writeControl(x68k::Scc::kChannelB, 5);
    scc.writeControl(x68k::Scc::kChannelB, 0x62);
    CHECK(scc.isMouseEnabled());

    scc.moveMouse(3, 4, false, false);
    CHECK(scc.hasPendingInterrupt());
    CHECK(scc.acknowledgeInterrupt() == 0x54u);
}

TEST_CASE("3 バイトのレポートは 1 バイトごとに 3 回割り込む")
{
    // 保証すること: 1 レポートが 3 回の受信割り込みとして届くこと。
    //
    // 根拠: IPL-ROM $FF150A のハンドラは 1 回の呼び出しで
    // $FF1512 の MOVE.W $E98002,D0 により 1 バイトだけ読み、
    // $FF1526 の SUBQ.W #1,$092A でカウンタを減らして抜ける。
    // 3 バイト揃うのは 3 回目で、そこで $FF1554 の MOVE.B ×3 が
    // $0CB1 へ写す。つまり ROM は 1 バイト = 1 割り込みを前提にしている。
    //
    // 壊れると: 1 回しか割り込まず、2・3 バイト目が FIFO に残ったまま
    // 二度と読まれない。ROM のカウンタが同期を失い、マウスは
    // ボタンの 1 バイトしか届かない。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(0x11, 0x22, false, false);

    // ROM のハンドラ 1 回分を 3 回繰り返す。
    x68k::u8 received[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        CHECK(scc.hasPendingInterrupt());
        CHECK(scc.acknowledgeInterrupt() == 0x54u);

        received[i] = scc.readData(x68k::Scc::kChannelB);

        // ハンドラ末尾の Reset Highest IUS ($FF1564)。
        scc.writeControl(x68k::Scc::kChannelB, x68k::Scc::kWr0CmdResetHighestIus);
    }

    CHECK(received[0] == 0x00);
    CHECK(received[1] == 0x11);
    CHECK(received[2] == 0x22);

    // 3 バイト読み切ったら要因が消える。
    CHECK(!scc.hasPendingInterrupt());
}

TEST_CASE("FIFO に 3 バイト分の空きが無ければレポートを 1 バイトも積まない")
{
    // 保証すること: レポートの enqueue が all-or-nothing であること。
    //
    // FIFO は 8 段なので、2 レポート (6 バイト) 積むと残りは 2 バイト。
    // ここで 3 バイトのレポートを積もうとすると、バイト単位で判定する
    // 実装では「ボタンと X だけ入って Y が落ちる」形になる。
    //
    // 壊れると: 以降のバイト境界が恒久的にずれる。ROM の $092A は
    // 3 バイトを数えるだけで境界を知る手段が無いので、次のレポートの
    // ボタン値が X 移動量として読まれ、カーソルが暴れ続ける。
    x68k::Scc scc = makeEnabledScc();

    scc.moveMouse(1, 1, false, false);
    scc.moveMouse(2, 2, false, false);
    CHECK(scc.pendingBytes(x68k::Scc::kChannelB) == 6);

    // 残り 2 バイトしか無いので、この 3 バイトは丸ごと捨てられる。
    scc.moveMouse(3, 3, false, false);
    CHECK(scc.pendingBytes(x68k::Scc::kChannelB) == 6);

    // 残っているのは最初の 2 レポートで、境界は保たれている。
    const MouseReport first = readReport(scc);
    CHECK(first.dx == 1);
    CHECK(first.dy == 1);
    const MouseReport second = readReport(scc);
    CHECK(second.dx == 2);
    CHECK(second.dy == 2);
}

TEST_CASE("チャネル B 受信のベクタは $54 になる")
{
    // 保証すること: 受信割り込みのベクタが、IPL-ROM がマウスハンドラを
    // 張ったベクタ番号と一致すること。
    //
    // 根拠は ROM から二重に取れる。
    //   1. $FF0E2C: チャネル A の初期化表が WR2 = $50 を書く
    //   2. $FF0DA2: LEA $00000140,A1 / LEA $FF0E04,A0 / MOVE.W #7,D1 の
    //      ループが $FF0E04 の 8 エントリを 1 つずつ 2 回書き、
    //      $140-$17F (ベクタ $50-$5F) を埋める。その並びで
    //      マウス受信ハンドラ $FF150A が入るのはベクタ $54/$55。
    // Z8530 の status 符号化 (ch B 受信 = 010 を bit3-1) でも $50+4 = $54。
    //
    // 壊れると: $51 などへ飛び、$FF0E04 の表では未使用要因の共通ハンドラ
    // $FF15C0 が呼ばれる。マウスのハンドラが一度も動かずカーソルが止まる。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(1, 1, false, false);

    const x68k::u32 vector = scc.acknowledgeInterrupt();
    CHECK(vector == 0x54u);
}

TEST_CASE("WR2 はチップ共通で、チャネル A から書いた値が B の受信ベクタに効く")
{
    // 保証すること: WR2 を A/B 別々の実体にしないこと。
    //
    // 根拠: IPL-ROM は WR2 をチャネル A の表からしか書かない
    // ($FF0E2C の $50)。チャネル B の表 ($FF0E4C) に WR2 の組は無い。
    // 別実体にすると B 側のベースが 0 のままになる。
    //
    // 壊れると: マウスのベクタが $04 (不正命令ベクタ) 付近になり、
    // 割り込みのたびに異常終了する。
    x68k::Scc scc = makeEnabledScc();

    // ROM は A の表でしか WR2 を書いていない。それが B の受信に効く。
    CHECK(scc.peek(x68k::Scc::kChannelB, 2) == 0x50);

    scc.moveMouse(1, 1, false, false);
    CHECK(scc.acknowledgeInterrupt() == 0x54u);
}

TEST_CASE("保留が無ければ受理は 0 を返す")
{
    // 保証すること: 割り込みが無いときに偽のベクタを返さないこと。
    //
    // 壊れると: Machine が毎命令 0 番地のベクタへ飛ばそうとする。
    x68k::Scc scc = makeEnabledScc();

    CHECK(scc.acknowledgeInterrupt() == 0u);
}

TEST_CASE("Reset Highest IUS は FIFO を空にした後だけ保留を落とす")
{
    // 保証すること: WR0 へ $38 を書いても、まだ読まれていないバイトが
    // 残っている限り受信割り込みは下がらないこと。
    //
    // 根拠: IPL-ROM $FF1564 が受信ハンドラの最後に
    // MOVE.W #$0038,$E98000 を実行するが、そのハンドラは $FF1512 で
    // 1 バイトしか読んでいない。Reset Highest IUS は「サービス中」の印を
    // 落とすコマンドで、受信データという要因を消すものではない。
    //
    // 壊れると: 1 バイト目の処理で保留が落ち、2・3 バイト目の割り込みが
    // 上がらない。ROM の $092A カウンタが 3 のまま止まり、マウスは
    // ボタンの 1 バイトしか受け取れずカーソルが動かない。
    x68k::Scc scc = makeEnabledScc();
    scc.moveMouse(1, 1, false, false);
    CHECK(scc.hasPendingInterrupt());

    // 1 バイト読んでから EOI を書く (ROM のハンドラ 1 回分)。
    (void)scc.readData(x68k::Scc::kChannelB);
    scc.writeControl(x68k::Scc::kChannelB, x68k::Scc::kWr0CmdResetHighestIus);
    CHECK(scc.hasPendingInterrupt());

    // 残り 2 バイトを読み切れば要因が消える。
    (void)scc.readData(x68k::Scc::kChannelB);
    (void)scc.readData(x68k::Scc::kChannelB);
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
