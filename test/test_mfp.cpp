// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: MFP の割り込みと GPIP が Human68k の起動を止めない形で
// 動くこと。
//
// MFP は X68000 の割り込みとタイマの中枢で、システムタイマ・垂直帰線・
// キーボード受信がすべてここを通る。壊れ方が独特で、「起動途中で止まる」
// 「キーが効かない」のような症状になるが、どこが原因か辿りにくい。
//
// 実際に起きた退行: 垂直帰線 (GPIP bit4) が CPU からの書き込みで固定されると
// Human68k が起動途中で止まる。GPIP は入力専用であることを押さえておく。

#include "dev/mfp.h"
#include "doctest.h"
#include "machine.h"

namespace
{

// リセット直後の状態から始める MFP。
x68k::Mfp makeMfp()
{
    x68k::Mfp mfp;
    mfp.reset();
    return mfp;
}

// 割り込みを受け取れる状態にする (許可 + マスク解除)。
void enableGroupA(x68k::Mfp& mfp, x68k::u8 bit)
{
    mfp.write(x68k::Mfp::kIera, bit);
    mfp.write(x68k::Mfp::kImra, bit);
}

void enableGroupB(x68k::Mfp& mfp, x68k::u8 bit)
{
    mfp.write(x68k::Mfp::kIerb, bit);
    mfp.write(x68k::Mfp::kImrb, bit);
}

}  // namespace

// --- GPIP は読み取り専用 -----------------------------------------------------

TEST_CASE("GPIP は CPU からの書き込みで変化しない")
{
    // 保証すること: GPIP は入力専用で、CPU が書いても値が変わらないこと。
    //
    // 壊れると: 垂直帰線 (bit4) が CPU の書き込みで固定され、
    // Human68k が垂直帰線待ちのループから出られず起動途中で止まる。
    // 実際にこの退行が起きた。
    x68k::Mfp mfp = makeMfp();
    const x68k::u8 before = mfp.peek(x68k::Mfp::kGpip);

    mfp.write(x68k::Mfp::kGpip, 0x00);
    CHECK(mfp.peek(x68k::Mfp::kGpip) == before);

    mfp.write(x68k::Mfp::kGpip, 0xFF);
    CHECK(mfp.peek(x68k::Mfp::kGpip) == before);
}

TEST_CASE("GPIP へ書き込んだ後も垂直帰線の状態は変化し続ける")
{
    // 保証すること: CPU の書き込みが GPIP を「固定」しないこと。
    // 値が変わらないだけでなく、その後の垂直帰線通知が効き続ける必要がある。
    //
    // 壊れると: 起動の早い段階で誰かが GPIP を書いた時点から
    // 垂直帰線が止まって見え、そこから先へ進まなくなる。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kGpip, 0x00);

    mfp.setVerticalBlank(true);
    CHECK((mfp.peek(x68k::Mfp::kGpip) & 0x10u) == 0);

    mfp.setVerticalBlank(false);
    CHECK((mfp.peek(x68k::Mfp::kGpip) & 0x10u) != 0);
}

TEST_CASE("垂直帰線中は GPIP4 が L になる")
{
    // 保証すること: 極性が逆でないこと。X68000 では帰線中が L (0)。
    //
    // 壊れると: 表示期間と帰線期間が入れ替わり、画面更新のタイミングを
    // 待つコードが常に空振りする。
    x68k::Mfp mfp = makeMfp();
    // リセット直後は帰線中ではない = H。
    CHECK((mfp.peek(x68k::Mfp::kGpip) & 0x10u) != 0);

    mfp.setVerticalBlank(true);
    CHECK((mfp.peek(x68k::Mfp::kGpip) & 0x10u) == 0);

    mfp.setVerticalBlank(false);
    CHECK((mfp.peek(x68k::Mfp::kGpip) & 0x10u) != 0);
}

TEST_CASE("リセット直後の GPIP は拡張ボードと電源スイッチを L にする")
{
    // 保証すること: IPL-ROM が起動時に bit1/bit2 が 0 になるのを待つループ
    // ($FF103C) を抜けられること。
    //
    // 壊れると: 起動の最初でタイムアウトするまで無駄に回り続ける。
    x68k::Mfp mfp = makeMfp();
    const x68k::u8 gpip = mfp.peek(x68k::Mfp::kGpip);

    CHECK((gpip & 0x02u) == 0);  // EXPON
    CHECK((gpip & 0x04u) == 0);  // POWER
}

