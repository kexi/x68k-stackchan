// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "mfp.h"

namespace x68k
{
namespace
{

// タイマ制御レジスタの下位 3bit が分周比を表す。
// 0 は停止、1-7 が それぞれ 4/10/16/50/64/100/200 分周。
constexpr u32 kPrescaleTable[8] = {0, 4, 10, 16, 50, 64, 100, 200};

// X68000 の MFP のクロックは 4MHz。CPU は 10MHz なので、
// CPU サイクルを MFP サイクルへ換算する必要がある。
// 分数のままだと毎回割り算が入るので、CPU サイクルを 2 で割って近似する
// (4/10 ≒ 1/2.5 だが、タイマ精度は Human68k の起動には影響しない)。
constexpr u32 kCpuToMfpShift = 1;

// GPIP4 (垂直帰線) のビット位置。
constexpr u8 kGpipVDisp = 0x10;

}  // namespace

void Mfp::reset()
{
    reg_.fill(0);
    prescaleCounter_.fill(0);
    timerValue_.fill(0);
    // GPIP は入力がすべて H の状態で始まる。
    reg_[kGpip] = 0xFF;
}

u8 Mfp::read(u32 regIndex) const
{
    if (regIndex >= kRegCount)
    {
        return 0u;
    }
    return reg_[regIndex];
}

void Mfp::write(u32 regIndex, u8 value)
{
    if (regIndex >= kRegCount)
    {
        return;
    }

    switch (regIndex)
    {
        case kIpra:
        case kIprb:
        case kIsra:
        case kIsrb:
            // 保留/サービス中レジスタは「書いたビットを 0 にする」という
            // 特殊な動作をする (1 を書いても立たない)。割り込みの取り下げに使う。
            reg_[regIndex] &= value;
            return;

        case kTadr:
            reg_[kTadr] = value;
            timerValue_[0] = value;
            return;
        case kTbdr:
            reg_[kTbdr] = value;
            timerValue_[1] = value;
            return;
        case kTcdr:
            reg_[kTcdr] = value;
            timerValue_[2] = value;
            return;
        case kTddr:
            reg_[kTddr] = value;
            timerValue_[3] = value;
            return;

        case kGpip:
            // GPIP は入力なので書き込みは DDR で出力に設定されたビットのみ有効。
            // X68000 では実質入力専用なので無視する。
            return;

        default:
            reg_[regIndex] = value;
            return;
    }
}

u32 Mfp::timerPrescale(u8 control) const
{
    return kPrescaleTable[control & 7u];
}

void Mfp::raise(bool groupA, u8 bit)
{
    const u32 ierIndex = groupA ? kIera : kIerb;
    const u32 iprIndex = groupA ? kIpra : kIprb;

    // 許可されていない割り込みは保留にもならない。
    if ((reg_[ierIndex] & bit) == 0)
    {
        return;
    }
    reg_[iprIndex] |= bit;
}

void Mfp::tickTimer(int index, u8 control, u32 cycles)
{
    const u32 prescale = timerPrescale(control);
    if (prescale == 0)
    {
        return;  // 停止中
    }

    prescaleCounter_[static_cast<std::size_t>(index)] += cycles;
    while (prescaleCounter_[static_cast<std::size_t>(index)] >= prescale)
    {
        prescaleCounter_[static_cast<std::size_t>(index)] -= prescale;

        u8& value = timerValue_[static_cast<std::size_t>(index)];
        if (value == 0)
        {
            // 0 からのデクリメントは 256 として扱う (データレジスタの 0 は 256)。
            value = 0xFF;
        }
        else
        {
            --value;
        }

        if (value != 0)
        {
            continue;
        }

        // タイムアウト。データレジスタの値でリロードして割り込みを上げる。
        static constexpr u32 kDataReg[4] = {kTadr, kTbdr, kTcdr, kTddr};
        value = reg_[kDataReg[static_cast<std::size_t>(index)]];

        switch (index)
        {
            case 0:
                raise(true, kIntTimerA);
                break;
            case 1:
                raise(true, kIntTimerB);
                break;
            case 2:
                raise(false, kIntTimerC);
                break;
            default:
                raise(false, kIntTimerD);
                break;
        }
    }
}

void Mfp::tick(u32 cycles)
{
    const u32 mfpCycles = cycles >> kCpuToMfpShift;
    if (mfpCycles == 0)
    {
        return;
    }

    tickTimer(0, reg_[kTacr], mfpCycles);
    tickTimer(1, reg_[kTbcr], mfpCycles);
    // タイマ C は TCDCR の上位 3bit、タイマ D は下位 3bit。
    tickTimer(2, static_cast<u8>((reg_[kTcdcr] >> 4) & 7u), mfpCycles);
    tickTimer(3, static_cast<u8>(reg_[kTcdcr] & 7u), mfpCycles);
}

void Mfp::setVerticalBlank(bool active)
{
    const u8 before = reg_[kGpip];
    if (active)
    {
        // 垂直帰線中は GPIP4 が L になる。
        reg_[kGpip] = static_cast<u8>(before & ~kGpipVDisp);
    }
    else
    {
        reg_[kGpip] = static_cast<u8>(before | kGpipVDisp);
    }

    if (reg_[kGpip] == before)
    {
        return;
    }

    // AER で指定されたエッジのときだけ割り込みを上げる。
    const bool risingEdgeWanted = (reg_[kAer] & kGpipVDisp) != 0;
    const bool isRising = (reg_[kGpip] & kGpipVDisp) != 0;
    if (isRising == risingEdgeWanted)
    {
        raise(false, kIntGpip4);
    }
}

void Mfp::receiveKeyboardByte(u8 value)
{
    reg_[kUdr] = value;
    // 受信バッファフル。
    reg_[kRsr] |= 0x80;
    raise(true, kIntRecvFull);
}

bool Mfp::hasPendingInterrupt() const
{
    const u8 pendingA = static_cast<u8>(reg_[kIpra] & reg_[kImra]);
    const u8 pendingB = static_cast<u8>(reg_[kIprb] & reg_[kImrb]);
    return (pendingA | pendingB) != 0;
}

u32 Mfp::acknowledgeInterrupt()
{
    // 優先度はグループ A の bit7 が最上位、グループ B の bit0 が最下位。
    // ベクタ番号は VR の上位 4bit + 割り込み番号 (0-15)。
    const u8 pendingA = static_cast<u8>(reg_[kIpra] & reg_[kImra]);
    const u8 pendingB = static_cast<u8>(reg_[kIprb] & reg_[kImrb]);

    for (int bit = 7; bit >= 0; --bit)
    {
        const u8 mask = static_cast<u8>(1u << bit);
        if ((pendingA & mask) != 0)
        {
            reg_[kIpra] = static_cast<u8>(reg_[kIpra] & ~mask);
            reg_[kIsra] |= mask;
            const u32 number = static_cast<u32>(bit) + 8u;
            return (static_cast<u32>(reg_[kVr] & 0xF0u)) | number;
        }
    }
    for (int bit = 7; bit >= 0; --bit)
    {
        const u8 mask = static_cast<u8>(1u << bit);
        if ((pendingB & mask) != 0)
        {
            reg_[kIprb] = static_cast<u8>(reg_[kIprb] & ~mask);
            reg_[kIsrb] |= mask;
            const u32 number = static_cast<u32>(bit);
            return (static_cast<u32>(reg_[kVr] & 0xF0u)) | number;
        }
    }

    return 0u;
}

}  // namespace x68k
