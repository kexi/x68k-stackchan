// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// X68000 のアドレスデコード。CPU からのアクセスを RAM / ROM / VRAM / I/O へ振り分ける。
//
// 性能について:
//   ここは 68000 が命令をフェッチするたびに通る、エミュレータで最も実行回数の多い
//   経路になる。判定の順序は実行頻度順 (RAM → ROM → VRAM → I/O) にしてある。
//   ESP32 では bus_read16 の分岐が丸ごとホットパスなので、将来 IRAM 配置の
//   第一候補になる。
//
// メモリの所有:
//   RAM や VRAM の実体はここが持たず、外から与えられたポインタを指すだけにする。
//   ESP32 では PSRAM の断片化を避けるため起動直後に一括確保する必要があり、
//   確保の主導権を platform 層に渡しておきたいため。

#ifndef X68K_CORE_BUS_H
#define X68K_CORE_BUS_H

#include <cstdint>

#include "cpu/m68k_types.h"
#include "dev/sram.h"
#include "memmap.h"

namespace x68k
{

// I/O デバイスへのアクセスを受け取る口。
//
// Machine がこれを実装して各デバイスへ振り分ける。バスがデバイスの詳細を
// 知らずに済むので、デバイスを足してもバスのコードは変わらない。
class IoHandler
{
public:
    virtual ~IoHandler() = default;

    virtual u8 ioRead8(u32 addr) = 0;
    virtual void ioWrite8(u32 addr, u8 value) = 0;
    virtual u16 ioRead16(u32 addr) = 0;
    virtual void ioWrite16(u32 addr, u16 value) = 0;
};

// メモリ領域の実体。platform 層が確保して渡す。
struct MemoryMap
{
    u8* mainRam = nullptr;       // kMainRamSize バイト
    u8* textVram = nullptr;      // kTvramSize バイト (4 プレーン連続)
    u8* graphicVram = nullptr;   // 任意。無ければ読み出しは 0
    const u8* iplRom = nullptr;  // kIplromSize バイト。必須
    const u8* cgRom = nullptr;   // 任意。無ければ読み出しは 0
};

class SystemBus final : public Bus
{
public:
    SystemBus(MemoryMap memory, Sram& sram, IoHandler& io) : mem_(memory), sram_(sram), io_(io) {}

    // メモリ領域を差し替える。
    //
    // Why 再代入ではなくこれを使うか: SystemBus は Sram と IoHandler を参照で
    // 持つため代入演算子が消える。実体の確保は platform 層の責務にしてあるので
    // (ESP32 では PSRAM の断片化を避けるため起動直後に一括確保したい)、
    // 後からポインタだけ差し替えられる必要がある。
    void setMemory(const MemoryMap& memory)
    {
        mem_ = memory;
    }

    u8 read8(u32 addr) override;
    u16 read16(u32 addr) override;
    void write8(u32 addr, u8 value) override;
    void write16(u32 addr, u16 value) override;

    // テキスト VRAM への書き込みがあった矩形を追跡する。
    //
    // 画面全体を毎フレーム転送すると SPI 接続の LCD では間に合わないので、
    // 変化した部分だけを送る。書き込み時にタイル単位で印を付けておき、
    // 描画側がそれを見て転送範囲を決める。
    static constexpr u32 kDirtyTileHeight = 16;
    static constexpr u32 kDirtyTileRows = 1024 / kDirtyTileHeight;

    [[nodiscard]] bool isTextRowDirty(u32 tileRow) const
    {
        return tileRow < kDirtyTileRows && textDirty_[tileRow];
    }

    void clearTextDirty()
    {
        for (auto& d : textDirty_)
        {
            d = false;
        }
    }

    [[nodiscard]] bool anyTextDirty() const
    {
        for (const auto d : textDirty_)
        {
            if (d)
            {
                return true;
            }
        }
        return false;
    }

    // 起動直後に IPL-ROM を $000000 に見せるかどうか。
    //
    // X68000 はリセット直後、ベクタを読むために ROM が $000000 へ写像される。
    // IPL-ROM がエリアセット ($E86001) に書き込むと通常の RAM 配置へ切り替わる。
    // これを再現しないとリセットベクタが読めない。
    void setRomMappedAtZero(bool mapped)
    {
        romAtZero_ = mapped;
    }

    [[nodiscard]] bool romMappedAtZero() const
    {
        return romAtZero_;
    }

private:
    void markTextDirty(u32 offsetInPlane);

    MemoryMap mem_;
    Sram& sram_;
    IoHandler& io_;
    bool romAtZero_ = true;
    bool textDirty_[kDirtyTileRows] = {};
};

}  // namespace x68k

#endif  // X68K_CORE_BUS_H
