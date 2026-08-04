// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: CPU から G-VRAM の 4 つの窓 ($C00000/$C80000/$D00000/$D80000) へ
// 書いたドットが、共有ワードの正しいニブル/バイトへ入り、他のページを壊さないこと。
// そしてその状態をラスタ側がそのまま絵として読めること。
//
// なぜここが要るか: 実 VRAM は 512KB しかないのに CPU 側のアドレス空間は 2MB ある。
// 単純に折り返すと $C80000 への書き込みが $C00000 と同じバイトを潰し、
// 「ページ 1 に描いたつもりがページ 0 が消える」という壊れ方をする。
// 既存の test_graphic_raster.cpp はワード表現 (0x4321 など) を手で組み立てて
// 検証しているので、バスがそのワードを作れるかどうかは一切見ていない。
// 実機のソフトはバス経由でしか VRAM を触れないので、この経路が唯一の入口になる。
//
// 実機の根拠 (rom/iplrom.dat = EXPERT 用 IPLROM v1.0、$FE0000 がファイル先頭):
//
//   ページ N の先頭 = $C00000 + N * $80000
//     $FFAEE8: MOVE.W D1,D0 / AND.W #$0003,D0 / ASL.W #3,D0 / ADD.W #$00C0,D0
//              / SWAP D0 / CLR.W D0 / MOVE.L D0,$095C
//     $FFB268 に同じ計算があり、$FFB282 に逆変換 (SWAP / SUB.W #$00C0 / LSR.W #3
//     / AND.L #3) がある。
//
//   窓 1 つが実 VRAM 全体を覆う (= ページごとに VRAM が分かれてはいない)
//     $FFAAB4: LEA $C00000,A0 / MOVE.L A0,$095C / LEA $C80000,A1 / BSR $FFABC0
//     $FFABC0: CLR.L (A0)+ / CMPA.L A1,A0 / BNE.S -6
//     512KB ぶんの消去だけで 4 ページ全部が消える。
//
//   1 ドットは MOVE.W で書き、値はゼロ拡張された 4bit (16 色モードの場合)
//     $FFB0A0: MOVE.B (A1)+,D5 / MOVE.B D5,D0 / LSR.B #4,D0 / AND.W #$000F,D0
//              / MOVE.W D0,(A0)+ / AND.W #$000F,D5 / MOVE.W D5,(A0)+
//     4bit しか入っていないワードを書いて、残りのページが無事であることを
//     ハードウェアに任せている。読み側も $FFAF2C で MOVE.W (A0),D0 /
//     AND.W #$000F,D0 と、ワードで読んでからマスクする。

#include <vector>

#include "bus.h"
#include "dev/video.h"
#include "doctest.h"
#include "video/graphic_raster.h"

namespace
{

// I/O には来ない前提のダミー。バスの構築に要るだけ。
class NullIo final : public x68k::IoHandler
{
public:
    x68k::u8 ioRead8(x68k::u32) override
    {
        return 0;
    }
    void ioWrite8(x68k::u32, x68k::u8) override {}
    x68k::u16 ioRead16(x68k::u32) override
    {
        return 0;
    }
    void ioWrite16(x68k::u32, x68k::u16) override {}
};

// $E82400 (画面モード R0) の VideoController 内でのオフセット。
constexpr x68k::u32 kScreenModeOffset = 0x400;

// バス + G-VRAM + ビデオコントローラ一式。
struct Gvram
{
    std::vector<x68k::u8> vram;
    x68k::Sram sram;
    NullIo io;
    x68k::VideoController video;
    x68k::SystemBus bus;

    Gvram() : vram(x68k::kTvramSize, 0), bus(x68k::MemoryMap{}, sram, io)
    {
        x68k::MemoryMap memory;
        memory.graphicVram = vram.data();
        bus.setMemory(memory);
        video.reset();
        bus.setVideoController(&video);
    }

    void setColorMode(x68k::VideoController::GraphicColorMode mode)
    {
        video.write(kScreenModeOffset, static_cast<x68k::u16>(mode));
    }

    // ページ page の窓での、座標 (x, y) のアドレス。1 ドット 1 ワード。
    static x68k::u32 addrOf(x68k::u32 page, x68k::u32 x, x68k::u32 y)
    {
        return x68k::kGvramBase + page * 0x80000u + ((y * x68k::kGvramPageWidth) + x) * 2u;
    }

