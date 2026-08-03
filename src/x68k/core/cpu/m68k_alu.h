// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 算術演算とフラグ計算の共通部分。命令実装ファイルから使う。
//
// フラグ計算を 1 箇所にまとめているのは、68000 の桁溢れ判定が
// 「符号ビットの組み合わせ」という間違えやすい規則で決まるため。
// 各命令で書き下すと必ずどこかで取り違える。

#ifndef X68K_CORE_CPU_M68K_ALU_H
#define X68K_CORE_CPU_M68K_ALU_H

#include "m68k_types.h"

namespace x68k
{
namespace alu
{

inline constexpr u32 kByte = 1;
inline constexpr u32 kWord = 2;
inline constexpr u32 kLong = 4;

inline constexpr u32 truncate(u32 value, u32 size)
{
    if (size == kByte)
    {
        return value & 0xFFu;
    }
    if (size == kWord)
    {
        return value & 0xFFFFu;
    }
    return value;
}

inline constexpr u32 signBit(u32 size)
{
    return 1u << (size * 8 - 1);
}

inline constexpr bool isNegative(u32 value, u32 size)
{
    return (value & signBit(size)) != 0;
}

// 加算の結果とフラグ。
//
// V (オーバーフロー): 同符号どうしを足して符号が変わったとき。
// C (キャリー): 符号なしとして桁が溢れたとき。
struct Result
{
    u32 value;
    bool n;
    bool z;
    bool v;
    bool c;
};

inline Result add(u32 dst, u32 src, u32 size)
{
    const u32 d = truncate(dst, size);
    const u32 s = truncate(src, size);
    const u32 r = truncate(d + s, size);

    const bool sm = isNegative(s, size);
    const bool dm = isNegative(d, size);
    const bool rm = isNegative(r, size);

    Result out{};
    out.value = r;
    out.n = rm;
    out.z = r == 0;
    out.v = (sm == dm) && (rm != dm);
    // キャリーは切り詰める前の和がサイズの表現範囲を超えたかで見る。
    const u64 wide = static_cast<u64>(d) + static_cast<u64>(s);
    out.c = wide > static_cast<u64>(truncate(0xFFFFFFFFu, size));
    return out;
}

inline Result sub(u32 dst, u32 src, u32 size)
{
    const u32 d = truncate(dst, size);
    const u32 s = truncate(src, size);
    const u32 r = truncate(d - s, size);

    const bool sm = isNegative(s, size);
    const bool dm = isNegative(d, size);
    const bool rm = isNegative(r, size);

    Result out{};
    out.value = r;
    out.n = rm;
    out.z = r == 0;
    // 異符号どうしを引いて、結果が引かれる数と違う符号になったとき。
    out.v = (sm != dm) && (rm != dm);
    out.c = s > d;
    return out;
}

// 拡張加算 (ADDX/SUBX/ABCD/SBCD) の Z フラグは特殊。
//
// 結果がゼロでも Z をクリアしない (前の状態を保つ) という累積動作をする。
// 多倍長演算で「全部のワードがゼロのときだけ Z が立つ」を実現するため。
// ここを普通の代入にすると多倍長比較が壊れる。
inline bool accumulateZero(bool previousZ, bool resultIsZero)
{
    return previousZ && resultIsZero;
}

// BCD 加算 (ABCD) の結果とフラグ。dst/src はパック 10 進のバイト。
//
// 「2 進で足してから 10 進補正する」方式を採る。下位ニブルが 9 を超えるか
// 半桁繰り上がりが出たら +6、上位も同じなら +0x60 を足す。
//
// Why not: 各ニブルを 10 進数に戻して足し直す実装のほうが読みやすいが、
// それだと N と V が出てこない。この 2 つは 68000 では「未定義」ながら
// 実機は補正途中の 2 進値から決まる値を返しており、テストベクタ
// (MAME のマイクロコード由来) もその値を期待する。補正前後の中間値が
// 要るので、実機と同じ手順をなぞる。
inline Result bcdAdd(u32 dst, u32 src, bool extend)
{
    const u32 d = dst & 0xFFu;
    const u32 s = src & 0xFFu;
    const u32 x = extend ? 1u : 0u;

    // 補正前の 2 進和。上位への繰り上がりを見たいので 8bit で切らない。
    const u32 binary = d + s + x;

    u32 corrected = binary;
    const bool lowNeedsCarry = ((d ^ s ^ binary) & 0x10u) != 0 || (binary & 0x0Fu) > 9u;
    if (lowNeedsCarry)
    {
        corrected += 6u;
    }
    // 上位ニブルの判定は下位補正後の値で行う。下位から桁が上がるため。
    const bool highNeedsCarry = corrected > 0x9Fu;
    if (highNeedsCarry)
    {
        corrected += 0x60u;
    }

    Result out{};
    out.value = corrected & 0xFFu;
    out.c = highNeedsCarry;
    // N は補正後の値のビット 7。V は「補正で符号ビットが 0→1 に変わったか」。
    // どちらも仕様上は未定義だが、実機はこの値を返す。
    out.n = (corrected & 0x80u) != 0;
    out.v = ((~binary & corrected) & 0x80u) != 0;
    out.z = out.value == 0;
    return out;
}

// BCD 減算 (SBCD/NBCD) の結果とフラグ。dst - src - X を 10 進で行う。
//
// 加算と対称に、2 進で引いてから借りが出たニブルを -6 / -0x60 で補正する。
inline Result bcdSub(u32 dst, u32 src, bool extend)
{
    const u32 d = dst & 0xFFu;
    const u32 s = src & 0xFFu;
    const u32 x = extend ? 1u : 0u;

    const u32 binary = d - s - x;

    u32 corrected = binary;
    // 下位ニブルで借りが出たかは、被減数・減数・結果の bit4 の食い違いで判る。
    const bool lowBorrowed = ((d ^ s ^ binary) & 0x10u) != 0;
    if (lowBorrowed)
    {
        corrected -= 6u;
    }
    // 上位桁の補正は 2 進減算そのものが借りたときだけ行う。
    const bool binaryBorrowed = (binary & 0x100u) != 0;
    if (binaryBorrowed)
    {
        corrected -= 0x60u;
    }

    // C は「補正まで含めて桁が落ちたか」で決まり、値の補正条件とは一致しない。
    //
    // Why not: C を binaryBorrowed で代用すると C0 - BD - 1 のような
    // 「2 進では借りないが下位の -6 で bit8 まで借りが伝わる」ケースを落とす。
    // 逆に corrected 側で -0x60 の要否まで判定すると、今度は値が 0x60 ずれる。
    // 実機は値と C を別の信号で作っているので、ここでも分けて持つ。
    const bool borrowedOut = binaryBorrowed || (corrected & 0x100u) != 0;

    Result out{};
    out.value = corrected & 0xFFu;
    out.c = borrowedOut;
    // 加算と同じく N/V は未定義。V は符号ビットが 1→0 に落ちたかで決まる。
    out.n = (corrected & 0x80u) != 0;
    out.v = ((binary & ~corrected) & 0x80u) != 0;
    out.z = out.value == 0;
    return out;
}

}  // namespace alu
}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_ALU_H
