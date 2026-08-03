// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "machine.h"

#include <cstring>

namespace x68k
{
namespace
{

// SASI のコマンド。IPL-ROM がブートセクタを読むのに使う範囲だけ実装する。
constexpr u8 kSasiTestUnitReady = 0x00;
constexpr u8 kSasiRezeroUnit = 0x01;
constexpr u8 kSasiRequestSense = 0x03;
constexpr u8 kSasiRead = 0x08;
constexpr u8 kSasiWrite = 0x0A;
constexpr u8 kSasiSeek = 0x0B;

// SASI のセクタ長。X68000 の SASI HDD は 256 バイト/セクタ。
constexpr u32 kSasiSectorSize = 256;

// SASI のフェーズ。
constexpr u8 kPhaseBusFree = 0;
constexpr u8 kPhaseCommand = 1;
constexpr u8 kPhaseDataIn = 2;
constexpr u8 kPhaseDataOut = 3;
constexpr u8 kPhaseStatus = 4;
constexpr u8 kPhaseMessage = 5;

// MFP の割り込みレベル。X68000 では MFP がレベル 6 に繋がっている。
constexpr u32 kMfpInterruptLevel = 6;

}  // namespace

Machine::Machine() : bus_(MemoryMap{}, sram_, *this), cpu_(bus_)
{
    // X68000 では RESET 命令で $000000 の ROM 写像が解除される。
    // 68000 自身は RESET 信号を出すだけなので、機種固有のこの反応は
    // Machine が受け取って処理する。
    cpu_.setResetCallback(
        [](void* context)
        {
            auto* self = static_cast<Machine*>(context);
            self->bus_.setRomMappedAtZero(false);
        },
        this);
}

void Machine::setMemory(const MemoryMap& memory)
{
    bus_.setMemory(memory);
}

void Machine::reset()
{
    sram_.formatDefaults();
    crtc_.reset();
    video_.reset();
    mfp_.reset();
    rtc_.reset();
    sasi_ = SasiState{};

    // リセット直後は IPL-ROM が $000000 に写像されている。
    // これがないとリセットベクタが読めない。
    bus_.setRomMappedAtZero(true);
    bus_.clearTextDirty();

    cpu_.reset();
}

u32 Machine::step()
{
    serviceInterrupts();

    const u32 cycles = cpu_.step();
    if (cycles == 0)
    {
        return 0;
    }

    mfp_.tick(cycles);
    rtc_.tick(cycles);
    if (crtc_.tick(cycles))
    {
        mfp_.setVerticalBlank(crtc_.inVerticalBlank());
    }

    return cycles;
}

u32 Machine::run(u32 cycles)
{
    u32 spent = 0;
    while (spent < cycles)
    {
        const u32 used = step();
        if (used == 0)
        {
            break;  // 停止した
        }
        spent += used;
    }
    return spent;
}

void Machine::serviceInterrupts()
{
    if (!mfp_.hasPendingInterrupt())
    {
        return;
    }
    // MFP はベクタ番号を自分で返す (自動ベクタではない)。
    // ここでは CPU にレベルだけ伝え、ベクタは MFP から取る。
    cpu_.requestInterrupt(kMfpInterruptLevel);
}

void Machine::pressKey(u8 scanCode)
{
    mfp_.receiveKeyboardByte(scanCode);
}

// --- I/O ディスパッチ --------------------------------------------------------

u8 Machine::ioRead8(u32 addr)
{
    const u32 base = addr & 0xFFE000u;

    switch (base)
    {
        case kCrtcBase:
            // CRTC はワード単位。バイトアクセスは上下を切り出す。
            {
                const u32 reg = (addr - kCrtcBase) / 2;
                const u16 value = crtc_.read(reg);
                return static_cast<u8>((addr & 1) != 0 ? (value & 0xFFu) : (value >> 8));
            }

        case kVideoCtrlBase:
        {
            const u16 value = video_.read(addr - kVideoCtrlBase);
            return static_cast<u8>((addr & 1) != 0 ? (value & 0xFFu) : (value >> 8));
        }

        case kMfpBase:
            // MFP のレジスタは奇数アドレスにのみ現れる。
            return mfp_.read((addr - kMfpBase) / 2);

        case kSasiBase:
            return sasiRead(addr);

        case kAreaSetBase:
            // エリアセットは書き込み専用。読んでも意味のある値は返さない。
            //
            // 重要: IPL-ROM 1.3 以降は CLR.B でここへ書き込む。68000 の CLR は
            // read-modify-write なので必ず読み出しが先に起きる。ここで副作用を
            // 持たせると起動しない。
            return 0u;

        case kRtcBase:
            // RTC (RP5C15)。レジスタは 4bit 幅で 2 バイトおきに並ぶ。
            // Human68k は起動時に日付を読むので、妥当な値を返す必要がある。
            return rtc_.read((addr - kRtcBase) / 2);

        case kSysPortBase:
            // システムポート。コントラストや CPU 種別。
            //
            // $E8E00B の bit3-0 が CPU 種別で、$DC が 68000 を表す
            // (上位ニブルは常に $D)。ここを間違えると IOCS が 68030 向けの
            // 初期化をしようとして失敗する。
            if ((addr & 0x0Fu) == 0x0B)
            {
                return 0xDCu;
            }
            return 0u;

        case kOpmBase:
            // YM2151。ステータスレジスタは bit7 = BUSY。
            // 常に「準備完了」(bit7 = 0) を返さないと IOCS の初期化ループが
            // 終わらない。
            return 0u;

        case kAdpcmBase:
        case kSccBase:
        case kPpiBase:
        case kDmacBase:
        case kFdcBase:
        case kIoScBase:
        case kPrinterBase:
            // スタブ。読み出しは 0。
            return 0u;

        default:
            return 0u;
    }
}

void Machine::ioWrite8(u32 addr, u8 value)
{
    const u32 base = addr & 0xFFE000u;

    switch (base)
    {
        case kCrtcBase:
        {
            const u32 reg = (addr - kCrtcBase) / 2;
            const u16 old = crtc_.read(reg);
            const u16 next = (addr & 1) != 0 ? static_cast<u16>((old & 0xFF00u) | value)
                                             : static_cast<u16>((old & 0x00FFu) | (value << 8));
            crtc_.write(reg, next);
            return;
        }

        case kVideoCtrlBase:
        {
            const u32 offset = addr - kVideoCtrlBase;
            const u16 old = video_.read(offset);
            const u16 next = (addr & 1) != 0 ? static_cast<u16>((old & 0xFF00u) | value)
                                             : static_cast<u16>((old & 0x00FFu) | (value << 8));
            video_.write(offset, next);
            return;
        }

        case kMfpBase:
            mfp_.write((addr - kMfpBase) / 2, value);
            return;

        case kRtcBase:
            rtc_.write((addr - kRtcBase) / 2, value);
            return;

        case kSasiBase:
            sasiWrite(addr, value);
            return;

        case kAreaSetBase:
            // エリアセットへの書き込みで ROM の $000000 写像が解除される。
            // これで通常のメモリ配置になり、以降 $000000 は RAM を指す。
            bus_.setRomMappedAtZero(false);
            return;

        default:
            // その他のデバイスへの書き込みは捨てる。
            // IPL-ROM と IOCS は存在しないデバイスも初期化しに来るので、
            // ここでエラーにすると起動が進まない。
            return;
    }
}

u16 Machine::ioRead16(u32 addr)
{
    const u32 base = addr & 0xFFE000u;

    if (base == kCrtcBase)
    {
        return crtc_.read((addr - kCrtcBase) / 2);
    }
    if (base == kVideoCtrlBase)
    {
        return video_.read(addr - kVideoCtrlBase);
    }

    return static_cast<u16>((ioRead8(addr) << 8) | ioRead8(addr + 1));
}

void Machine::ioWrite16(u32 addr, u16 value)
{
    const u32 base = addr & 0xFFE000u;

    if (base == kCrtcBase)
    {
        crtc_.write((addr - kCrtcBase) / 2, value);
        return;
    }
    if (base == kVideoCtrlBase)
    {
        video_.write(addr - kVideoCtrlBase, value);
        return;
    }

    ioWrite8(addr, static_cast<u8>(value >> 8));
    ioWrite8(addr + 1, static_cast<u8>(value & 0xFFu));
}

// --- SASI --------------------------------------------------------------------
//
// X68000 の SASI インタフェースは $E96000 から数バイトのレジスタを持つ。
//   $E96001: データレジスタ (コマンドの送出とデータの授受)
//   $E96003: ステータスレジスタ (ビジー/リクエスト等)
// IPL-ROM はここへ 6 バイトのコマンドを送り、ブートセクタを読み出す。

u8 Machine::sasiRead(u32 addr)
{
    const u32 reg = addr & 0x0Fu;

    if (reg == 0x01)
    {
        // データレジスタ。
        if (sasi_.phase == kPhaseDataIn && sasi_.bufferPos < sasi_.bufferLength)
        {
            const u8 value = sasi_.buffer[sasi_.bufferPos++];
            if (sasi_.bufferPos >= sasi_.bufferLength)
            {
                sasi_.phase = kPhaseStatus;
            }
            return value;
        }
        if (sasi_.phase == kPhaseStatus)
        {
            sasi_.phase = kPhaseMessage;
            return sasi_.status;
        }
        if (sasi_.phase == kPhaseMessage)
        {
            sasi_.phase = kPhaseBusFree;
            return 0u;
        }
        return 0u;
    }

    if (reg == 0x03)
    {
        // ステータスレジスタ。
        //   bit0 (BSY): バスが使用中
        //   bit1 (REQ): データの授受を要求している
        //   bit2 (MSG): メッセージフェーズ
        //   bit3 (C/D): 1 = コマンド/ステータス、0 = データ
        //   bit4 (I/O): 1 = ターゲット → イニシエータ
        u8 status = 0;
        switch (sasi_.phase)
        {
            case kPhaseCommand:
                status = 0x0B;  // BSY|REQ|C/D
                break;
            case kPhaseDataIn:
                status = 0x13;  // BSY|REQ|I/O
                break;
            case kPhaseDataOut:
                status = 0x03;  // BSY|REQ
                break;
            case kPhaseStatus:
                status = 0x1B;  // BSY|REQ|C/D|I/O
                break;
            case kPhaseMessage:
                status = 0x1F;  // BSY|REQ|MSG|C/D|I/O
                break;
            default:
                status = 0x00;  // バスフリー
                break;
        }
        return status;
    }

    return 0u;
}

void Machine::sasiWrite(u32 addr, u8 value)
{
    const u32 reg = addr & 0x0Fu;

    if (reg == 0x01)
    {
        // データレジスタへの書き込み。
        if (sasi_.phase == kPhaseBusFree)
        {
            // セレクション。ターゲット ID が書かれる。
            sasi_.phase = kPhaseCommand;
            sasi_.commandLength = 0;
            return;
        }

        if (sasi_.phase == kPhaseCommand)
        {
            if (sasi_.commandLength < sizeof(sasi_.command))
            {
                sasi_.command[sasi_.commandLength++] = value;
            }
            if (sasi_.commandLength < 6)
            {
                return;
            }

            // 6 バイト揃ったのでコマンドを実行する。
            const u8 opcode = sasi_.command[0];
            // LBA は command[1] の下位 5bit と command[2], command[3]。
            const u32 lba = (static_cast<u32>(sasi_.command[1] & 0x1Fu) << 16) |
                            (static_cast<u32>(sasi_.command[2]) << 8) |
                            static_cast<u32>(sasi_.command[3]);
            const u32 count = sasi_.command[4];

            sasi_.status = 0;
            sasi_.bufferPos = 0;
            sasi_.bufferLength = 0;

            switch (opcode)
            {
                case kSasiTestUnitReady:
                case kSasiRezeroUnit:
                case kSasiSeek:
                    // ディスクが無ければエラーを返す。
                    sasi_.status = (disk_ != nullptr && disk_->isPresent()) ? 0x00 : 0x02;
                    sasi_.phase = kPhaseStatus;
                    return;

                case kSasiRequestSense:
                    // センスデータ 4 バイト。エラーなしを返す。
                    std::memset(sasi_.buffer, 0, 4);
                    sasi_.bufferLength = 4;
                    sasi_.phase = kPhaseDataIn;
                    return;

                case kSasiRead:
                {
                    const u32 sectors = count == 0 ? 1u : count;
                    // バッファは 1 セクタぶんしかないので 1 セクタずつ返す。
                    // IPL-ROM は 1024 バイト (4 セクタ) を読むが、
                    // セクタごとにコマンドを発行する。
                    const bool ok = disk_ != nullptr && disk_->isPresent() &&
                                    disk_->readSector(lba, sasi_.buffer, 1);
                    if (!ok)
                    {
                        sasi_.status = 0x02;
                        sasi_.phase = kPhaseStatus;
                        return;
                    }
                    sasi_.bufferLength = kSasiSectorSize;
                    sasi_.phase = kPhaseDataIn;
                    (void)sectors;
                    return;
                }

                case kSasiWrite:
                    sasi_.bufferLength = kSasiSectorSize;
                    sasi_.phase = kPhaseDataOut;
                    return;

                default:
                    // 未対応コマンド。エラーを返す。
                    sasi_.status = 0x02;
                    sasi_.phase = kPhaseStatus;
                    return;
            }
        }

        if (sasi_.phase == kPhaseDataOut)
        {
            if (sasi_.bufferPos < sizeof(sasi_.buffer))
            {
                sasi_.buffer[sasi_.bufferPos++] = value;
            }
            if (sasi_.bufferPos >= sasi_.bufferLength)
            {
                const u32 lba = (static_cast<u32>(sasi_.command[1] & 0x1Fu) << 16) |
                                (static_cast<u32>(sasi_.command[2]) << 8) |
                                static_cast<u32>(sasi_.command[3]);
                if (disk_ != nullptr && disk_->isPresent())
                {
                    disk_->writeSector(lba, sasi_.buffer, 1);
                }
                sasi_.phase = kPhaseStatus;
            }
            return;
        }
        return;
    }

    if (reg == 0x05)
    {
        // 割り込み許可。
        sasi_.interruptEnabled = (value & 1u) != 0;
        return;
    }
}

}  // namespace x68k
