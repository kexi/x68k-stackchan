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

TEST_CASE("受理すると保留が落ちる")
{
    // 保証すること: acknowledgeInterrupt() が IPR を落とすこと。
    // 同じ割り込みが二重に受理されない。
    //
    // 壊れると: 同じ割り込みを延々と受理し続け、ハンドラから戻れなくなる。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);
    CHECK(mfp.hasPendingInterrupt());

    mfp.acknowledgeInterrupt();

    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) == 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());
}

TEST_CASE("Automatic EOI では ISR が立たない")
{
    // 保証すること: VR bit3 (S) が 0 のとき、受理しても ISR は 0 のままで
    // あること。S=0 は受理と同時にサービス完了として扱う方式。
    //
    // 壊れると: 実機では常に 0 のはずの ISR が埋まっていく。X68000 の
    // IOCS は VR に $40 を書く (S=0) ので、実機で使われるのはこちら。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x40);  // S=0
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);

    mfp.acknowledgeInterrupt();

    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntRecvFull) == 0);
}

TEST_CASE("Software EOI では ISR が立つ")
{
    // 保証すること: VR bit3 (S) が 1 のとき、受理で ISR が立つこと。
    // ハンドラが明示的に落とすまでサービス中として残る。
    //
    // 壊れると: S=1 を使うプログラムがサービス中かどうかを判断できない。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x48);  // S=1
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);

    mfp.acknowledgeInterrupt();

    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntRecvFull) != 0);
}

TEST_CASE("Software EOI ではサービス中より下位の割り込みを受理しない")
{
    // 保証すること: S=1 で ISR が立っている間、それ以下の優先度の割り込みが
    // 受理されないこと。ISR を立てるだけで優先度を見ないなら意味がない。
    //
    // 壊れると: ハンドラの実行中に同じ割り込みが再入し、スタックを食い潰す。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x48);  // S=1
    // 受信バッファフル ($10) の方がタイマ B ($01) より優先度が高い。
    enableGroupA(mfp, static_cast<x68k::u8>(x68k::Mfp::kIntRecvFull | x68k::Mfp::kIntTimerB));

    mfp.receiveKeyboardByte(0x41);
    CHECK(mfp.acknowledgeInterrupt() != 0);
    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntRecvFull) != 0);

    // サービス中に下位の割り込みが上がっても受理できない。
    mfp.write(x68k::Mfp::kTbdr, 1);
    mfp.write(x68k::Mfp::kTbcr, 0x01);
    mfp.tickFast<true>(4 * 2);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntTimerB) != 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());
    CHECK(mfp.acknowledgeInterrupt() == 0);

    // ISR を落とせば受理できるようになる。
    mfp.write(x68k::Mfp::kIsra, static_cast<x68k::u8>(~x68k::Mfp::kIntRecvFull));
    CHECK(mfp.hasPendingInterrupt());
    CHECK(mfp.acknowledgeInterrupt() != 0);
}

TEST_CASE("Software EOI でもサービス中より上位の割り込みは受理する")
{
    // 保証すること: 抑止するのは「以下」だけで、上位の割り込みは通ること。
    //
    // 壊れると: 低優先度のハンドラ実行中に、キー入力やタイマなど本来
    // 割り込めるはずのものが止まる。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x48);  // S=1
    enableGroupA(mfp, static_cast<x68k::u8>(x68k::Mfp::kIntRecvFull | x68k::Mfp::kIntTimerB));

    // 先に下位 (タイマ B) をサービス中にする。
    mfp.write(x68k::Mfp::kTbdr, 1);
    mfp.write(x68k::Mfp::kTbcr, 0x01);
    mfp.tickFast<true>(4 * 2);
    CHECK(mfp.acknowledgeInterrupt() != 0);
    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntTimerB) != 0);

    // 上位 (受信バッファフル) は割り込める。
    mfp.receiveKeyboardByte(0x41);
    CHECK(mfp.hasPendingInterrupt());
    CHECK(mfp.acknowledgeInterrupt() != 0);
}

TEST_CASE("グループ A のサービス中はグループ B を全て抑止する")
{
    // 保証すること: 優先度がグループをまたいで一列に並ぶこと。
    // グループ A はすべてグループ B より上位。
    //
    // 壊れると: グループごとに独立した優先度になり、A のハンドラ実行中に
    // B が割り込む。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x48);           // S=1
    enableGroupA(mfp, x68k::Mfp::kIntTimerB);  // グループ A の最下位
    enableGroupB(mfp, x68k::Mfp::kIntGpip4);

    mfp.write(x68k::Mfp::kTbdr, 1);
    mfp.write(x68k::Mfp::kTbcr, 0x01);
    mfp.tickFast<true>(4 * 2);
    CHECK(mfp.acknowledgeInterrupt() != 0);

    // グループ B は A の最下位より下位なので受理されない。
    mfp.write(x68k::Mfp::kAer, 0x00);
    mfp.setVerticalBlank(true);
    CHECK((mfp.peek(x68k::Mfp::kIprb) & x68k::Mfp::kIntGpip4) != 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());
}

