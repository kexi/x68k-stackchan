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
// $C2 は X68000 固有。IPL-ROM が最初に発行し ($FF99AC のテンプレート)、
// ドライブのパラメータを設定する。コマンド 6 バイトの後に 10 バイトの
// パラメータが続く ($FF990E で D3=9 の DBRA)。中身は使わないが、
// 受け取り切らないと IPL-ROM が次へ進まない。
constexpr u8 kSasiSpecify = 0xC2;
constexpr u32 kSasiSpecifyParamBytes = 10;

// SASI のセクタ長。X68000 の SASI HDD は 256 バイト/セクタ。
constexpr u32 kSasiSectorSize = 256;

// SASI のフェーズ。
//
// $E96003 の下位 5bit が実機のフェーズを表す。IPL-ROM は AND.B #$1F の後に
// この値と比較して待つ ($FF97BA / $FF9842 / $FF991C)。
// IPL-ROM が待つ値は 2 つだけ。ビットの意味を推測して組み立てるより、
// 実際に比較されている値をそのまま名前にする方が間違えない。
//   $0B コマンド送出フェーズ ($FF9842 / $FF9890 / $FF98BC)
//       — CPU からターゲットへ送る側。コマンド 6 バイトとパラメータ。
//   $07 データインフェーズ ($FF97BE)
//       — ターゲットから CPU へ返す側。READ したセクタはここで渡す。
//   $03 $C2 のパラメータ送出待ち ($FF991C)
//   $0F ステータスフェーズ       ($FF970A で D6=$0F、$FF97E8 で一致を待つ)
//       — 終了ステータス 1 バイトを $E96001 から読む
//   $1F メッセージフェーズ       ($FF971E で D6=$1F)
//       — メッセージ 1 バイトを読んでバスを解放する
//
// Why not $07 を使うか: $FF97BA に $07 を待つ経路があるが、実際に呼ばれるのは
// $FF981E 側で、そちらは $0B を待つ。$07 を返すとコマンドを 1 バイトも
// 受け取れないまま止まる。
// セレクション待ちだけはビット単位で、$E96003 の bit1 が **0** になるのを
// 待つ ($FF96DA の BTST #1 → BEQ)。コマンドフェーズの $07 は bit1 が 1 なので、
// セレクションを受け付けた直後にいきなり $07 を返すと待ちを抜けられない。
// 「セレクション成立」を表す中間状態を挟む。
constexpr u8 kSasiStatusCommand = 0x0B;
constexpr u8 kSasiStatusDataOut = 0x0B;
constexpr u8 kSasiStatusDataIn = 0x07;
constexpr u8 kSasiStatusSpecifyParam = 0x03;
constexpr u8 kSasiStatusStatus = 0x0F;
constexpr u8 kSasiStatusMessage = 0x1F;
constexpr u8 kSasiStatusBusFree = 0x00;

constexpr u8 kPhaseBusFree = 0;
constexpr u8 kPhaseSelected = 6;  // セレクション成立。まだコマンドを受けない
// $C2 のパラメータ待ち。通常のデータアウト ($0B) と違い、IPL-ROM は
// ステータスフェーズと同じ $03 を待ってから送ってくる ($FF9910)。
constexpr u8 kPhaseSpecifyParam = 7;
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
    // SASI のデータ転送は DMAC 経由で行われる。DMAC からはデータの出どころが
    // SASI、転送先がバスに見える。
    dmac_.setDevice(this);
    dmac_.setMemory(this);

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
    fdc_.reset();
    sasi_ = SasiState{};
    dmac_.reset();

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

    // MFP は自分のベクタ番号を返すデバイス (自動ベクタではない)。
    // VR レジスタの上位 4bit と割り込み番号を組み合わせた値になる。
    //
    // ここを自動ベクタ (24+6=30) にすると、IOCS が未初期化ベクタ用に
    // 埋めている「上位バイト = ベクタ番号」の値を PC に読み込んでしまい、
    // 不正ベクタのハンドラへ飛んで「エラーが発生しました」で止まる。
    const u32 vectorNumber = mfp_.acknowledgeInterrupt();
    if (vectorNumber == 0)
    {
        return;
    }
    cpu_.requestInterrupt(kMfpInterruptLevel, vectorNumber);
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

        case kFdcBase:
            // FDC (uPD72065)。ドライブ未接続として振る舞う状態機械。
            //   $E94001 メインステータス
            //   $E94003 データ (コマンド送出と結果の受け取り)
            if ((addr & 0x0Fu) == 0x01)
            {
                return fdc_.readStatus();
            }
            if ((addr & 0x0Fu) == 0x03)
            {
                return fdc_.readData();
            }
            return 0u;

        case kDmacBase:
            return dmac_.read(addr - kDmacBase);

        case kAdpcmBase:
        case kSccBase:
        case kPpiBase:
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

        case kDmacBase:
            dmac_.write(addr - kDmacBase, value);
            return;

        case kFdcBase:
            if ((addr & 0x0Fu) == 0x03)
            {
                fdc_.writeData(value);
            }
            else if ((addr & 0x0Fu) == 0x05)
            {
                // ドライブ制御 (選択とモーター)。
                fdc_.writeDriveControl(value);
            }
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

// --- DMA ---------------------------------------------------------------------
//
// DMAC は「デバイスから 1 バイト取ってメモリへ書く」を繰り返すだけなので、
// SASI 側はデータインフェーズのバッファを 1 バイトずつ差し出せばよい。

