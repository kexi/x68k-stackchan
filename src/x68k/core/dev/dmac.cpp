// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "dmac.h"

namespace x68k
{

void Dmac::reset()
{
    for (auto& channel : channels_)
    {
        channel.fill(0);
    }

    // 繋がっているデバイスはリセットで外さない。SASI の転送バッファや
    // FDC のイメージと同じく「外から与えられた配線」であって、
    // リセットで変わるものではない。
}

void Dmac::setDevice(u32 channel, DmaDevice* device)
{
    if (channel >= kChannelCount)
    {
        return;
    }
    devices_[channel] = device;
}

u8 Dmac::read(u32 offset) const
{
    const u32 channel = offset / kChannelStride;
    const u32 reg = offset % kChannelStride;

    if (channel >= kChannelCount)
    {
        return 0u;
    }
    return channels_[channel][reg];
}

void Dmac::write(u32 offset, u8 value)
{
    const u32 channel = offset / kChannelStride;
    const u32 reg = offset % kChannelStride;

    if (channel >= kChannelCount)
    {
        return;
    }

    if (reg == kRegCsr)
    {
        // CSR は書き込んだビットがクリアされる。IPL-ROM は転送前に
        // $FF を書いて全部落とす (SASI は $FF9944、FDC は $FF8F3C)。
        channels_[channel][kRegCsr] = static_cast<u8>(channels_[channel][kRegCsr] & ~value);
        return;
    }

    channels_[channel][reg] = value;

    // CCR の bit7 で転送が始まる。
    const bool isStartRequested = reg == kRegCcr && (value & kCcrStart) != 0;
    if (isStartRequested)
    {
        runTransfer(channel);
    }
}

void Dmac::runTransfer(u32 channel)
{
    DmaDevice* const device = devices_[channel];
    if (device == nullptr || memory_ == nullptr)
    {
        return;
    }

    auto& regs = channels_[channel];

    u32 addr = (static_cast<u32>(regs[kRegMar]) << 24) |
               (static_cast<u32>(regs[kRegMar + 1]) << 16) |
               (static_cast<u32>(regs[kRegMar + 2]) << 8) | static_cast<u32>(regs[kRegMar + 3]);
    u32 count = (static_cast<u32>(regs[kRegMtc]) << 8) | static_cast<u32>(regs[kRegMtc + 1]);

    // HD63450 の MTC は 0 を 65536 と解釈する (16bit で表せる最大長を
    // 表現するため)。0 を「転送なし」と読むと最大サイズの要求が黙って
    // 何もしないまま完了扱いになる。
    const bool isMaxCount = count == 0;
    if (isMaxCount)
    {
        count = 0x10000;
    }

    const bool isToDevice = (regs[kRegOcr] & kOcrDirectionToMemory) == 0;

    while (count > 0)
    {
        if (isToDevice)
        {
            if (!device->dmaWrite(memory_->dmaMemRead(addr)))
            {
                break;
            }
        }
        else
        {
            u8 value = 0;
            if (!device->dmaRead(&value))
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
    regs[kRegMar] = static_cast<u8>(addr >> 24);
    regs[kRegMar + 1] = static_cast<u8>(addr >> 16);
    regs[kRegMar + 2] = static_cast<u8>(addr >> 8);
    regs[kRegMar + 3] = static_cast<u8>(addr);
    regs[kRegMtc] = static_cast<u8>(count >> 8);
    regs[kRegMtc + 1] = static_cast<u8>(count);

    // 要求量を転送し切れたときだけ完了 (COC) を立てる。IPL-ROM は COC だけを
    // 見て転送の成否を判断するので、途中で止まったのに COC を立てると
    // 転送前のメモリをブートセクタとして扱ってしまう。
    //
    // 「1 バイトでも進んだら成功」にはしない。尻切れのブートコードを
    // 完全なものとして実行するのが、まさに避けたい失敗の形。
    const bool isComplete = count == 0;

    // デバイスへ終わりを伝える。自分では転送長を知らないデバイス (FDC) が
    // 実行フェーズを畳むのに要る。伝えないとメインステータスの CB が
    // 立ったままになり、次のコマンド送出が止まる。
    device->dmaComplete(isComplete);

    if (isComplete)
    {
        regs[kRegCsr] = static_cast<u8>(regs[kRegCsr] | kCsrChannelOperationComplete);
    }
    else
    {
        // 打ち切りはエラーとして残す。CER にも理由を書いておかないと
        // CSR の ERR だけでは後から原因を追えない。
        regs[kRegCsr] = static_cast<u8>(regs[kRegCsr] | kCsrError);
        regs[kRegCer] = kCerBusErrorDevice;
    }
    regs[kRegCsr] = static_cast<u8>(regs[kRegCsr] & ~kCsrChannelActive);
}

}  // namespace x68k
