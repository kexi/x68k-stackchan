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

// 除算 (DIVU/DIVS) の結果とフラグ。
//
// 68000 の除算は「32bit ÷ 16bit → 商 16bit + 余り 16bit」で、商が 16bit に
// 収まらないとオーバーフローになる。オーバーフロー時は商も余りも書き戻さない
// (レジスタは元のまま) ので、呼び出し側は overflow を見て書き戻しを止める。
struct DivResult
{
    u32 quotient;   // 下位 16bit のみ有効
    u32 remainder;  // 下位 16bit のみ有効
    bool overflow;
    bool n;
    bool z;
};

// 符号なし除算 (DIVU) の中身。divisor が 0 でないことは呼び出し側の責任。
//
// 実装は 68000 のマイクロコードと同じ「シフト減算 (筆算)」で組む。
//
// Why not 素直に C++ の / と % で商を出して比較するか:
//   結果だけなら C++ の除算で足りる。ここで筆算をなぞるのは、オーバーフロー時に
//   実機が残す N/Z を出すため。マイクロコードは商がオーバーフローすると
//   ループの途中で抜け、そのときの内部状態が N/Z に残る。C++ の除算では
//   その「途中」が存在しないので、抜けた瞬間の状態を作れない。
//
// アルゴリズム:
//   余りレジスタに被除数の上位 16bit を置き、下位 16bit を 1bit ずつ
//   降ろしながら「引けるなら引いて商のビットを立てる」を 16 回繰り返す。
//   ループに入る前に既に上位 16bit ≥ 除数なら、商の 17bit 目が立つことが
//   確定するのでその場でオーバーフロー (実機もここで早期に抜ける)。
inline DivResult divideUnsigned(u32 dividend, u32 divisor)
{
    const u32 d = divisor & 0xFFFFu;

    DivResult out{};

    // 早期オーバーフロー判定。上位 16bit が除数以上なら商は 17bit 以上になる。
    if ((dividend >> 16) >= d)
    {
        out.overflow = true;
        // オーバーフローで抜けた時点の内部状態が N/Z に出る。実機のベクタでは
        // この経路は必ず N=1, Z=0 (商レジスタの最上位に 1 が立ったまま
        // 抜けるため、Z が立つ余地がない)。
        out.n = true;
        out.z = false;
        return out;
    }

    // 筆算本体。remainder は 17bit 必要になる場面はない (上のガードで
    // 上位 < 除数を保証済み) が、シフトで一時的に溢れるので u32 で持つ。
    u32 remainder = dividend >> 16;
    u32 quotient = 0;
    for (int i = 15; i >= 0; --i)
    {
        remainder = (remainder << 1) | ((dividend >> static_cast<u32>(i)) & 1u);
        if (remainder >= d)
        {
            remainder -= d;
            quotient |= 1u << static_cast<u32>(i);
        }
    }

    out.quotient = quotient & 0xFFFFu;
    out.remainder = remainder & 0xFFFFu;
    out.overflow = false;
    out.n = (out.quotient & 0x8000u) != 0;
    out.z = out.quotient == 0;
    return out;
}

// 符号付き除算 (DIVS) の中身。divisor が 0 でないことは呼び出し側の責任。
//
// 68000 は両オペランドの絶対値を取ってから符号なしの筆算を回し、最後に符号を
// 付け直す。オーバーフロー判定も絶対値どうしで行うため、「負の商は -32768 まで
// 許される」という非対称性がそのまま出る。
inline DivResult divideSigned(u32 dividend, u32 divisor)
{
    const bool dividendNegative = (dividend & 0x80000000u) != 0;
    const bool divisorNegative = (divisor & 0x8000u) != 0;
    const bool quotientNegative = dividendNegative != divisorNegative;

    // 絶対値は符号なしで取る。dividend = 0x80000000 の絶対値は s32 に
    // 収まらないので、s32 のまま negate すると未定義動作になる。
    const u32 absDividend = dividendNegative ? (0u - dividend) : dividend;
    const u32 absDivisor = divisorNegative ? ((0u - divisor) & 0xFFFFu) : (divisor & 0xFFFFu);

    DivResult out{};

    // 早期オーバーフロー判定。符号なしと同じく上位 16bit で見る。
    if ((absDividend >> 16) >= absDivisor)
    {
        out.overflow = true;
        out.n = true;
        out.z = false;
        return out;
    }

    u32 remainder = absDividend >> 16;
    u32 quotient = 0;
    for (int i = 15; i >= 0; --i)
    {
        remainder = (remainder << 1) | ((absDividend >> static_cast<u32>(i)) & 1u);
        if (remainder >= absDivisor)
        {
            remainder -= absDivisor;
            quotient |= 1u << static_cast<u32>(i);
        }
    }

    // 符号付きの範囲に収まるかを、符号を付ける前の絶対値で判定する。
    //
    // Why not 符号を付けてから -32768..32767 で見るか: 絶対値が 0x8000 の
    // ときに商が正か負かで可否が変わる。絶対値のまま見れば
    // 「正の商なら 0x8000 は溢れ、負の商なら 0x8000 (= -32768) は許す」を
    // 分岐ひとつで書ける。符号付きに直してから見ると、-32768 を作る過程で
    // 一度 32768 を経由することになり、判定順を間違えやすい。
    const bool absOverflow = quotientNegative ? (quotient > 0x8000u) : (quotient > 0x7FFFu);
    if (absOverflow)
    {
        out.overflow = true;
        out.n = true;
        out.z = false;
        return out;
    }

    // 余りの符号は被除数に従う (商の符号ではない)。C++ の % と同じ規則。
    const u32 signedQuotient = quotientNegative ? (0u - quotient) : quotient;
    const u32 signedRemainder = dividendNegative ? (0u - remainder) : remainder;

    out.quotient = signedQuotient & 0xFFFFu;
    out.remainder = signedRemainder & 0xFFFFu;
    out.overflow = false;
    out.n = (out.quotient & 0x8000u) != 0;
    out.z = out.quotient == 0;
    return out;
}

}  // namespace alu
}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_ALU_H
