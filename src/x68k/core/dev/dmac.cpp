// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "dmac.h"

namespace x68k
{

void Dmac::reset()
{
    channel_.fill(0);
    other_.fill(0);
}

u8 Dmac::read(u32 offset) const
{
    // SASI が使うのはチャネル 1 ($E84040-$E8407F)。
    const u32 channel = offset / kChannelStride;
    const u32 reg = offset % kChannelStride;

    if (channel != kSasiChannel)
    {
        return offset < other_.size() ? other_[offset] : 0u;
    }
    return reg < channel_.size() ? channel_[reg] : 0u;
}

void Dmac::write(u32 offset, u8 value)
{
    const u32 channel = offset / kChannelStride;
    const u32 reg = offset % kChannelStride;

    if (channel != kSasiChannel)
    {
        if (offset < other_.size())
        {
            other_[offset] = value;
        }
        return;
    }

    if (reg >= channel_.size())
    {
        return;
    }

    if (reg == kRegCsr)
    {
        // CSR は書き込んだビットがクリアされる。IPL-ROM は転送前に
        // $FF を書いて全部落とす ($FF9944)。
        channel_[kRegCsr] = static_cast<u8>(channel_[kRegCsr] & ~value);
        return;
    }

    channel_[reg] = value;

    // CCR の bit7 で転送が始まる。
    const bool isStartRequested = reg == kRegCcr && (value & kCcrStart) != 0;
    if (isStartRequested)
    {
        runTransfer();
    }
}

void Dmac::runTransfer()
{
    if (device_ == nullptr || memory_ == nullptr)
    {
        return;
    }

    u32 addr = (static_cast<u32>(channel_[kRegMar]) << 24) |
               (static_cast<u32>(channel_[kRegMar + 1]) << 16) |
               (static_cast<u32>(channel_[kRegMar + 2]) << 8) |
               static_cast<u32>(channel_[kRegMar + 3]);
    u32 count =
        (static_cast<u32>(channel_[kRegMtc]) << 8) | static_cast<u32>(channel_[kRegMtc + 1]);

    // HD63450 の MTC は 0 を 65536 と解釈する (16bit で表せる最大長を
    // 表現するため)。0 を「転送なし」と読むと最大サイズの要求が黙って
    // 何もしないまま完了扱いになる。
    const bool isMaxCount = count == 0;
    if (isMaxCount)
    {
        count = 0x10000;
    }

    const bool isToDevice = (channel_[kRegOcr] & kOcrDirectionToMemory) == 0;

    while (count > 0)
    {
        if (isToDevice)
        {
            if (!device_->dmaWrite(memory_->dmaMemRead(addr)))
            {
                break;
            }
        }
        else
        {
            u8 value = 0;
            if (!device_->dmaRead(&value))
            {
                break;
            }
            memory_->dmaMemWrite(addr, value);
        }
        ++addr;
        --count;
    }

    // 進んだぶんをレジスタへ書き戻す。IPL-ROM は残りカウントを見ないが、
    // 実機と同じ状態にしておかないと後から辻褄が合わなくなる。
    channel_[kRegMar] = static_cast<u8>(addr >> 24);
    channel_[kRegMar + 1] = static_cast<u8>(addr >> 16);
    channel_[kRegMar + 2] = static_cast<u8>(addr >> 8);
    channel_[kRegMar + 3] = static_cast<u8>(addr);
    channel_[kRegMtc] = static_cast<u8>(count >> 8);
    channel_[kRegMtc + 1] = static_cast<u8>(count);

    // 要求量を転送し切れたときだけ完了 (COC) を立てる。IPL-ROM は COC だけを
    // 見て転送の成否を判断するので、途中で止まったのに COC を立てると
    // 転送前のメモリをブートセクタとして扱ってしまう。
    //
    // 「1 バイトでも進んだら成功」にはしない。尻切れのブートコードを
    // 完全なものとして実行するのが、まさに避けたい失敗の形。
    const bool isComplete = count == 0;
    if (isComplete)
    {
        channel_[kRegCsr] = static_cast<u8>(channel_[kRegCsr] | kCsrChannelOperationComplete);
    }
    else
    {
        // 打ち切りはエラーとして残す。CER にも理由を書いておかないと
        // CSR の ERR だけでは後から原因を追えない。
        channel_[kRegCsr] = static_cast<u8>(channel_[kRegCsr] | kCsrError);
        channel_[kRegCer] = kCerBusErrorDevice;
    }
    channel_[kRegCsr] = static_cast<u8>(channel_[kRegCsr] & ~kCsrChannelActive);
}

}  // namespace x68k
