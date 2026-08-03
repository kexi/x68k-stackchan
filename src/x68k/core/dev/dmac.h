// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// DMAC (HD63450) ($E84000)。
//
// X68000 の SASI はデータ転送を DMAC 経由で行う。IPL-ROM はブートセクタを
// 読むとき、SASI へ READ コマンドを送った後に DMAC のチャネル 1 を設定して
// 起動し ($FF9944)、転送が終わるのを待つ。DMAC が無いとブートセクタが
// メモリへ届かず、IPL-ROM の "X68K" 検査 ($FF91FA) で必ず失敗する。
//
// 実装範囲: SASI が使うチャネル 1 の、メモリへの単純転送だけ。
// チェイン転送や複数チャネルの調停は実装しない。FDC が使うチャネル 0 は
// 本エミュレータでは FD 起動を使わないので対象外。

#ifndef X68K_CORE_DEV_DMAC_H
#define X68K_CORE_DEV_DMAC_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

// DMAC が転送するデータの出どころ。SASI がこれを実装する。
class DmaDevice
{
public:
    virtual ~DmaDevice() = default;

    // デバイスからメモリへ 1 バイト渡す。転送できるものが無ければ false。
    virtual bool dmaRead(u8* value) = 0;
    // メモリからデバイスへ 1 バイト受け取る。
    virtual bool dmaWrite(u8 value) = 0;
};

// DMAC が読み書きするメモリ空間。バスがこれを実装する。
class DmaMemory
{
public:
    virtual ~DmaMemory() = default;

    virtual u8 dmaMemRead(u32 addr) = 0;
    virtual void dmaMemWrite(u32 addr, u8 value) = 0;
};

class Dmac
{
public:
    // チャネル 1 のレジスタ。先頭からのオフセット。
    static constexpr u32 kChannelStride = 0x40;
    static constexpr u32 kSasiChannel = 1;

    static constexpr u32 kRegCsr = 0x00;  // ステータス
    static constexpr u32 kRegCer = 0x01;  // エラー
    static constexpr u32 kRegDcr = 0x04;  // デバイス制御
    static constexpr u32 kRegOcr = 0x05;  // 動作制御 (bit7 が転送方向)
    static constexpr u32 kRegScr = 0x06;  // シーケンス制御
    static constexpr u32 kRegCcr = 0x07;  // チャネル制御 (bit7 = 起動)
    static constexpr u32 kRegMtc = 0x0A;  // 転送カウント (2B)
    static constexpr u32 kRegMar = 0x0C;  // メモリアドレス (4B)

    // CSR のビット。IPL-ROM は転送完了を待つのに使う。
    static constexpr u8 kCsrChannelOperationComplete = 0x80;  // COC
    static constexpr u8 kCsrChannelActive = 0x08;             // ACT

    // OCR bit7 (DIR): 1 = デバイス → メモリ、0 = メモリ → デバイス。
    //
    // IPL-ROM はブートセクタを読むとき OCR に $B2 を書く ($FF994E)。
    // bit7 が立っているのでこれが「読み出し」の向き。逆に取ると
    // 1 バイトも転送されず、"X68K" の検査で必ず失敗する。
    static constexpr u8 kOcrDirectionToMemory = 0x80;

    // CCR bit7: 転送開始。
    static constexpr u8 kCcrStart = 0x80;

    void reset();

    void setDevice(DmaDevice* device)
    {
        device_ = device;
    }

    void setMemory(DmaMemory* memory)
    {
        memory_ = memory;
    }

    [[nodiscard]] u8 read(u32 offset) const;
    void write(u32 offset, u8 value);

private:
    // 転送を最後まで行う。
    //
    // 実機は 1 バイトずつバスを取り合いながら進むが、本エミュレータでは
    // 起動された時点で一気に転送し切る。IPL-ROM は完了を待つだけなので
    // 差が出ない。バスを止めないぶん、むしろ速い。
    void runTransfer();

    // チャネル 1 のレジスタ実体。他チャネルは値を覚えるだけ。
    std::array<u8, kChannelStride> channel_{};
    std::array<u8, kChannelStride * 4> other_{};

    DmaDevice* device_ = nullptr;
    DmaMemory* memory_ = nullptr;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_DMAC_H
