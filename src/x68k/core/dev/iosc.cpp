// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// I/O 割り込みコントローラ ($E9C000) の実装。
// レジスタ配置とビット割り当ての根拠は iosc.h のコメントに書いた。

#include "iosc.h"

namespace x68k
{

namespace
{

// レジスタは奇数アドレスにだけ現れる。
constexpr u32 kRegEnable = 0x01;  // $E9C001
constexpr u32 kRegVector = 0x03;  // $E9C003

// 扱うデバイス数 (FDC / FDD / HDD / プリンタ)。
constexpr int kDeviceCount = 4;

// $E9C001 の下位 4bit が許可、上位 4bit が状態。
constexpr u8 kEnableMask = 0x0F;
constexpr u8 kStatusMask = 0xF0;

// $E9C003 の下位 2bit はデバイス番号なので、ベースとしては切り落とす。
constexpr u8 kVectorBaseMask = 0xFC;
constexpr u8 kVectorDeviceMask = 0x03;

}  // namespace

u8 IoSc::enableMaskOf(u8 device)
{
    // 並びは bit0=プリンタ / bit1=FDD / bit2=FDC / bit3=HDD。
    // デバイス番号 (0=FDC 1=FDD 2=HDD 3=プリンタ) とは順序が違うので、
    // シフトで済ませずに引き当てる。
    //
    // Why not 1<<device で済ませないか: 一度それで書いて、FDC (デバイス 0) の
    // 許可をプリンタの bit0 で判定していた。ROM の初期値 $06 では bit0 が
    // 落ちているので FDC 割り込みが一切通らず、原因が「配線していない」のか
    // 「ビットが違う」のか切り分けられなくなる。
    switch (device)
    {
        case kDeviceFdc:
            return kEnableFdc;
        case kDeviceFdd:
            return kEnableFdd;
        case kDeviceHdd:
            return kEnableHdd;
        case kDevicePrinter:
            return kEnablePrinter;
        default:
            return 0;
    }
}

u8 IoSc::statusMaskOf(u8 device)
{
    switch (device)
    {
        case kDeviceFdc:
            return kStatusFdc;
        case kDeviceFdd:
            return kStatusFdd;
        case kDeviceHdd:
            return kStatusHdd;
        case kDevicePrinter:
            // プリンタの状態ビットは BUSY 信号で、割り込み要因ではない。
            // ここで返すと BUSY のたびに割り込みが上がってしまう。
            return 0;
        default:
            return 0;
    }
}

void IoSc::reset()
{
    // すべて 0。ROM が $FF0D04 で $06 を書くまでは何も通さない。
    //
    // Why not ベクタを $60 で初期化しないか: リセット直後にそれらしい
    // ベクタ番号を持たせると、ROM がベクタレジスタを書く前に上がった
    // 割り込みが配送されてしまう。実機は不定なので、0 = 未設定として
    // 配送しない方が誤りを早く見つけられる。
    enable_ = 0;
    status_ = 0;
    vector_ = 0;
}

u8 IoSc::read(u32 offset) const
{
    const u32 reg = offset & 0x0Fu;
    if (reg == kRegEnable)
    {
        // 上位が状態、下位が許可。ROM は read-modify-write でここを触るので、
        // 状態ビットもそのまま見せる ($FF81CC の BTST #5 がこれを読む)。
        return static_cast<u8>((status_ & kStatusMask) | (enable_ & kEnableMask));
    }
    if (reg == kRegVector)
    {
        return vector_;
    }
    return 0u;
}

void IoSc::write(u32 offset, u8 value)
{
    const u32 reg = offset & 0x0Fu;
    if (reg == kRegEnable)
    {
        // 書けるのは下位 4bit の許可だけ。上位の状態はデバイスが持つ。
        //
        // Why not 上位も書かせないか: ROM は read-modify-write で
        // 「読んだ値に BSET/BCLR して書き戻す」ので、上位を書けるように
        // すると読んだ瞬間の状態がそのまま固定されてしまう。要因が消えても
        // 状態ビットが残り、割り込みが下がらなくなる。
        enable_ = static_cast<u8>(value & kEnableMask);
        return;
    }
    if (reg == kRegVector)
    {
        vector_ = value;
    }
}

void IoSc::setSource(u8 device, bool asserted)
{
    const u8 mask = statusMaskOf(device);
    if (mask == 0)
    {
        return;
    }

    if (asserted)
    {
        status_ = static_cast<u8>(status_ | mask);
        return;
    }
    status_ = static_cast<u8>(status_ & static_cast<u8>(~mask));
}

int IoSc::pendingDevice() const
{
    for (int i = 0; i < kDeviceCount; ++i)
    {
        const u8 device = static_cast<u8>(i);
        const bool isEnabled = (enable_ & enableMaskOf(device)) != 0;
        const u8 statusBit = statusMaskOf(device);
        const bool isAsserted = statusBit != 0 && (status_ & statusBit) != 0;
        if (isEnabled && isAsserted)
        {
            return i;
        }
    }
    return -1;
}

bool IoSc::hasPendingInterrupt() const
{
    return pendingDevice() >= 0;
}

u8 IoSc::acknowledgeInterrupt() const
{
    const int device = pendingDevice();
    if (device < 0)
    {
        return 0u;
    }

    // ベクタが未設定 (0) のうちは配送しない。ベクタ 0 は 68000 では
    // リセット SSP なので、そこへ飛ばすと原因の分からない暴走になる。
    if (vector_ == 0)
    {
        return 0u;
    }

    // ベクタ番号 = (レジスタ値 & $FC) | デバイス番号。
    // ROM は $60 を書くので、FDC は $60、FDD は $61 になる。
    return static_cast<u8>((vector_ & kVectorBaseMask) |
                           (static_cast<u8>(device) & kVectorDeviceMask));
}

}  // namespace x68k