    // 実 VRAM に載っている生のワード。バスを通さずに覗く。
    [[nodiscard]] x68k::u16 rawWord(x68k::u32 x, x68k::u32 y) const
    {
        const std::size_t off = (static_cast<std::size_t>(y) * x68k::kGvramPageWidth + x) * 2u;
        return static_cast<x68k::u16>((static_cast<x68k::u16>(vram[off]) << 8) | vram[off + 1]);
    }
};

}  // namespace

// --- 16 色モード -------------------------------------------------------------

TEST_CASE("16 色: 4 つの窓がそれぞれ別のニブルへ書き込む")
{
    // 保証すること: $C00000/$C80000/$D00000/$D80000 への MOVE.W が、
    // 共有ワードのニブル 0/1/2/3 へ順に入ること。
    //
    // 壊れると: 折り返しでどの窓も同じバイトを指し、ページ 1 に書いた値が
    // ページ 0 を潰す。実機のソフトはページを裏画面に使うので、
    // 描画中の絵がそのまま表画面に出てちらつく、では済まず絵が壊れる。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k16Color);

    g.bus.write16(Gvram::addrOf(0, 0, 0), 0x0001);
    g.bus.write16(Gvram::addrOf(1, 0, 0), 0x0002);
    g.bus.write16(Gvram::addrOf(2, 0, 0), 0x0003);
    g.bus.write16(Gvram::addrOf(3, 0, 0), 0x0004);

    // 上位ニブルからページ 3,2,1,0。
    CHECK(g.rawWord(0, 0) == 0x4321);
}

TEST_CASE("16 色: あるページへの書き込みが他のページを壊さない")
{
    // 保証すること: 窓ごとの書き込みが read-modify-write になっていて、
    // 同じワードに同居する他ページのニブルを保つこと。
    //
    // 壊れると: 最後に書いたページだけが残り、それ以外が 0 になる。
    // 「4 ページに絵を用意してから表示を切り替える」という定石が成立しない。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k16Color);

    // 4 ページすべてを埋めてから、ページ 1 だけを書き換える。
    g.bus.write16(Gvram::addrOf(0, 3, 5), 0x000A);
    g.bus.write16(Gvram::addrOf(1, 3, 5), 0x000B);
    g.bus.write16(Gvram::addrOf(2, 3, 5), 0x000C);
    g.bus.write16(Gvram::addrOf(3, 3, 5), 0x000D);
    CHECK(g.rawWord(3, 5) == 0xDCBA);

    g.bus.write16(Gvram::addrOf(1, 3, 5), 0x0007);
    CHECK(g.rawWord(3, 5) == 0xDC7A);
}

TEST_CASE("16 色: 窓から読むと自分のページのニブルだけが返る")
{
    // 保証すること: 読み出しがページのニブルをゼロ拡張した値になること。
    //
    // 壊れると: ページ 1 を読むとページ 0 のニブルが混ざる。IPL-ROM は
    // MOVE.W で読んでから AND.W #$000F する ($FFAF2C) のでそこは救われるが、
    // 「読んで書き戻す」描画は他ページの絵を自分のページへ焼き付ける。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k16Color);

    // 実 VRAM に直接 0x4321 を置く (ページ 0=1, 1=2, 2=3, 3=4)。
    g.vram[0] = 0x43;
    g.vram[1] = 0x21;

    CHECK(g.bus.read16(Gvram::addrOf(0, 0, 0)) == 0x0001);
    CHECK(g.bus.read16(Gvram::addrOf(1, 0, 0)) == 0x0002);
    CHECK(g.bus.read16(Gvram::addrOf(2, 0, 0)) == 0x0003);
    CHECK(g.bus.read16(Gvram::addrOf(3, 0, 0)) == 0x0004);
}

TEST_CASE("16 色: 座標は窓が変わってもずれない")
{
    // 保証すること: どの窓から触っても、同じ (x, y) が実 VRAM の同じワードに
    // 当たること。
    //
    // 壊れると: 窓ごとに実 VRAM のオフセットをずらす実装になり、
    // ページ 1 に描いた絵がページ 0 と違う場所に出る。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k16Color);

    g.bus.write16(Gvram::addrOf(2, 100, 200), 0x0005);
    CHECK(g.rawWord(100, 200) == 0x0500);
    // 他の座標は無傷。
    CHECK(g.rawWord(100, 199) == 0x0000);
    CHECK(g.rawWord(99, 200) == 0x0000);
}