TEST_CASE("Software EOI では同じチャネルの再入も抑止する")
{
    // 保証すること: サービス中のチャネル自身が再び保留になっても、
    // 受理されないこと。抑止するのは「真に高い優先度だけ通す」であって、
    // 同じ優先度は通らない。
    //
    // 壊れると: ハンドラの実行中に同じ割り込みが再入し、スタックを
    // 食い潰す。別チャネルで見るテストだけだと、サービス中のビット自身を
    // 許可してしまう実装を見逃す。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x48);  // S=1
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);

    mfp.receiveKeyboardByte(0x41);
    CHECK(mfp.acknowledgeInterrupt() != 0);
    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntRecvFull) != 0);

    // 同じチャネルがもう一度上がっても受理されない。
    mfp.receiveKeyboardByte(0x42);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());
    CHECK(mfp.acknowledgeInterrupt() == 0);
}

TEST_CASE("Software EOI の優先度基準は最上位のサービス中ビット")
{
    // 保証すること: ISR に複数のビットが立っているとき、最も優先度の高い
    // ものを基準にすること。
    //
    // 壊れると: 最下位を基準にする実装だと、その間の優先度が誤って
    // 受理される。ネストしたハンドラの途中で下位が割り込む。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x48);  // S=1
    // タイマ A ($20) > 受信バッファフル ($10) > タイマ B ($01)。
    enableGroupA(mfp, static_cast<x68k::u8>(x68k::Mfp::kIntTimerA | x68k::Mfp::kIntRecvFull |
                                            x68k::Mfp::kIntTimerB));

    // 最下位 (タイマ B) と最上位 (タイマ A) をサービス中にする。間に
    // 受信バッファフルが挟まる並びを作るのが要点。ISR はレジスタ直書きで
    // 立てられないので、実際の受理経路を通す。
    mfp.write(x68k::Mfp::kTbdr, 1);
    mfp.write(x68k::Mfp::kTbcr, 0x01);
    mfp.tickFast<true>(4 * 2);
    CHECK(mfp.acknowledgeInterrupt() != 0);
    mfp.write(x68k::Mfp::kTadr, 1);
    mfp.write(x68k::Mfp::kTacr, 0x01);
    mfp.tickFast<true>(4 * 2);
    CHECK(mfp.acknowledgeInterrupt() != 0);
    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntTimerA) != 0);
    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntTimerB) != 0);

    // 中間の受信バッファフルを保留にする。最上位 (タイマ A) が基準なら
    // 通らない。最下位 (タイマ B) を基準にする実装だと誤って通る。
    mfp.receiveKeyboardByte(0x41);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());

    // タイマ A を落とすと基準がタイマ B に下がり、受信が通るようになる。
    mfp.write(x68k::Mfp::kIsra, static_cast<x68k::u8>(~x68k::Mfp::kIntTimerA));
    CHECK(mfp.hasPendingInterrupt());
}

TEST_CASE("Automatic EOI へ切り替えると ISR が捨てられる")
{
    // 保証すること: VR の S を 0 にした時点で ISRA/ISRB の両方が 0 に
    // なること。S=0 の間、ISR は強制的に 0 に保たれる。
    //
    // 壊れると: Software EOI から切り替えた後もサービス中とみなされ、
    // それ以下の優先度が二度と受理されなくなる。
    //
    // Why not グループ A だけ見ないか: ISRB を触り忘れた実装でも、
    // A だけのテストなら通ってしまう。
    x68k::Mfp mfp = makeMfp();
    mfp.write(x68k::Mfp::kVr, 0x48);  // S=1
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    enableGroupB(mfp, x68k::Mfp::kIntGpip4);

    // 先にグループ B をサービス中にする。A を先に受理すると B が
    // 抑止されて受理できない (ISR は書き込みで立てられない。0 を書いた
    // ビットが落ちるだけ)。
    mfp.write(x68k::Mfp::kAer, 0x00);
    mfp.setVerticalBlank(true);
    CHECK(mfp.acknowledgeInterrupt() != 0);
    CHECK((mfp.peek(x68k::Mfp::kIsrb) & x68k::Mfp::kIntGpip4) != 0);

    // グループ A は B より上位なので、B のサービス中でも受理できる。
    mfp.receiveKeyboardByte(0x41);
    CHECK(mfp.acknowledgeInterrupt() != 0);
    CHECK((mfp.peek(x68k::Mfp::kIsra) & x68k::Mfp::kIntRecvFull) != 0);

    mfp.write(x68k::Mfp::kVr, 0x40);  // S=0 へ切り替え

    CHECK(mfp.peek(x68k::Mfp::kIsra) == 0);
    CHECK(mfp.peek(x68k::Mfp::kIsrb) == 0);
}

