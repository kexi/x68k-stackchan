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

}  // namespace alu
}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_ALU_H