TEST_CASE("リセット直後の TSR は送信バッファ空きを立てる")
{
    // 保証すること: IPL-ROM がキーボードへコマンドを送る前の待ちループ
    // ($FF61C8) を抜けられること。
    //
    // 壊れると: そこで永久に待ち続ける。
    x68k::Mfp mfp = makeMfp();
    CHECK((mfp.peek(x68k::Mfp::kTsr) & 0x80u) != 0);
}

TEST_CASE("TSR へ何を書いても送信バッファ空きは落ちない")
{
    // 保証すること: 本エミュレータは送信を即座に完了したものとして扱うので、
    // 空きビットは常に立っていること。
    //
    // 壊れると: IPL-ROM が次の送信で待ち続ける。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kTsr, 0x00);
    CHECK((mfp.peek(x68k::Mfp::kTsr) & 0x80u) != 0);
}

// --- 割り込みの保留と受理 ----------------------------------------------------

TEST_CASE("許可されていない割り込みは保留にもならない")
{
    // 保証すること: IER が 0 のビットは IPR に立たないこと (MC68901 の仕様)。
    //
    // 壊れると: 誰も期待していない割り込みが保留され続け、
    // 受理のたびに関係ないベクタへ飛ぶ。
    x68k::Mfp mfp = makeMfp();
    // IER を立てずにキー入力。
    mfp.receiveKeyboardByte(0x41);

    CHECK(mfp.peek(x68k::Mfp::kIpra) == 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());
}

TEST_CASE("許可された割り込みは IPR に保留される")
{
    // 保証すること: IER が立っていれば IPR に積まれること。
    //
    // 壊れると: キーを押しても何も起きない。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kIera, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);

    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);
}

TEST_CASE("マスクされている間は保留のまま受理されない")
{
    // 保証すること: IMR が 0 のビットは hasPendingInterrupt() に出ないが、
    // IPR には残り続けること。マスクを外せば受理できる。
    //
    // 壊れると: マスク中の割り込みが握りつぶされ、マスクを外しても
    // 二度と上がってこない。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kIera, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);

    // IMR がまだ 0。
    CHECK_FALSE(mfp.hasPendingInterrupt());
    // 保留自体は残っている。
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);

    // マスクを外すと受理できるようになる。
    mfp.write(x68k::Mfp::kImra, x68k::Mfp::kIntRecvFull);
    CHECK(mfp.hasPendingInterrupt());
}

TEST_CASE("受理すると IPR から ISR へ移る")
{
    // 保証すること: acknowledgeInterrupt() が保留を落としてサービス中へ
    // 移すこと。同じ割り込みが二重に受理されない。
    //
    // 壊れると: 同じ割り込みを延々と受理し続け、ハンドラから戻れなくなる。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);
    CHECK(mfp.hasPendingInterrupt());

    mfp.acknowledgeInterrupt();

    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) == 0);
    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntRecvFull) != 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());
}

TEST_CASE("ベクタ番号は VR の上位 4bit と割り込み番号から作られる")
{
    // 保証すること: MFP は自分のベクタ番号を返すデバイスであること。
    //
    // 壊れると: 自動ベクタ (24+6=30) になり、IOCS が未初期化ベクタ用に
    // 埋めている値を PC に読み込んで不正ベクタのハンドラへ飛び、
    // 「エラーが発生しました」で止まる。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x40);
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);

    // グループ A の bit4 (受信バッファフル) は割り込み番号 4+8 = 12。
    CHECK(mfp.acknowledgeInterrupt() == 0x40u + 12u);
}