TEST_CASE("割り込みを禁止すると保留も落ちる")
{
    // 保証すること: IER のビットを 0 にすると、対応する IPR のビットも
    // 同時に落ちること (MC68901 の仕様)。
    //
    // 壊れると: 割り込みを止めた後も保留が残り、ハンドラを外した後で
    // 受理されて不正なベクタへ飛ぶ。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, x68k::Mfp::kIntRecvFull);
    mfp.receiveKeyboardByte(0x41);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) != 0);

    mfp.write(x68k::Mfp::kIera, 0x00);

    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) == 0);
    CHECK_FALSE(mfp.hasPendingInterrupt());
}

TEST_CASE("割り込みの禁止は他のチャネルの保留を巻き込まない")
{
    // 保証すること: IER への書き込みが、禁止したビット以外の保留を
    // 落とさないこと。
    //
    // 壊れると: 1 つのチャネルを止めるたびに、動いている他の割り込みが
    // 取りこぼされる。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, static_cast<x68k::u8>(x68k::Mfp::kIntRecvFull | x68k::Mfp::kIntTimerA));

    // 両方を保留にする。
    mfp.receiveKeyboardByte(0x41);
    mfp.write(x68k::Mfp::kTadr, 1);
    mfp.write(x68k::Mfp::kTacr, 0x01);
    mfp.tickFast<true>(4 * 2);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntTimerA) != 0);

    // 受信だけ禁止する。タイマ A の保留は残るはず。
    mfp.write(x68k::Mfp::kIera, x68k::Mfp::kIntTimerA);

    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntRecvFull) == 0);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntTimerA) != 0);
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

    mfp.tickFast<true>(100000);
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

    mfp.tickFast<true>(64);
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
    mfp.tickFast<true>(64);

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

TEST_CASE("動作中にデータレジスタへ書いても現在の周期は変わらない")
{
    // 保証すること: MC68901 は動作中のタイマに書いた値を、カウンタが
    // 0 まで下りてリロードするときに初めて反映すること。停止中なら即座。
    //
    // 壊れると: 書いた瞬間にカウンタが再スタートし、割り込みの間隔が
    // 変わる。時計が狂う形の不具合になり、原因を追いにくい。
    x68k::Mfp mfp = makeMfp();
    enableGroupA(mfp, x68k::Mfp::kIntTimerA);

    // 分周比 1 (4 分周)、データ 4 で動かす。
    mfp.write(x68k::Mfp::kTadr, 4);
    mfp.write(x68k::Mfp::kTacr, 0x01);

    // 1 カウントぶん進める。CPU サイクルは MFP サイクルの 2 倍で数える。
    mfp.tickFast<true>(4 * 2);
    // read はライブカウンタを返す。peek は生のレジスタ (次のリロード値) を
    // 返すテスト用の窓口なので、ここでは read を使う。
    const x68k::u8 afterOneCount = mfp.read(x68k::Mfp::kTadr);
    CHECK(afterOneCount == 3);

    // 動作中に大きな値を書く。カウンタは動かないはず。
    mfp.write(x68k::Mfp::kTadr, 200);
    CHECK(mfp.read(x68k::Mfp::kTadr) == 3);

    // 残り 3 カウントで最初のタイムアウト。書いた 200 ではない。
    mfp.tickFast<true>(4 * 2 * 3);
    CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntTimerA) != 0);

    // リロードされた値は書いた 200。
    CHECK(mfp.read(x68k::Mfp::kTadr) == 200);
}

TEST_CASE("停止中のデータレジスタへの書き込みは即座に効く")
{
    // 保証すること: 停止中 (制御レジスタが 0) なら、書いた値がそのまま
    // カウンタへ入ること。
    //
    // 壊れると: 初期化のために「止めて値を入れて動かす」手順が効かず、
    // 前の値のまま動き出す。
    x68k::Mfp mfp = makeMfp();

    mfp.write(x68k::Mfp::kTacr, 0x00);  // 停止
    mfp.write(x68k::Mfp::kTadr, 42);
    CHECK(mfp.read(x68k::Mfp::kTadr) == 42);
}

