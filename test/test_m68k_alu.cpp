// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: DIVU/DIVS の筆算 (シフト減算) が、商・余り・N/Z/V を
// 68000 実機と同じ規則で返すこと。
//
// 適合性ベクタ (test_m68k_vectors.cpp) は全体を機械的に突き合わせるが、
// 落ちたときに「除算のどの規則が壊れたか」までは示さない。ここでは規則を
// 一つずつ名前を付けて固定し、退行したときに原因が直接読めるようにする。

#include "doctest.h"
#include "m68k_alu.h"

using x68k::alu::divideSigned;
using x68k::alu::divideUnsigned;

TEST_SUITE_BEGIN("m68k-alu-div");

TEST_CASE("DIVU は 32bit ÷ 16bit の商と余りを返す")
{
    const auto r = divideUnsigned(100000u, 7u);
    CHECK(r.overflow == false);
    CHECK(r.quotient == 14285u);
    CHECK(r.remainder == 5u);
}

TEST_CASE("DIVU は割り切れるとき余りが 0 になる")
{
    // 0x0002FFFF / 3 = 0xFFFF 余り 0。商が 16bit の上限ちょうどで割り切れる。
    const auto r = divideUnsigned(0x0002FFFDu, 3u);
    CHECK(r.overflow == false);
    CHECK(r.quotient == 0xFFFFu);
    CHECK(r.remainder == 0u);
}

TEST_CASE("DIVU は商 0 のとき Z を立てる")
{
    const auto r = divideUnsigned(5u, 10u);
    CHECK(r.overflow == false);
    CHECK(r.quotient == 0u);
    CHECK(r.remainder == 5u);
    CHECK(r.z == true);
    CHECK(r.n == false);
}

TEST_CASE("DIVU は商の bit15 で N を立てる")
{
    // 商 0x8000 は符号なしでも N が立つ (N は結果の bit15 をそのまま見る)。
    const auto r = divideUnsigned(0x8000u * 2u, 2u);
    CHECK(r.overflow == false);
    CHECK(r.quotient == 0x8000u);
    CHECK(r.n == true);
    CHECK(r.z == false);
}

TEST_CASE("DIVU は商 0xFFFF までは収まり 0x10000 で溢れる")
{
    // 境界のちょうど内側。
    const auto inside = divideUnsigned(0x0000FFFFu, 1u);
    CHECK(inside.overflow == false);
    CHECK(inside.quotient == 0xFFFFu);
    CHECK(inside.remainder == 0u);

    // ちょうど外側。商 0x10000 は 16bit に収まらない。
    const auto outside = divideUnsigned(0x00010000u, 1u);
    CHECK(outside.overflow == true);
}

TEST_CASE("DIVU はオーバーフロー時に N=1 / Z=0 を返す")
{
    // 実機はオーバーフローで筆算を途中で打ち切り、その内部状態を N/Z に残す。
    // 適合性ベクタ (MAME のマイクロコード由来) では、この経路は例外なく
    // N=1, Z=0 になる。Motorola の資料上は「未定義」だが値は決まっている。
    const auto r = divideUnsigned(0xFFFFFFFFu, 1u);
    CHECK(r.overflow == true);
    CHECK(r.n == true);
    CHECK(r.z == false);
}

TEST_CASE("DIVU は上位ワードが除数以上なら必ず溢れる")
{
    // 上位 16bit ≥ 除数 は、商の 17bit 目が立つことと同値。
    const auto r = divideUnsigned(0x00050000u, 5u);
    CHECK(r.overflow == true);

    // 上位が除数未満なら、下位がいくら大きくても収まる。
    const auto ok = divideUnsigned(0x0004FFFFu, 5u);
    CHECK(ok.overflow == false);
}

TEST_CASE("DIVS は符号の組み合わせで商の符号を決める")
{
    // -100 / 7 = -14 余り -2 (余りの符号は被除数に従う)。
    const auto r = divideSigned(static_cast<x68k::u32>(-100), static_cast<x68k::u32>(7) & 0xFFFFu);
    CHECK(r.overflow == false);
    CHECK(static_cast<x68k::s16>(r.quotient) == -14);
    CHECK(static_cast<x68k::s16>(r.remainder) == -2);
}

TEST_CASE("DIVS の余りの符号は被除数に従い除数には従わない")
{
    // 100 / -7 = -14 余り +2。被除数が正なので余りも正。
    const auto r = divideSigned(100u, static_cast<x68k::u32>(-7) & 0xFFFFu);
    CHECK(r.overflow == false);
    CHECK(static_cast<x68k::s16>(r.quotient) == -14);
    CHECK(static_cast<x68k::s16>(r.remainder) == 2);
}

TEST_CASE("DIVS の商の範囲は正と負で非対称")
{
    // 正の商は 32767 まで。32768 は溢れる。
    const auto posOk = divideSigned(32767u, 1u);
    CHECK(posOk.overflow == false);
    CHECK(static_cast<x68k::s16>(posOk.quotient) == 32767);

    const auto posNg = divideSigned(32768u, 1u);
    CHECK(posNg.overflow == true);

    // 負の商は -32768 まで許される。絶対値では同じ 0x8000 なのに可否が違う。
    const auto negOk = divideSigned(static_cast<x68k::u32>(-32768), 1u);
    CHECK(negOk.overflow == false);
    CHECK(static_cast<x68k::s16>(negOk.quotient) == -32768);

    const auto negNg = divideSigned(static_cast<x68k::u32>(-32769), 1u);
    CHECK(negNg.overflow == true);
}

TEST_CASE("DIVS は 0x80000000 / -1 で未定義動作に落ちず溢れを返す")
{
    // s32 のまま negate すると符号付きオーバーフロー (C++ の未定義動作) に
    // なる組み合わせ。絶対値を符号なしで取るので落ちない。
    const auto r = divideSigned(0x80000000u, 0xFFFFu);
    CHECK(r.overflow == true);
    CHECK(r.n == true);
    CHECK(r.z == false);
}

TEST_CASE("DIVS はオーバーフロー時に N=1 / Z=0 を返す")
{
    // 早期判定 (上位ワード ≥ 除数の絶対値) で抜ける経路。
    const auto early = divideSigned(0x7FFFFFFFu, 1u);
    CHECK(early.overflow == true);
    CHECK(early.n == true);
    CHECK(early.z == false);

    // 筆算を回しきってから商の範囲で溢れる経路。どちらも同じ N/Z になる。
    const auto late = divideSigned(0x00010000u, 2u);
    CHECK(late.overflow == true);
    CHECK(late.n == true);
    CHECK(late.z == false);
}

TEST_CASE("DIVS は商 0 のとき Z を立てる")
{
    const auto r = divideSigned(5u, 10u);
    CHECK(r.overflow == false);
    CHECK(r.quotient == 0u);
    CHECK(r.z == true);
    CHECK(r.n == false);
}

TEST_CASE("DIVS の N は商の bit15 ではなく符号付きの負を表す")
{
    // 商 -1 は 0xFFFF なので bit15 が立ち N=1。
    const auto r = divideSigned(static_cast<x68k::u32>(-10), 10u);
    CHECK(r.overflow == false);
    CHECK(static_cast<x68k::s16>(r.quotient) == -1);
    CHECK(r.n == true);
    CHECK(r.z == false);
}

TEST_SUITE_END();
