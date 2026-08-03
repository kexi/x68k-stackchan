// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "fdc.h"

namespace x68k
{
namespace
{

// コマンドコード (1 バイト目の下位 5bit)。
// 上位 3bit は MT/MF/SK の修飾ビットなので落として判定する。
constexpr u8 kCmdSpecify = 0x03;
constexpr u8 kCmdSenseDriveStatus = 0x04;
constexpr u8 kCmdRecalibrate = 0x07;
constexpr u8 kCmdSenseInterruptStatus = 0x08;
constexpr u8 kCmdSeek = 0x0F;
constexpr u8 kCmdReadId = 0x0A;
constexpr u8 kCmdWriteId = 0x0D;
constexpr u8 kCmdReadTrack = 0x02;
constexpr u8 kCmdWriteData = 0x05;
constexpr u8 kCmdReadData = 0x06;
constexpr u8 kCmdWriteDeleted = 0x09;
constexpr u8 kCmdReadDeleted = 0x0C;

// 結果ステータス ST0 のビット。
// 終了コードは bit7-6 の 2bit で、00 正常終了 / 01 異常終了 / 11 無効なコマンド。
// 無効なコマンドは片方だけでなく両方立てる。bit7 だけだと
// 「ドライブのレディ状態が変化した」(10) の意味になってしまう。
constexpr u8 kSt0AbnormalTermination = 0x40;
constexpr u8 kSt0InvalidCommand = 0xC0;
// bit4 = 装置チェック (ドライブが応答しない)。
constexpr u8 kSt0EquipmentCheck = 0x10;
// bit5 = シーク終了。
constexpr u8 kSt0SeekEnd = 0x20;

// ST3 のビット。SENSE DRIVE STATUS が返す。
// bit5 = レディ。ドライブ未接続なので立てない。
constexpr u8 kSt3Ready = 0x20;

}  // namespace

void Fdc::reset()
{
    phase_ = Phase::Command;
    command_.fill(0);
    commandLength_ = 0;
    commandExpected_ = 0;
    result_.fill(0);
    resultLength_ = 0;
    resultPos_ = 0;
    interruptPending_ = false;
    selectedDrive_ = 0;
}

u32 Fdc::parameterCount(u8 commandByte)
{
    switch (commandByte & 0x1Fu)
    {
        case kCmdSpecify:
            return 2;
        case kCmdSenseDriveStatus:
        case kCmdRecalibrate:
        case kCmdReadId:
            return 1;
        case kCmdSenseInterruptStatus:
            return 0;
        case kCmdSeek:
            return 2;
        case kCmdWriteId:
            return 5;
        case kCmdReadTrack:
        case kCmdWriteData:
        case kCmdReadData:
        case kCmdWriteDeleted:
        case kCmdReadDeleted:
            return 8;
        default:
            // 未知のコマンド。パラメータ無しとして扱い、
            // 「無効なコマンド」の結果を返す。
            return 0;
    }
}

u8 Fdc::readStatus() const
{
    switch (phase_)
    {
        case Phase::Command:
            // コマンドを受け付けられる状態。
            //
            // IPL-ROM はこの順で待つ ($FF904C-$FF9062):
            //   CB (bit4) が 0 → RQM (bit7) が 1 → DIO (bit6) が 0
            // コマンドの途中 (パラメータ待ち) は CB を立てる。
            if (commandLength_ > 0)
            {
                return static_cast<u8>(kStatusRqm | kStatusCb);
            }
            return kStatusRqm;

        case Phase::Execute:
            // 実行中。本エミュレータでは即座に結果へ移るのでここには来ない。
            return kStatusCb;

        case Phase::Result:
            // 結果を返す状態。IPL-ROM は RQM|DIO|CB ($D0) が揃うのを待つ
            // ($FF89DE)。
            return static_cast<u8>(kStatusRqm | kStatusDio | kStatusCb);
    }
    return kStatusRqm;
}

u8 Fdc::readData()
{
    if (phase_ != Phase::Result || resultPos_ >= resultLength_)
    {
        return 0u;
    }

    const u8 value = result_[resultPos_++];
    if (resultPos_ >= resultLength_)
    {
        // 全部返したのでコマンド待ちへ戻る。
        // ここで戻さないと IPL-ROM が次のコマンドを送れない。
        phase_ = Phase::Command;
        commandLength_ = 0;
        resultLength_ = 0;
        resultPos_ = 0;
    }
    return value;
}

void Fdc::writeData(u8 value)
{
    if (phase_ == Phase::Result)
    {
        // 結果を読み切らないまま次のコマンドが来た。溜まった結果を捨てて
        // 受け付ける。
        //
        // IPL-ROM は DMA を使うコマンド (READ/WRITE DATA) の結果フェーズを
        // インラインでは読まず、FDC 割り込みハンドラ ($FF1130) に任せている。
        // 本エミュレータは FDC の割り込み線 (実機では IRQ レベル 1 の
        // オートベクタで、MFP は通さない) を配線していないので、結果は
        // 誰にも読まれずに残る。残したまま無視すると次のコマンド送出
        // ($FF9036) が CB の落ちるのを永久に待つ。CB を無条件に落とす手も
        // あるが、それだと SENSE DRIVE STATUS のように IPL-ROM が
        // インラインで結果を読むコマンド ($FF89DE) が読めなくなるので、
        // 「次のコマンドが来た時点で捨てる」形にした。
        phase_ = Phase::Command;
        commandLength_ = 0;
        resultLength_ = 0;
        resultPos_ = 0;
    }

    if (phase_ != Phase::Command)
    {
        return;  // 実行中の書き込みは無視する
    }

    if (commandLength_ == 0)
    {
        // コマンドの 1 バイト目。
        command_[0] = value;
        commandLength_ = 1;
        commandExpected_ = 1 + parameterCount(value);
    }
    else if (commandLength_ < command_.size())
    {
        command_[commandLength_++] = value;
    }

    if (commandLength_ >= commandExpected_)
    {
        executeCommand();
    }
}

void Fdc::writeDriveControl(u8 value)
{
    // $E94005: bit1-0 がドライブ選択、bit7 がモーター。
    // ドライブ未接続なので選択番号だけ覚えておく。
    selectedDrive_ = static_cast<u8>(value & 0x03u);
}

void Fdc::executeCommand()
{
    const u8 opcode = static_cast<u8>(command_[0] & 0x1Fu);
    resultPos_ = 0;

    switch (opcode)
    {
        case kCmdSpecify:
            // タイミングパラメータの設定。結果を返さずコマンド待ちへ戻る。
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;

        case kCmdSenseInterruptStatus:
            // 直前の割り込みの原因を返す。ST0 と現在のシリンダ番号。
            //
            // 割り込みが無いのに呼ばれたら「無効なコマンド」を返す。
            // ここで正常終了を返し続けると IPL-ROM が
            // 「まだ処理中の割り込みがある」と判断してループする。
            if (interruptPending_)
            {
                result_[0] = static_cast<u8>(kSt0SeekEnd | kSt0AbnormalTermination |
                                             kSt0EquipmentCheck | selectedDrive_);
                result_[1] = 0;  // シリンダ番号
                resultLength_ = 2;
                interruptPending_ = false;
            }
            else
            {
                result_[0] = kSt0InvalidCommand;
                resultLength_ = 1;
            }
            break;

        case kCmdSenseDriveStatus:
            // ドライブの状態。レディを立てないことで未接続を表す。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            result_[0] = selectedDrive_;  // ST3。kSt3Ready は立てない
            resultLength_ = 1;
            static_assert(kSt3Ready == 0x20, "ST3 のレディビットは bit5");
            break;

        case kCmdRecalibrate:
        case kCmdSeek:
            // シーク系。ドライブが無いので割り込みだけ上げ、
            // SENSE INTERRUPT STATUS で異常終了を返す。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            interruptPending_ = true;
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;

        case kCmdReadId:
        case kCmdReadData:
        case kCmdWriteData:
        case kCmdReadTrack:
        case kCmdReadDeleted:
        case kCmdWriteDeleted:
        case kCmdWriteId:
            // 読み書き系。ドライブ未接続なので実行フェーズに入らず、
            // 結果フェーズも持たずにコマンド待ちへ戻る。
            //
            // 実機ならここは ST0/ST1/ST2 + C/H/R/N の 7 バイトを結果として
            // 返す。IPL-ROM はそれを FDC 割り込みハンドラ ($FF1130) で
            // 読み出し、ドライブごとの状態表 $C90 へ積む。本エミュレータは
            // FDC の割り込み線を配線していないため、結果を積むと誰も読まず、
            // 次のコマンド送出 ($FF9036 の CB 待ち) がそこで止まる。
            // ROM 側は割り込み待ちではなく $FF9014 → $FF9006 で
            // 「メインステータスの下位 5bit が 0 になる」ことを完了条件に
            // しているので、結果を持たずに即アイドルへ戻すのが
            // ドライブ未接続の表現として最も近い。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            interruptPending_ = true;
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;

        default:
            // 未知のコマンド。
            result_[0] = kSt0InvalidCommand;
            resultLength_ = 1;
            break;
    }

    phase_ = Phase::Result;
    commandLength_ = 0;
}

}  // namespace x68k
