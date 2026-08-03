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

    // 完了を知らせる。IPL-ROM はこのビットが立つのを待つ。
    channel_[kRegCsr] = static_cast<u8>(channel_[kRegCsr] | kCsrChannelOperationComplete);
    channel_[kRegCsr] = static_cast<u8>(channel_[kRegCsr] & ~kCsrChannelActive);
}

}  // namespace x68k