TEST_CASE("グループ A はグループ B より優先される")
{
    // 保証すること: 優先度がグループ A の bit7 を最上位、グループ B の bit0 を
    // 最下位とする並びになっていること。
    //
    // 壊れると: 優先度の低い割り込みが先に受理され、
    // タイミングに依存した再現しにくい不具合になる。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x40);
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    enableGroupB(mfp, x68k::Mfp::kIntGpip4);

    // 両方を保留にする。
    mfp.receiveKeyboardByte(0x41);
    mfp.write(x68k::Mfp::kAer, 0x00);
    mfp.setVerticalBlank(true);

    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);
    CHECK((mfp.peek(x68k::Mfp::kIprb) & x68k::Mfp::kIntGpip4) != 0);

    // グループ A が先。
    CHECK(mfp.acknowledgeInterrupt() == 0x40u + 12u);
    // 次にグループ B (bit6 = 割り込み番号 6)。
    CHECK(mfp.acknowledgeInterrupt() == 0x40u + 6u);
}

TEST_CASE("保留が無ければ受理は 0 を返す")
{
    // 保証すること: 何も無いときにベクタ番号を作らないこと。
    //
    // 壊れると: 割り込みが無いのに CPU が例外処理へ入る。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x40);
    CHECK(mfp.acknowledgeInterrupt() == 0);
}

TEST_CASE("IPR は 0 を書いたビットだけが落ちる")
{
    // 保証すること: IPR/ISR は「書いたビットを 0 にする」特殊な動作をすること
    // (1 を書いても立たない)。割り込みの取り下げに使う。
    //
    // 壊れると: 通常の代入で実装してしまい、ハンドラが IPR を触った拍子に
    // 別の割り込みを取りこぼす。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);

    // 全ビット 1 を書いても保留は残る。
    mfp.write(x68k::Mfp::kIpra, 0xFF);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);

    // 該当ビットを 0 にした値を書くと落ちる。
    mfp.write(x68k::Mfp::kIpra, static_cast<x68k::u8>(~x68k::Mfp::kIntRecvFull));
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) == 0);
}

// --- 垂直帰線の割り込み ------------------------------------------------------

TEST_CASE("AER で指定したエッジのときだけ垂直帰線割り込みが上がる")
{
    // 保証すること: エッジの向きが AER に従うこと。
    //
    // 壊れると: 帰線の開始と終了の両方で割り込みが上がる (2 倍の頻度) か、
    // 一度も上がらない。Human68k のシステムタイマがずれる。
    x68k::Mfp mfp = makeMfp();
    enableGroupB(mfp, x68k::Mfp::kIntGpip4);

    // AER = 0: 立ち下がり (帰線開始) で割り込み。
    mfp.write(x68k::Mfp::kAer, 0x00);
    mfp.setVerticalBlank(true);
    CHECK((mfp.peek(x68k::Mfp::kIprb) & x68k::Mfp::kIntGpip4) != 0);

    // 帰線終了 (立ち上がり) では上がらない。
    mfp.write(x68k::Mfp::kIprb, static_cast<x68k::u8>(~x68k::Mfp::kIntGpip4));
    mfp.setVerticalBlank(false);
    CHECK((mfp.peek(x68k::Mfp::kIprb) & x68k::Mfp::kIntGpip4) == 0);
}

TEST_CASE("状態が変わらなければ垂直帰線割り込みは上がらない")
{
    // 保証すること: 同じ状態を続けて通知しても割り込みが重ならないこと。
    // CRTC は毎ティック状態を渡してくるので、変化点だけを拾う必要がある。
    //
    // 壊れると: 割り込みが立ちっぱなしになり、CPU がハンドラから
    // 抜け出せなくなる。
    x68k::Mfp mfp = makeMfp();
    enableGroupB(mfp, x68k::Mfp::kIntGpip4);
    mfp.write(x68k::Mfp::kAer, 0x00);

    mfp.setVerticalBlank(true);
    mfp.write(x68k::Mfp::kIprb, static_cast<x68k::u8>(~x68k::Mfp::kIntGpip4));

    // 同じ状態をもう一度。
    mfp.setVerticalBlank(true);
    CHECK((mfp.peek(x68k::Mfp::kIprb) & x68k::Mfp::kIntGpip4) == 0);
}

// --- タイマ ------------------------------------------------------------------

