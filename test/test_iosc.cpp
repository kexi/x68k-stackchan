// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: I/O 割り込みコントローラ ($E9C000) が、IPL-ROM が実際に書く
// 手順どおりに設定でき、FDC の割り込みを正しいベクタ番号で配送すること。
//
// このデバイスは「ビットの割り当てを間違えても静かに動いているように見える」
// のが厄介なので、ROM の実測値を直接テストの前提に据える。
//
//   $FF0CC2: MOVE.B #$60,$E9C003   ← ベクタレジスタ
//   $FF0D04: MOVE.B #$06,$E9C001   ← 割り込み許可 ($06 = FDC + FDD)
//   $FF9F14: ベクタ $63 にハンドラを入れて BSET #0 (プリンタ許可)
//   $FF81CC: BTST #5,$E9C001       ← プリンタ BUSY
//
// 特に重要なのが「$06 で FDC がもう許可されている」こと。ここを取り違えて
// bit0 を FDC だと解釈すると、ROM は bit0 を立てないので FDC 割り込みが
// 永久に配送されず、しかも他のテストは全部通ってしまう。
//
//   $E9C001 の並び (MAME の x68k ドライバと一致):
//     bit7 FDC 発生 / bit6 FDD 発生 / bit5 プリンタ BUSY / bit4 HDD 発生
//     bit3 HDD 許可 / bit2 FDC 許可 / bit1 FDD 許可 / bit0 プリンタ許可
//
//   ベクタ番号 = ($E9C003 & $FC) | デバイス番号
//     デバイス 0=FDC 1=FDD 2=HDD 3=プリンタ

#include "dev/iosc.h"
#include "doctest.h"

namespace
{

// レジスタのオフセット。
constexpr x68k::u32 kRegEnable = 0x01;  // $E9C001
constexpr x68k::u32 kRegVector = 0x03;  // $E9C003

// IPL-ROM が書く値。
constexpr x68k::u8 kRomVector = 0x60;      // $FF0CC2
constexpr x68k::u8 kRomEnableInit = 0x06;  // $FF0D04

// ROM の初期化をそのまま再現する。
x68k::IoSc makeRomInitialized()
{
    x68k::IoSc iosc;
    iosc.reset();
    iosc.write(kRegVector, kRomVector);
    iosc.write(kRegEnable, kRomEnableInit);
    return iosc;
}

}  // namespace

