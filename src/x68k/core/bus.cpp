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

// リセット直後に $000000 へ写像される ROM の位置と大きさ。
//
// 写像元は $FF0000 側 (IPL-ROM 128KB の後半 64KB)。リセットベクタと
// 起動コードはここに置かれており、実行は $FF0010 から始まる。
constexpr u32 kRomAtZeroOffset = 0xFF0000u - kIplromBase;  // ROM 内オフセット
constexpr u32 kRomAtZeroSize = kIplromSize - kRomAtZeroOffset;

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
    faulted_ = false;

    // --- メインメモリ (最頻) ---
    if (a < kMainRamSize)
    {
        // リセット直後は IPL-ROM が $000000 に写像されている。
        // IPL-ROM がエリアセットに書き込むまでこの状態が続く。
        //
        // 写像されるのは ROM の先頭 ($FE0000) ではなく $FF0000 の側。
        // リセットベクタ (SSP と PC) はそこに置かれており、実機の
        // 起動は PC=$FF0010 から始まる。ここを取り違えると
        // ベクタが読めず即座に暴走する。
        if (romAtZero_ && a < kRomAtZeroSize && mem_.iplRom != nullptr)
        {
            return mem_.iplRom[kRomAtZeroOffset + a];
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

    // ここまでのどれにも当たらない領域。
    //
    // X68000 の IPL-ROM は「バスエラーベクタを差し替えてから読みに行き、
    // エラーが起きれば装置が無い」という方法で SCSI ROM ($FC0000) の有無を
    // 調べる ($FF0236)。0 を返してしまうと「ROM がある」ことになり、
    // その先頭を JSR で呼んで暴走する。
    //
    // I/O 空間 ($E80000-$EBFFFF) は上で処理済みなので、ここへ来るのは
    // 本当に何も繋がっていないアドレス。バスエラーにしてよい。
    faulted_ = true;
    return 0u;
}

u16 SystemBus::read16(u32 addr)
{
    const u32 a = addr & kAddrMask;
    faulted_ = false;

    // ワード単位でまとめて読める領域は 2 回の read8 を避ける。
    // 命令フェッチが必ずここを通るので効果が大きい。
    if (a < kMainRamSize)
    {
        if (romAtZero_ && a + 1 < kRomAtZeroSize && mem_.iplRom != nullptr)
        {
            const u8* rom = mem_.iplRom + kRomAtZeroOffset;
            return static_cast<u16>((rom[a] << 8) | rom[a + 1]);
        }
        if (mem_.mainRam != nullptr)
        {
            return static_cast<u16>((mem_.mainRam[a] << 8) | mem_.mainRam[a + 1]);
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

    // 上のどれにも当たらない領域は read8 を 2 回に分ける。
    // read8 が faulted_ を書き換えるので、どちらかが失敗したら
    // ワード全体を失敗として扱う。
    const u8 hi = read8(a);
    const bool hiFaulted = faulted_;
    const u8 lo = read8(a + 1);
    faulted_ = faulted_ || hiFaulted;
    return static_cast<u16>((hi << 8) | lo);
}

void SystemBus::write8(u32 addr, u8 value)
{
    const u32 a = addr & kAddrMask;
    notifyWatch(a, value, 1);

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
    notifyWatch(a, value, 2);

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
