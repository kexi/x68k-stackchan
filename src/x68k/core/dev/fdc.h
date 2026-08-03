// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// FDC (NEC uPD72065) ($E94000)。
//
// 本エミュレータの起動デバイスは SASI だが、IPL-ROM は起動デバイスの設定に
// 関わらず FDC を初期化しに来る。そのポーリングループ (CB 待ち / RQM 待ち /
// 結果待ち) はいずれもタイムアウトを持たないため、正しく応答しないと
// そこで永久に止まる。
//
// 実装範囲: コマンド・実行・結果の 3 フェーズを持つ状態機械と、
// ドライブ未接続を示す結果ステータス。実際の読み書きは行わない。
// FD からの起動に対応するときは executeCommand を埋める。

#ifndef X68K_CORE_DEV_FDC_H
#define X68K_CORE_DEV_FDC_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Fdc
{
public:
    // メインステータスレジスタ ($E94001) のビット。
    static constexpr u8 kStatusRqm = 0x80;     // データ転送要求
    static constexpr u8 kStatusDio = 0x40;     // 転送方向 (1 = FDC → CPU)
    static constexpr u8 kStatusNdm = 0x20;     // 非 DMA モード
    static constexpr u8 kStatusCb = 0x10;      // コマンド実行中
    static constexpr u8 kStatusDrive0 = 0x01;  // ドライブ 0 がシーク中

    void reset();

    // $E94001 (メインステータス) を読む。
    [[nodiscard]] u8 readStatus() const;

    // $E94003 (データレジスタ) を読む。結果フェーズで結果バイトを返す。
    u8 readData();

    // $E94003 へ書く。コマンドとそのパラメータ。
    void writeData(u8 value);

    // $E94005 (ドライブ制御) へ書く。モーター制御など。
    void writeDriveControl(u8 value);

    // 割り込みが上がっているか。IPL-ROM は SENSE INTERRUPT STATUS で
    // これを確認する。
    [[nodiscard]] bool hasInterrupt() const
    {
        return interruptPending_;
    }

private:
    enum class Phase
    {
        Command,  // コマンドとパラメータを受け取る
        Execute,  // 実行中 (本エミュレータでは即座に終わる)
        Result,   // 結果バイトを返す
    };

    // コマンドに続くパラメータのバイト数を返す。
    [[nodiscard]] static u32 parameterCount(u8 commandByte);

    // コマンドが揃ったときに呼ばれる。結果バイトを組み立てる。
    void executeCommand();

    Phase phase_ = Phase::Command;

    // 受け取ったコマンドとパラメータ。最長は WRITE ID の 9 バイト。
    std::array<u8, 9> command_{};
    u32 commandLength_ = 0;
    u32 commandExpected_ = 0;

    // 返す結果バイト。最長は READ/WRITE 系の 7 バイト。
    std::array<u8, 7> result_{};
    u32 resultLength_ = 0;
    u32 resultPos_ = 0;

    bool interruptPending_ = false;
    // 直近に選択されたドライブ番号。結果ステータスに載せる。
    u8 selectedDrive_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_FDC_H