// --- 256 色モード -----------------------------------------------------------

TEST_CASE("256 色: 2 つの窓がワードの下位/上位バイトへ分かれる")
{
    // 保証すること: $C00000 がページ 0 (下位バイト)、$C80000 がページ 1
    // (上位バイト) になり、互いを壊さないこと。
    //
    // 壊れると: 256 色 2 ページを使う構成でページ 1 を書いた瞬間に
    // ページ 0 の絵が消える。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k256Color);

    g.bus.write16(Gvram::addrOf(0, 7, 9), 0x0012);
    CHECK(g.rawWord(7, 9) == 0x0012);

    g.bus.write16(Gvram::addrOf(1, 7, 9), 0x00AB);
    CHECK(g.rawWord(7, 9) == 0xAB12);

    CHECK(g.bus.read16(Gvram::addrOf(0, 7, 9)) == 0x0012);
    CHECK(g.bus.read16(Gvram::addrOf(1, 7, 9)) == 0x00AB);
}

TEST_CASE("256 色: 8bit を超える値は切り捨てられる")
{
    // 保証すること: ページに収まらない上位ビットが隣のページへこぼれないこと。
    //
    // 壊れると: $C00000 へ $12AB を書くと $AB がページ 1 へ漏れる。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k256Color);

    g.bus.write16(Gvram::addrOf(1, 0, 0), 0x0055);
    g.bus.write16(Gvram::addrOf(0, 0, 0), 0x12AB);
    CHECK(g.rawWord(0, 0) == 0x55AB);
}

TEST_CASE("256 色: 窓 2 と 3 はページ 0 と 1 の繰り返しになる")
{
    // 保証すること: 256 色モードで使うページは 2 つなので、$D00000 / $D80000 が
    // $C00000 / $C80000 と同じページを指すこと。
    //
    // 壊れると: 存在しないページ 2/3 のためにワードの外へシフトし、
    // 書き込みがどこにも当たらない (絵が出ない) か、別のビットを壊す。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k256Color);

    g.bus.write16(Gvram::addrOf(2, 1, 1), 0x0033);
    CHECK(g.rawWord(1, 1) == 0x0033);
    g.bus.write16(Gvram::addrOf(3, 1, 1), 0x0044);
    CHECK(g.rawWord(1, 1) == 0x4433);
}

// --- 65536 色モード ---------------------------------------------------------

TEST_CASE("65536 色: ワード全体が 1 ドットになる")
{
    // 保証すること: ページの概念が無く、16bit がそのまま実 VRAM へ載ること。
    //
    // 壊れると: 4bit や 8bit に切り詰められて色が化ける。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k65536Color);

    g.bus.write16(Gvram::addrOf(0, 2, 3), 0xBEEF);
    CHECK(g.rawWord(2, 3) == 0xBEEF);
    CHECK(g.bus.read16(Gvram::addrOf(0, 2, 3)) == 0xBEEF);

    // どの窓から触っても同じワードに当たる。
    g.bus.write16(Gvram::addrOf(3, 2, 3), 0x1234);
    CHECK(g.rawWord(2, 3) == 0x1234);
}

TEST_CASE("65536 色: バイトアクセスがワードの上位/下位を選ぶ")
{
    // 保証すること: 1 ドットが 2 バイトにまたがるモードで、
    // 偶数番地が上位バイト、奇数番地が下位バイトになること。
    //
    // 壊れると: read16 を read8 2 回に分ける経路が同じバイトを 2 度読み、
    // 色の上位が下位の値に化ける。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k65536Color);

    const x68k::u32 addr = Gvram::addrOf(0, 4, 4);
    g.bus.write8(addr, 0xAB);
    g.bus.write8(addr + 1, 0xCD);
    CHECK(g.rawWord(4, 4) == 0xABCD);
    CHECK(g.bus.read8(addr) == 0xAB);
    CHECK(g.bus.read8(addr + 1) == 0xCD);
    CHECK(g.bus.read16(addr) == 0xABCD);
}

