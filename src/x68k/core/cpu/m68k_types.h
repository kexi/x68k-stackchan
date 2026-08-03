// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MC68000 の実行状態と、CPU がメモリへ届くための口。

#ifndef X68K_CORE_CPU_M68K_TYPES_H
#define X68K_CORE_CPU_M68K_TYPES_H

#include <cstdint>

namespace x68k
{

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// ステータスレジスタのビット。
// 下位バイト (CCR) はユーザモードでも読み書きでき、上位バイトは特権。
namespace sr_bit
{
inline constexpr u16 kCarry = 0x0001;
inline constexpr u16 kOverflow = 0x0002;
inline constexpr u16 kZero = 0x0004;
inline constexpr u16 kNegative = 0x0008;
inline constexpr u16 kExtend = 0x0010;
inline constexpr u16 kCcrMask = 0x001F;
inline constexpr u16 kIntMask = 0x0700;  // 割り込みマスクレベル (I2-I0)
inline constexpr u16 kSupervisor = 0x2000;
inline constexpr u16 kTrace = 0x8000;
// 68000 が実際に保持できるビット。未定義ビットは常に 0 として振る舞う。
inline constexpr u16 kImplemented = kCcrMask | kIntMask | kSupervisor | kTrace;
}  // namespace sr_bit

// SR のビットを落とすためのマスク。
//
// `sr & ~sr_bit::kZero` と書くと ~ が int に昇格し、u16 へ戻すときに
// -Wsign-conversion が出る。毎回 static_cast で囲うとフラグ操作が読めなくなるので、
// 補数を u16 の範囲で閉じたヘルパーにする。
inline constexpr u16 clearMask(u16 bits)
{
    return static_cast<u16>(~static_cast<unsigned>(bits) & 0xFFFFu);
}

// 例外ベクタ番号。番号 × 4 がベクタテーブルのオフセットになる。
namespace vector
{
inline constexpr u32 kResetSsp = 0;
inline constexpr u32 kResetPc = 1;
inline constexpr u32 kBusError = 2;
inline constexpr u32 kAddressError = 3;
inline constexpr u32 kIllegalInstruction = 4;
inline constexpr u32 kZeroDivide = 5;
inline constexpr u32 kChk = 6;
inline constexpr u32 kTrapv = 7;
inline constexpr u32 kPrivilegeViolation = 8;
inline constexpr u32 kTrace = 9;
inline constexpr u32 kLineA = 10;  // A-line emulator
inline constexpr u32 kLineF = 11;  // F-line emulator
inline constexpr u32 kUninitializedInterrupt = 15;
inline constexpr u32 kSpuriousInterrupt = 24;
inline constexpr u32 kAutoVectorBase = 24;  // レベル n の自動ベクタは 24+n
inline constexpr u32 kTrapBase = 32;        // TRAP #n は 32+n
}  // namespace vector

// CPU からメモリ空間へアクセスするための口。
//
// エミュレータ本体 (バスとデバイス) はこれを実装する。テストベクタの検証では
// 単純な連想配列を実装として渡す。この間接化があるおかげで、CPU コアだけを
// バスやデバイスから切り離して検証できる。
//
// 単位がワードなのは 68000 のバスが 16bit だから。バイトアクセスは
// UDS/LDS のどちらを立てるかで表現され、read8/write8 がそれを担う。
class Bus
{
public:
    virtual ~Bus() = default;

    virtual u16 read16(u32 addr) = 0;
    virtual void write16(u32 addr, u16 value) = 0;
    virtual u8 read8(u32 addr) = 0;
    virtual void write8(u32 addr, u8 value) = 0;
};

// 68000 の実行状態。
//
// レジスタの持ち方について:
//   A7 (スタックポインタ) は特権状態によって USP と SSP が切り替わる。
//   毎回どちらを見るか分岐すると命令実装が汚れるので、a[7] を「現在有効な A7」
//   として扱い、S ビットが変わる瞬間に usp/ssp と入れ替える方式にする
//   (setSr が担当する)。
struct M68kState
{
    u32 d[8] = {};
    u32 a[8] = {};  // a[7] は現在のスタックポインタ

    u32 usp = 0;  // 非特権時のスタックポインタの控え
    u32 ssp = 0;  // 特権時のスタックポインタの控え

    u32 pc = 0;
    u16 sr = 0;

    // プリフェッチキュー。68000 は実行中の命令の先を 2 ワードまで読んでいる。
    // テストベクタが初期状態・最終状態としてこれを持つので、エミュレータ側も
    // 同じ形で保持しないと突き合わせられない。
    //   irc: 直近に読んだワード (次に ir へ流れる)
    //   ir : 現在デコード中の命令語
    u16 ir = 0;
    u16 irc = 0;

    // 停止状態。STOP 命令で立ち、割り込みで解除される。
    bool stopped = false;
    // 未実装命令や二重バスエラーで停止したことを示す。実機には無い、
    // エミュレータの開発用フラグ。
    bool halted = false;

    // 実行したサイクル数の累計。デバイスを進める量の基準に使う。
    u64 cycles = 0;

    [[nodiscard]] bool isSupervisor() const
    {
        return (sr & sr_bit::kSupervisor) != 0;
    }

    [[nodiscard]] u32 interruptMask() const
    {
        return static_cast<u32>((sr & sr_bit::kIntMask) >> 8);
    }
};

}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_TYPES_H