TEST_SUITE("iosc")
{
    // これが本命。ROM が書く $06 だけで FDC 割り込みが通ること。
    // bit0 を FDC と誤解していると、ここだけが落ちる。
    TEST_CASE("ROM の初期値 $06 だけで FDC 割り込みがベクタ $60 で配送される")
    {
        x68k::IoSc iosc = makeRomInitialized();

        // 追加の許可操作は一切しない。ROM もしないため。
        iosc.setSource(x68k::IoSc::kDeviceFdc, true);

        CHECK(iosc.hasPendingInterrupt());
        // $60 = IPL-ROM が $180 に張った FDC ハンドラ ($FF1130) のベクタ。
        CHECK(iosc.acknowledgeInterrupt() == 0x60);
    }

    // 二つのレジスタが独立していること。取り違えを直接捕まえる。
    TEST_CASE("ベクタレジスタと割り込み許可は別のレジスタである")
    {
        x68k::IoSc iosc = makeRomInitialized();

        CHECK(iosc.vectorRegister() == kRomVector);
        CHECK(iosc.enableRegister() == kRomEnableInit);
        CHECK(iosc.read(kRegVector) == kRomVector);
        // 状態ビットが立っていなければ、読み戻しは許可ビットそのもの。
        CHECK(iosc.read(kRegEnable) == kRomEnableInit);
    }

    // $06 = bit2|bit1 が FDC と FDD であること。
    TEST_CASE("初期値 $06 は FDC と FDD を許可し、HDD とプリンタは許可しない")
    {
        x68k::IoSc iosc = makeRomInitialized();

        iosc.setSource(x68k::IoSc::kDeviceFdd, true);
        CHECK(iosc.hasPendingInterrupt());
        CHECK(iosc.acknowledgeInterrupt() == 0x61);
        iosc.setSource(x68k::IoSc::kDeviceFdd, false);

        // HDD は bit3 が落ちているので通らない。
        iosc.setSource(x68k::IoSc::kDeviceHdd, true);
        CHECK_FALSE(iosc.hasPendingInterrupt());
        iosc.setSource(x68k::IoSc::kDeviceHdd, false);

        // HDD を許可すればベクタ $62 で通る。
        iosc.write(kRegEnable, static_cast<x68k::u8>(kRomEnableInit | x68k::IoSc::kEnableHdd));
        iosc.setSource(x68k::IoSc::kDeviceHdd, true);
        CHECK(iosc.acknowledgeInterrupt() == 0x62);
    }

    // リセット直後は何も通さない。ROM が設定を書く前の割り込みを
    // 配送すると、ベクタ 0 (リセット SSP) へ飛んで暴走する。
    TEST_CASE("リセット直後は割り込みを配送しない")
    {
        x68k::IoSc iosc;
        iosc.reset();

        CHECK(iosc.enableRegister() == 0);
        CHECK(iosc.vectorRegister() == 0);

        iosc.setSource(x68k::IoSc::kDeviceFdc, true);
        CHECK_FALSE(iosc.hasPendingInterrupt());
        CHECK(iosc.acknowledgeInterrupt() == 0);
    }

    // ベクタ未設定のまま許可だけされても配送しない。
    TEST_CASE("ベクタ未設定なら許可されていても配送しない")
    {
        x68k::IoSc iosc;
        iosc.reset();
        iosc.write(kRegEnable, x68k::IoSc::kEnableFdc);
        iosc.setSource(x68k::IoSc::kDeviceFdc, true);

        CHECK(iosc.acknowledgeInterrupt() == 0);
    }

    // レベル割り込みであること。acknowledge しただけでは下がらない。
    //
    // エッジにすると、ハンドラが要因を読む前に線が下がって取りこぼす。
    // 逆に落とす口が無いと二度と下がらずハンドラが無限に呼ばれる。
    TEST_CASE("acknowledge しても線は下がらず、要因が消えて初めて下がる")
    {
        x68k::IoSc iosc = makeRomInitialized();
        iosc.setSource(x68k::IoSc::kDeviceFdc, true);

        CHECK(iosc.acknowledgeInterrupt() == 0x60);
        CHECK(iosc.hasPendingInterrupt());
        CHECK(iosc.acknowledgeInterrupt() == 0x60);

        iosc.setSource(x68k::IoSc::kDeviceFdc, false);
        CHECK_FALSE(iosc.hasPendingInterrupt());
        CHECK(iosc.acknowledgeInterrupt() == 0);
    }

    // 許可を落とせば線が上がっていても配送されないこと。
    TEST_CASE("FDC の許可を落とすと線が上がっていても配送されない")
    {
        x68k::IoSc iosc = makeRomInitialized();
        iosc.setSource(x68k::IoSc::kDeviceFdc, true);
        REQUIRE(iosc.hasPendingInterrupt());

        iosc.write(kRegEnable, static_cast<x68k::u8>(kRomEnableInit & ~x68k::IoSc::kEnableFdc));
        CHECK_FALSE(iosc.hasPendingInterrupt());

        // 戻せば線は上がったままなので再び通る。
        iosc.write(kRegEnable, kRomEnableInit);
        CHECK(iosc.hasPendingInterrupt());
    }

    // $FF9F14 が行うプリンタ割り込みの許可 (BSET #0) がベクタ $63 に
    // 対応すること。bit0 = プリンタという読みの直接の検査。
    TEST_CASE("bit0 はプリンタで、ベクタ $63 に対応する")
    {
        x68k::IoSc iosc = makeRomInitialized();

        // $FF9F14: BSET #0 相当。
        iosc.write(kRegEnable, static_cast<x68k::u8>(iosc.read(kRegEnable) | 0x01u));
        CHECK((iosc.enableRegister() & x68k::IoSc::kEnablePrinter) != 0);

        // プリンタの状態ビット (bit5) は BUSY 信号であって割り込み要因では
        // ないので、setSource では上がらない。
        iosc.setSource(x68k::IoSc::kDevicePrinter, true);
        CHECK_FALSE(iosc.hasPendingInterrupt());
    }

    // 状態ビットは上位 4bit に見えること。ROM は read-modify-write で
    // ここを読むので、見え方が違うと書き戻しで壊れる。
    TEST_CASE("状態は上位 4bit に見え、許可は下位 4bit に見える")
    {
        x68k::IoSc iosc = makeRomInitialized();
        iosc.setSource(x68k::IoSc::kDeviceFdc, true);

        // bit7 (FDC 発生) が立ち、下位は $06 のまま。
        CHECK(iosc.read(kRegEnable) == static_cast<x68k::u8>(0x80u | kRomEnableInit));
    }

    // 状態ビットは CPU から書けないこと。
    //
    // 書けてしまうと、ROM の read-modify-write が読んだ瞬間の状態を固定し、
    // 要因が消えても割り込みが下がらなくなる。
    TEST_CASE("状態ビットは CPU からは書けない")
    {
        x68k::IoSc iosc = makeRomInitialized();

        // 上位を全部立てて書いても、状態は変わらない。
        iosc.write(kRegEnable, 0xFF);
        CHECK_FALSE(iosc.hasPendingInterrupt());
        // 許可だけが入る。
        CHECK(iosc.enableRegister() == 0x0F);
        CHECK(iosc.read(kRegEnable) == 0x0F);
    }

    // 複数同時に上がったら若いデバイス番号が勝つ。ベクタもその並びで出る。
    TEST_CASE("同時に上がったら FDC が最優先される")
    {
        x68k::IoSc iosc = makeRomInitialized();

        iosc.setSource(x68k::IoSc::kDeviceFdd, true);
        iosc.setSource(x68k::IoSc::kDeviceFdc, true);
        CHECK(iosc.acknowledgeInterrupt() == 0x60);

        // FDC が消えれば FDD が出る。
        iosc.setSource(x68k::IoSc::kDeviceFdc, false);
        CHECK(iosc.acknowledgeInterrupt() == 0x61);
    }

    // ベクタレジスタの下位 2bit はデバイス番号なので、ベースとしては
    // 切り落とされること。
    TEST_CASE("ベクタレジスタの下位 2bit はデバイス番号で上書きされる")
    {
        x68k::IoSc iosc;
        iosc.reset();
        // 下位 2bit にゴミを入れても、FDC は必ず $60 になる。
        iosc.write(kRegVector, 0x63);
        iosc.write(kRegEnable, x68k::IoSc::kEnableFdc);
        iosc.setSource(x68k::IoSc::kDeviceFdc, true);

        CHECK(iosc.acknowledgeInterrupt() == 0x60);
    }

    // 偶数アドレスは実機でも応答しない。
    TEST_CASE("偶数アドレスは読み書きともに素通りする")
    {
        x68k::IoSc iosc;
        iosc.reset();

        iosc.write(0x00, 0xFF);
        iosc.write(0x02, 0xFF);
        CHECK(iosc.enableRegister() == 0);
        CHECK(iosc.vectorRegister() == 0);
        CHECK(iosc.read(0x00) == 0);
        CHECK(iosc.read(0x02) == 0);
    }

    // 実装していないデバイス番号で配列外を踏まないこと。
    TEST_CASE("範囲外のデバイス番号は無視される")
    {
        x68k::IoSc iosc;
        iosc.reset();
        iosc.write(kRegVector, kRomVector);
        iosc.write(kRegEnable, 0x0F);

        iosc.setSource(4, true);
        iosc.setSource(200, true);
        CHECK_FALSE(iosc.hasPendingInterrupt());
    }
}
