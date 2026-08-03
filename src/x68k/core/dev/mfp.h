// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MC68901 MFP ($E88000)。
//
// X68000 の割り込みとタイマの中枢。Human68k のシステムタイマ、垂直帰線割り込み、
// キーボード受信がすべてここを通るため、これが動かないと起動しても操作できない。
//
// 実装範囲: レジスタの読み書き、タイマ A-D、割り込みコントローラ (IERA/B,
// IPRA/B, ISRA/B, IMRA/B)、GPIP (垂直帰線の状態)、シリアル受信 (キーボード)。
// 実機の非同期な細部 (タイマの分周器の途中経過など) は追わない。

#ifndef X68K_CORE_DEV_MFP_H
#define X68K_CORE_DEV_MFP_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Mfp
{
public:
    // レジスタ番号 (ベースからのオフセット / 2)。
    // MFP は奇数アドレスにのみレジスタが並ぶ (8bit デバイスを 16bit バスに繋ぐため)。
    enum Reg : u32
    {
        kGpip = 0x00,  // 汎用入出力。bit4 に垂直帰線が入る
        kAer = 0x01,   // アクティブエッジ
        kDdr = 0x02,   // 入出力方向
        kIera = 0x03,  // 割り込み許可 A
        kIerb = 0x04,
        kIpra = 0x05,  // 割り込み保留 A
        kIprb = 0x06,
        kIsra = 0x07,  // 割り込みサービス中 A
        kIsrb = 0x08,
        kImra = 0x09,  // 割り込みマスク A
        kImrb = 0x0A,
        kVr = 0x0B,    // ベクタ番号 (上位 4bit)
        kTacr = 0x0C,  // タイマ A 制御
        kTbcr = 0x0D,
        kTcdcr = 0x0E,  // タイマ C/D 制御
        kTadr = 0x0F,   // タイマ A データ
        kTbdr = 0x10,
        kTcdr = 0x11,
        kTddr = 0x12,
        kScr = 0x13,  // 同期文字
        kUcr = 0x14,  // USART 制御
        kRsr = 0x15,  // 受信状態
        kTsr = 0x16,  // 送信状態
        kUdr = 0x17,  // USART データ
        kRegCount = 0x18,
    };

    // IERA/IPRA のビット。X68000 での割り当て。
    static constexpr u8 kIntGpip7 = 0x80;  // 未使用
    static constexpr u8 kIntGpip6 = 0x40;  // CRTC 同期
    static constexpr u8 kIntTimerA = 0x20;
    static constexpr u8 kIntRecvFull = 0x10;  // キーボード受信
    static constexpr u8 kIntRecvError = 0x08;
    static constexpr u8 kIntSendEmpty = 0x04;
    static constexpr u8 kIntSendError = 0x02;
    static constexpr u8 kIntTimerB = 0x01;  // 水平同期

    // IERB/IPRB のビット。
    static constexpr u8 kIntGpip5 = 0x80;
    static constexpr u8 kIntGpip4 = 0x40;  // 垂直帰線
    static constexpr u8 kIntTimerC = 0x20;
    static constexpr u8 kIntTimerD = 0x10;
    static constexpr u8 kIntGpip3 = 0x08;
    static constexpr u8 kIntGpip2 = 0x04;
    static constexpr u8 kIntGpip1 = 0x02;
    static constexpr u8 kIntGpip0 = 0x01;

    void reset();

    [[nodiscard]] u8 read(u32 regIndex) const;
    void write(u32 regIndex, u8 value);

    // CPU のサイクル数ぶん時間を進める。タイマのカウントダウンを行う。
    void tick(u32 cycles);

    // 垂直帰線の開始/終了を通知する。GPIP4 の状態が変わり、
    // 設定によっては割り込みが上がる。
    void setVerticalBlank(bool active);

    // キーボードから 1 バイト受信した。受信バッファに積んで割り込みを上げる。
    void receiveKeyboardByte(u8 value);

    // レジスタの現在値を副作用なしで見る。
    //
    // read() は読み出しで状態が変わるレジスタがあるため、状態を調べる用途には
    // 使えない。「キー入力が届かない」ような不具合は、割り込みがマスクされて
    // いるのか上がっていないのかで原因が全く違うので、覗く手段が要る。
    [[nodiscard]] u8 peek(u32 reg) const
    {
        return reg < reg_.size() ? reg_[reg] : 0u;
    }

    // 保留中で、マスクされていない割り込みがあるか。
    [[nodiscard]] bool hasPendingInterrupt() const;

    // 最も優先度の高い保留割り込みのベクタ番号を返し、その割り込みを
    // サービス中へ移す。保留が無ければ 0 を返す。
    u32 acknowledgeInterrupt();

private:
    void raise(bool groupA, u8 bit);
    [[nodiscard]] u32 timerPrescale(u8 control) const;
    void tickTimer(int index, u8 control, u32 cycles);

    std::array<u8, kRegCount> reg_{};
    // 各タイマの分周カウンタ。実機の分周器に相当する。
    std::array<u32, 4> prescaleCounter_{};
    // タイマの現在値。データレジスタ書き込みでリロードされる。
    std::array<u8, 4> timerValue_{};
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_MFP_H