bool Machine::dmaRead(u8* value)
{
    if (sasi_.phase != kPhaseDataIn || sasi_.bufferPos >= sasi_.bufferLength)
    {
        return false;
    }
    *value = sasi_.buffer[sasi_.bufferPos++];
    if (sasi_.bufferPos >= sasi_.bufferLength)
    {
        sasi_.phase = kPhaseStatus;
    }
    return true;
}

bool Machine::dmaWrite(u8 value)
{
    if (sasi_.phase != kPhaseDataOut || sasi_.bufferPos >= sasi_.bufferLength)
    {
        return false;
    }
    sasi_.buffer[sasi_.bufferPos++] = value;
    if (sasi_.bufferPos >= sasi_.bufferLength)
    {
        if (sasi_.command[0] == kSasiWrite && disk_ != nullptr && disk_->isPresent())
        {
            const u32 lba = (static_cast<u32>(sasi_.command[1] & 0x1Fu) << 16) |
                            (static_cast<u32>(sasi_.command[2]) << 8) |
                            static_cast<u32>(sasi_.command[3]);
            disk_->writeSector(lba, sasi_.buffer, 1);
        }
        sasi_.phase = kPhaseStatus;
    }
    return true;
}

u8 Machine::dmaMemRead(u32 addr)
{
    return bus_.read8(addr);
}

void Machine::dmaMemWrite(u32 addr, u8 value)
{
    bus_.write8(addr, value);
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
        // データレジスタ。ターゲットから CPU へ渡す側。
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
            // 終了ステータスの次はメッセージ。IPL-ROM は 2 バイト読む。
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
        // ステータスレジスタ。IPL-ROM は下位 5bit をフェーズとして読む。
        switch (sasi_.phase)
        {
            case kPhaseSelected:
                // セレクションが成立した直後。IPL-ROM は bit1 が 0 になるのを
                // 待っているので、ここでは 0 を返してから次の読みでコマンド
                // フェーズへ移る。
                sasi_.phase = kPhaseCommand;
                return kSasiStatusBusFree;

            case kPhaseCommand:
                return kSasiStatusCommand;
            case kPhaseDataIn:
                return kSasiStatusDataIn;

            case kPhaseDataOut:
                return kSasiStatusDataOut;
            case kPhaseStatus:
                return kSasiStatusStatus;

            case kPhaseMessage:
                return kSasiStatusMessage;

            case kPhaseSpecifyParam:
                return kSasiStatusSpecifyParam;
            default:
                return kSasiStatusBusFree;
        }
    }

    return 0u;
}

void Machine::sasiWrite(u32 addr, u8 value)
{
    const u32 reg = addr & 0x0Fu;

    if (reg == 0x07)
    {
        // セレクション。IPL-ROM はここへターゲット ID を書き ($FF96CE)、
        // $E96003 の bit1 (BSY) が 0 になるのを待つ ($FF96DA)。
        //
        // Why not $E96001 でセレクションとするか: 実機の IPL-ROM は
        // $E96007 を使う。$E96001 はデータの授受専用で、セレクション前に
        // 書かれることはない。
        //
        // ディスクが無ければ BSY を立てたまま (バスフリーにしない) にして
        // タイムアウトさせる。
        if (disk_ != nullptr && disk_->isPresent())
        {
            sasi_.phase = kPhaseSelected;
            sasi_.commandLength = 0;
        }
        return;
    }

    if (reg == 0x01)
    {
        // データレジスタへの書き込み。
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
                case kSasiSpecify:
                    // パラメータ 10 バイトを受け取ってから終了ステータスを返す。
                    sasi_.bufferPos = 0;
                    sasi_.bufferLength = kSasiSpecifyParamBytes;
                    sasi_.phase = kPhaseSpecifyParam;
                    return;

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
                    // count = 0 は 1 セクタの意味。IPL-ROM はブートセクタを
                    // 4 セクタまとめて要求するので、一度に読んで載せる。
                    // 1 セクタずつ返すと DMA が 256 バイトで止まる。
                    u32 sectors = count == 0 ? 1u : count;
                    if (sectors > SasiState::kMaxSectorsPerCommand)
                    {
                        sectors = SasiState::kMaxSectorsPerCommand;
                    }
                    const bool ok = disk_ != nullptr && disk_->isPresent() &&
                                    disk_->readSector(lba, sasi_.buffer, sectors);
                    if (!ok)
                    {
                        sasi_.status = 0x02;
                        sasi_.phase = kPhaseStatus;
                        return;
                    }
                    sasi_.bufferLength = kSasiSectorSize * sectors;
                    sasi_.phase = kPhaseDataIn;
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

        if (sasi_.phase == kPhaseDataOut || sasi_.phase == kPhaseSpecifyParam)
        {
            if (sasi_.bufferPos < sizeof(sasi_.buffer))
            {
                sasi_.buffer[sasi_.bufferPos++] = value;
            }
            if (sasi_.bufferPos >= sasi_.bufferLength)
            {
                // WRITE ならディスクへ書く。$C2 のパラメータは捨てる。
                if (sasi_.command[0] == kSasiWrite && disk_ != nullptr && disk_->isPresent())
                {
                    const u32 lba = (static_cast<u32>(sasi_.command[1] & 0x1Fu) << 16) |
                                    (static_cast<u32>(sasi_.command[2]) << 8) |
                                    static_cast<u32>(sasi_.command[3]);
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
