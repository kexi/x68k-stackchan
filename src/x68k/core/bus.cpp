// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "bus.h"

namespace x68k
{
namespace
{

// 68000 のアドレスバスは 24bit。
constexpr u32 kAddrMask = 0x00FFFFFFu;

// I/O 空間の範囲。$E80000-$EBFFFF (CRTC からスプライトまで)。
constexpr u32 kIoBase = 0xE80000u;
constexpr u32 kIoEnd = 0xEC0000u;

}  // namespace

void SystemBus::markTextDirty(u32 offsetInPlane)
{
    // テキスト VRAM は 1 ライン 128 バイト。オフセットから行番号を求め、
    // タイル行 (16 ライン単位) の印を立てる。
    const u32 line = offsetInPlane / kTvramBytesPerLine;
    const u32 tileRow = line / kDirtyTileHeight;
    if (tileRow < kDirtyTileRows)
    {
        textDirty_[tileRow] = true;
    }
}

u8 SystemBus::read8(u32 addr)
{
    const u32 a = addr & kAddrMask;

    // --- メインメモリ (最頻) ---
    if (a < kMainRamSize)
    {
        // リセット直後は IPL-ROM が $000000 に写像されている。
        // IPL-ROM がエリアセットに書き込むまでこの状態が続く。
        if (romAtZero_ && a < kIplromSize && mem_.iplRom != nullptr)
        {
            return mem_.iplRom[a];
        }
        return mem_.mainRam != nullptr ? mem_.mainRam[a] : 0u;
    }

    // --- IPL-ROM ---
    if (a >= kIplromBase)
    {
        return mem_.iplRom != nullptr ? mem_.iplRom[a - kIplromBase] : 0u;
    }

    // --- CGROM ---
    if (a >= kCgromBase && a < kCgromEnd)
    {
        return mem_.cgRom != nullptr ? mem_.cgRom[a - kCgromBase] : 0u;
    }

    // --- SRAM ---
    if (a >= kSramBase && a < kSramEnd)
    {
        return sram_.read8(a - kSramBase);
    }

    // --- テキスト VRAM ---
    if (a >= kTvramBase && a < kTvramEnd)
    {
        return mem_.textVram != nullptr ? mem_.textVram[a - kTvramBase] : 0u;
    }

    // --- I/O ---
    if (a >= kIoBase && a < kIoEnd)
    {
        return io_.ioRead8(a);
    }

    // --- グラフィック VRAM ---
    if (a >= kGvramBase && a < kGvramEnd)
    {
        if (mem_.graphicVram == nullptr)
        {
            return 0u;
        }
        // アドレス空間は 2MB ぶんあるが実 VRAM は 512KB。折り返す。
        return mem_.graphicVram[(a - kGvramBase) & (kTvramSize - 1)];
    }

    // 未実装領域。バスエラーにはせず 0 を返す。
    // IPL-ROM や IOCS は存在しないデバイスも初期化しに来るため、
    // ここでバスエラーにすると起動が進まない。
    return 0u;
}

u16 SystemBus::read16(u32 addr)
{
    const u32 a = addr & kAddrMask;

    // ワード単位でまとめて読める領域は 2 回の read8 を避ける。
    // 命令フェッチが必ずここを通るので効果が大きい。
    if (a < kMainRamSize)
    {
        const u8* base = nullptr;
        if (romAtZero_ && a + 1 < kIplromSize && mem_.iplRom != nullptr)
        {
            base = mem_.iplRom;
        }
        else if (mem_.mainRam != nullptr)
        {
            base = mem_.mainRam;
        }
        if (base != nullptr)
        {
            return static_cast<u16>((base[a] << 8) | base[a + 1]);
        }
        return 0u;
    }

    if (a >= kIplromBase && mem_.iplRom != nullptr)
    {
        const u32 off = a - kIplromBase;
        return static_cast<u16>((mem_.iplRom[off] << 8) | mem_.iplRom[off + 1]);
    }

    if (a >= kIoBase && a < kIoEnd)
    {
        return io_.ioRead16(a);
    }

    return static_cast<u16>((read8(a) << 8) | read8(a + 1));
}

void SystemBus::write8(u32 addr, u8 value)
{
    const u32 a = addr & kAddrMask;

    if (a < kMainRamSize)
    {
        // ROM が写像されている間の書き込みは RAM 側へ行く (ROM は書けない)。
        if (mem_.mainRam != nullptr)
        {
            mem_.mainRam[a] = value;
        }
        return;
    }

    if (a >= kSramBase && a < kSramEnd)
    {
        sram_.write8(a - kSramBase, value);
        return;
    }

    if (a >= kTvramBase && a < kTvramEnd)
    {
        if (mem_.textVram != nullptr)
        {
            const u32 off = a - kTvramBase;
            mem_.textVram[off] = value;
            markTextDirty(off % kTvramPlaneSize);
        }
        return;
    }

    if (a >= kIoBase && a < kIoEnd)
    {
        io_.ioWrite8(a, value);
        return;
    }

    if (a >= kGvramBase && a < kGvramEnd)
    {
        if (mem_.graphicVram != nullptr)
        {
            mem_.graphicVram[(a - kGvramBase) & (kTvramSize - 1)] = value;
        }
        return;
    }

    // ROM 領域への書き込みは黙って捨てる。
}

void SystemBus::write16(u32 addr, u16 value)
{
    const u32 a = addr & kAddrMask;

    if (a < kMainRamSize && mem_.mainRam != nullptr)
    {
        mem_.mainRam[a] = static_cast<u8>(value >> 8);
        mem_.mainRam[a + 1] = static_cast<u8>(value & 0xFFu);
        return;
    }

    if (a >= kIoBase && a < kIoEnd)
    {
        io_.ioWrite16(a, value);
        return;
    }

    write8(a, static_cast<u8>(value >> 8));
    write8(a + 1, static_cast<u8>(value & 0xFFu));
}

}  // namespace x68k
