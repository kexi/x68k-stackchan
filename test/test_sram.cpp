// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: SRAM の工場出荷状態が、IPL-ROM の期待する形になっていること。
//
// IPL-ROM は起動時にマジックを検査し、メモリ容量と起動デバイスの設定を読む。
// ここが 1 バイトでも違うと起動シーケンスが先へ進まないが、症状は
// 「何も起きずに固まる」なので原因が非常に追いにくい。先に押さえておく。

#include "dev/sram.h"
#include "doctest.h"

TEST_CASE("工場出荷状態のマジックが正しい")
{
    x68k::Sram sram;
    CHECK(sram.hasValidMagic());

    // "X68000" + $00 + $57 の 8 バイト。
    CHECK(sram.read8(0) == 'X');
    CHECK(sram.read8(1) == '6');
    CHECK(sram.read8(2) == '8');
    CHECK(sram.read8(3) == '0');
    CHECK(sram.read8(4) == '0');
    CHECK(sram.read8(5) == '0');
    CHECK(sram.read8(7) == 0x57);
}

TEST_CASE("メモリ容量がバスの実装と一致する")
{
    x68k::Sram sram;
    const std::uint32_t ramSize =
        (static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize)) << 24) |
        (static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize + 1)) << 16) |
        (static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize + 2)) << 8) |
        static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize + 3));

    // ここが実際のバスの実装容量とずれると、Human68k が存在しない領域を
    // 使おうとして壊れる。
    CHECK(ramSize == x68k::kMainRamSize);
}

TEST_CASE("起動デバイスが SASI 優先に設定される")
{
    x68k::Sram sram;
    // 標準優先順位 ($0000) だと FD から探し始めるが、本エミュレータの FDC は
    // ドライブ未接続を表すスタブでしかなく、IPL-ROM のポーリングループが
    // タイムアウトを持たないため抜けられない。SASI を最優先にして回避する。
    CHECK(sram.read16(x68k::Sram::kOffsetBootDevice) == x68k::Sram::kBootDeviceSasi0);
}

TEST_CASE("起動時画面モードが 16 である")
{
    x68k::Sram sram;
    CHECK(sram.read8(x68k::Sram::kOffsetScreenMode) == 16);
}

TEST_CASE("書き込みが読み戻せて、dirty が立つ")
{
    x68k::Sram sram;
    CHECK_FALSE(sram.isDirty());

    sram.write8(0x100, 0xAB);
    CHECK(sram.read8(0x100) == 0xAB);
    CHECK(sram.isDirty());

    sram.clearDirty();
    CHECK_FALSE(sram.isDirty());
}

TEST_CASE("範囲外アクセスで壊れない")
{
    x68k::Sram sram;
    // 16KB を超えるオフセットは無視される。ここで落ちるとエミュレータ全体が
    // 道連れになるので、黙って捨てる方が安全。
    sram.write8(x68k::kSramSize, 0xFF);
    sram.write8(x68k::kSramSize + 1000, 0xFF);
    CHECK(sram.read8(x68k::kSramSize) == 0);
    CHECK(sram.hasValidMagic());
}

TEST_CASE("マジックが壊れたら検出できる")
{
    x68k::Sram sram;
    sram.write8(0, 0x00);
    CHECK_FALSE(sram.hasValidMagic());

    // 初期化し直せば復旧する。
    sram.formatDefaults();
    CHECK(sram.hasValidMagic());
}