// --- バイトアクセス (16 色) --------------------------------------------------

TEST_CASE("16 色: バイト書き込みが自分のページのニブルだけに効く")
{
    // 保証すること: MOVE.B でドットを書いても、ワード単位の書き込みと
    // 同じページ・同じニブルへ入ること。
    //
    // 壊れると: 生のバイトをそのまま書いてしまい、隣のページのニブルを潰す。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k16Color);

    g.bus.write16(Gvram::addrOf(0, 0, 0), 0x0009);
    // 1 ドットは 1 ワードなので、奇数番地 (下位バイト) がドットの入り口。
    g.bus.write8(Gvram::addrOf(1, 0, 0) + 1, 0x06);
    CHECK(g.rawWord(0, 0) == 0x0069);
    CHECK(g.bus.read8(Gvram::addrOf(1, 0, 0) + 1) == 0x06);
    CHECK(g.bus.read8(Gvram::addrOf(0, 0, 0) + 1) == 0x09);
}

// --- ラスタとの突き合わせ ---------------------------------------------------

TEST_CASE("バス経由で書いた 16 色のドットをラスタが同じページで拾う")
{
    // 保証すること: バスの書き込み表現とラスタの読み出し表現が一致すること。
    //
    // これが本来いちばん守りたい性質。バスとラスタが別々に「正しい」形を
    // 持っていても、両者が食い違っていれば実機のソフトが作れる状態を
    // ラスタが描けない。既存のラスタ側テストはワードを手で組むので、
    // この食い違いを構造的に検出できない。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k16Color);

    for (x68k::u32 page = 0; page < 4; ++page)
    {
        g.bus.write16(Gvram::addrOf(page, 10, 20), static_cast<x68k::u16>(page + 1));
    }

    for (x68k::u32 page = 0; page < 4; ++page)
    {
        CHECK(x68k::GraphicRaster::pixelIndex(g.vram.data(),
                                              x68k::VideoController::GraphicColorMode::k16Color,
                                              page, 10, 20) == page + 1);
    }
}

TEST_CASE("バス経由で書いた 256 色のドットをラスタが同じページで拾う")
{
    // 保証すること: 256 色でもバスとラスタのページ解釈が揃うこと。
    //
    // 壊れると: ページ 1 に書いた絵をラスタがページ 0 として読み、
    // 別の色が出る。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k256Color);

    g.bus.write16(Gvram::addrOf(0, 30, 40), 0x0071);
    g.bus.write16(Gvram::addrOf(1, 30, 40), 0x00E2);

    CHECK(x68k::GraphicRaster::pixelIndex(g.vram.data(),
                                          x68k::VideoController::GraphicColorMode::k256Color, 0, 30,
                                          40) == 0x71);
    CHECK(x68k::GraphicRaster::pixelIndex(g.vram.data(),
                                          x68k::VideoController::GraphicColorMode::k256Color, 1, 30,
                                          40) == 0xE2);
}

TEST_CASE("ページ 1 に書いた絵をラスタが表示ページとして描ける")
{
    // 保証すること: 実機のソフトがやる手順 (ページ 1 に描いて、$E82600 で
    // ページ 1 だけを表示させる) が、バス → ラスタで一貫して通ること。
    //
    // 壊れると: バスがページ 1 の書き込みをページ 0 へ落とすので、
    // 「ページ 1 だけ表示」にした瞬間に画面が空になる。
    Gvram g;
    g.setColorMode(x68k::VideoController::GraphicColorMode::k16Color);

    // パレット 5 番に色を入れておく ($E82000 + 5*2)。
    constexpr x68k::u16 kGreen = 0x07C0;
    g.video.write(5 * 2, kGreen);
    // グラフィック表示を許可し、ページ 1 だけを表示対象にする ($E82600)。
    g.video.write(0x600, 0x0012);  // bit4=グラフィック表示, bit1=ページ 1

    g.bus.write16(Gvram::addrOf(1, 0, 0), 0x0005);

    x68k::u16 out[4] = {0, 0, 0, 0};
    x68k::GraphicRaster::render(g.vram.data(), g.video, 0, 0, 2, 2, out, 2);

    CHECK(out[0] == x68k::VideoController::toRgb565(kGreen));
    // 隣のドットは透明のまま。
    CHECK(out[1] == 0);
}