TEST_CASE("制御レジスタが 0 のタイマは止まったまま")
{
    // 保証すること: 分周比 0 = 停止であること。
    //
    // 壊れると: 設定していないタイマが勝手に割り込みを上げる。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, x68k::Mfp::kIntTimerA);
    mfp.write(x68k::Mfp::kTacr, 0x00);
    mfp.write(x68k::Mfp::kTadr, 1);

    mfp.tick(100000);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntTimerA) == 0);
}

TEST_CASE("タイマ A がタイムアウトすると割り込みが上がる")
{
    // 保証すること: Human68k のシステムタイマが動くこと。
    //
    // 壊れると: 時計が進まず、時間待ちをするコードが永久に待つ。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, x68k::Mfp::kIntTimerA);
    // 分周比 1 (4 分周) でデータレジスタ 1。
    mfp.write(x68k::Mfp::kTadr, 1);
    mfp.write(x68k::Mfp::kTacr, 0x01);

    mfp.tick(64);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntTimerA) != 0);
}

TEST_CASE("タイマ C と D は TCDCR の別々のニブルで制御される")
{
    // 保証すること: タイマ C は上位 3bit、D は下位 3bit を見ること。
    //
    // 壊れると: 片方を設定したつもりが両方動く、あるいはどちらも動かない。
    x68k::Mfp mfp = makeMfp();
    enableGroupB(mfp, static_cast<x68k::u8>(x68k::Mfp::kIntTimerC | x68k::Mfp::kIntTimerD));
    mfp.write(x68k::Mfp::kTcdr, 1);
    mfp.write(x68k::Mfp::kTddr, 1);

    // 上位ニブルだけ設定 = タイマ C のみ動く。
    mfp.write(x68k::Mfp::kTcdcr, 0x10);
    mfp.tick(64);

    CHECK((mfp.peek(x68k::Mfp::kIprb) & x68k::Mfp::kIntTimerC) != 0);
    CHECK((mfp.peek(x68k::Mfp::kIprb) & x68k::Mfp::kIntTimerD) == 0);
}

// --- レジスタアクセス --------------------------------------------------------

TEST_CASE("範囲外のレジスタ番号は無視される")
{
    // 保証すること: 存在しないレジスタへのアクセスで配列外に触らないこと。
    //
    // 壊れると: 隣のメンバを壊す。原因が全く別の場所に見える不具合になる。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kRegCount, 0xFF);
    CHECK(mfp.read(x68k::Mfp::kRegCount) == 0);
    CHECK(mfp.read(x68k::Mfp::kRegCount + 100) == 0);
}

TEST_CASE("MFP のレジスタは奇数アドレスに現れる")
{
    // 保証すること: Machine の I/O ディスパッチが $E88000 のオフセットを
    // 2 で割ってレジスタ番号にすること。MFP は 8bit デバイスを 16bit バスへ
    // 繋いでいるので、レジスタは 2 バイトおきに並ぶ。
    //
    // 壊れると: 全レジスタが 1 つずつずれ、どの設定も効かなくなる。
    x68k::Machine m;
    m.reset();

    // VR (レジスタ番号 0x0B) は $E88000 + 0x0B*2 + 1 に現れる。
    m.ioWrite8(x68k::kMfpBase + x68k::Mfp::kVr * 2 + 1, 0x40);
    CHECK(m.mfp().peek(x68k::Mfp::kVr) == 0x40);
    CHECK(m.ioRead8(x68k::kMfpBase + x68k::Mfp::kVr * 2 + 1) == 0x40);
}

TEST_CASE("Machine の reset() で MFP が初期状態へ戻る")
{
    // 保証すること: リセットで GPIP と TSR が起動可能な値になること。
    //
    // 壊れると: ウォームブートのときだけ IPL-ROM の待ちループを
    // 抜けられなくなる。
    x68k::Machine m;
    m.mfp().write(x68k::Mfp::kTsr, 0x00);
    m.mfp().write(x68k::Mfp::kIera, 0xFF);

    m.reset();

    CHECK((m.mfp().peek(x68k::Mfp::kGpip) & 0x02u) == 0);
    CHECK((m.mfp().peek(x68k::Mfp::kGpip) & 0x04u) == 0);
    CHECK((m.mfp().peek(x68k::Mfp::kTsr) & 0x80u) != 0);
    CHECK(m.mfp().peek(x68k::Mfp::kIera) == 0);
}