TEST_CASE("TAI/TBI を要するモードのタイマは経過サイクルで進まない")
{
    // 保証すること: 制御値 $08 (イベントカウント) と $09-$0F (パルス幅測定) の
    // タイマが、tick() では減らないこと。$08 は入力の有効エッジがカウント源、
    // $09-$0F は入力が有効な間だけ内部クロックで動く。本エミュレータは
    // TAI/TBI を実装していないので、どちらも進めようがない。
    //
    // 壊れると: 下位 3bit が分周比として拾われ、入力に関係なくカウンタが
    // 減り続ける。$0C なら 64 分周のタイマとして勝手に割り込みを上げる。
    for (const x68k::u8 control : {x68k::u8{0x08}, x68k::u8{0x09}, x68k::u8{0x0C}, x68k::u8{0x0F}})
    {
        CAPTURE(control);
        x68k::Mfp mfp = makeMfp();
        enableGroupA(mfp, x68k::Mfp::kIntTimerA);

        mfp.write(x68k::Mfp::kTadr, 4);
        mfp.write(x68k::Mfp::kTacr, control);

        // 最大の分周比 (200) で 4 カウントぶん回しても足りるだけ進める。
        mfp.tickFast<true>(200 * 2 * 8);

        CHECK(mfp.read(x68k::Mfp::kTadr) == 4);
        CHECK((mfp.peek(x68k::Mfp::kIpra) & x68k::Mfp::kIntTimerA) == 0);
    }
}

TEST_CASE("停止するとプリスケーラの端数は捨てられる")
{
    // 保証すること: 実機は停止でメインカウンタを保つがプリスケーラの残量は
    // 失うこと。止めて設定し直して再開したとき、最初の 1 カウントが
    // 丸ごと必要になる。
    //
    // 壊れると: 端数が残ったまま再開し、最初の周期だけ早く減る。
    // 「止めて設定して動かす」を繰り返すコードで割り込み間隔がずれる。
    x68k::Mfp mfp = makeMfp();

    mfp.write(x68k::Mfp::kTadr, 10);
    mfp.write(x68k::Mfp::kTacr, 0x01);  // 4 分周

    // 1 カウントに満たない端数だけ進める。
    mfp.tickFast<true>(3 * 2);
    CHECK(mfp.read(x68k::Mfp::kTadr) == 10);

    // 停止して再開する。端数が残っていれば 1 MFP サイクルで減ってしまう。
    mfp.write(x68k::Mfp::kTacr, 0x00);
    mfp.write(x68k::Mfp::kTacr, 0x01);

    mfp.tickFast<true>(3 * 2);
    CHECK(mfp.read(x68k::Mfp::kTadr) == 10);

    // 4 サイクル目でようやく減る。
    mfp.tickFast<true>(1 * 2);
    CHECK(mfp.read(x68k::Mfp::kTadr) == 9);
}

TEST_CASE("TCDCR はタイマ C と D のプリスケーラを別々に捨てる")
{
    // 保証すること: TCDCR は 1 バイトで 2 つのタイマを制御する。片方だけを
    // 止めたとき、もう片方の端数まで捨てないこと。C を止めた場合と D を
    // 止めた場合の両方を見る。
    //
    // 壊れると: タイマ C を止めるたびに動作中のタイマ D の周期が延びる。
    // Human68k はタイマ C を割り込みに使うため、実害が出る。
    //
    // Why not 片方向だけ見ないか: ビット位置とタイマ番号の対応を
    // 取り違えた実装 (C の停止で D の端数を捨てる、など) は、片方向の
    // テストだけだと素通りする。
    struct Case
    {
        const char* name;
        x68k::u8 stopped;      // 片方だけ止めた制御値
        x68k::u32 clearedReg;  // 端数を捨てられた側のデータレジスタ
        x68k::u32 keptReg;     // 端数が残る側
    };

    // C は bits 6-4、D は bits 2-0。0x11 は両方 4 分周。
    const Case cases[] = {
        {"C だけ止める", 0x01, x68k::Mfp::kTcdr, x68k::Mfp::kTddr},
        {"D だけ止める", 0x10, x68k::Mfp::kTddr, x68k::Mfp::kTcdr},
    };

    for (const Case& c : cases)
    {
        CAPTURE(c.name);
        x68k::Mfp mfp = makeMfp();

        mfp.write(x68k::Mfp::kTcdr, 10);
        mfp.write(x68k::Mfp::kTddr, 10);
        mfp.write(x68k::Mfp::kTcdcr, 0x11);

        // 両方を 1 カウントに満たない端数だけ進める。
        mfp.tickFast<true>(3 * 2);

        // 片方だけ止めて再開する。もう片方は動かしたまま。
        mfp.write(x68k::Mfp::kTcdcr, c.stopped);
        mfp.write(x68k::Mfp::kTcdcr, 0x11);

        // 1 サイクル進めると、端数が残っている側は減り、捨てられた側は残る。
        mfp.tickFast<true>(1 * 2);
        CHECK(mfp.read(c.clearedReg) == 10);
        CHECK(mfp.read(c.keptReg) == 9);
    }
}
