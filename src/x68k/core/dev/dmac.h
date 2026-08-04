// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// DMAC (HD63450) ($E84000)。
//
// X68000 の SASI と FDC はデータ転送を DMAC 経由で行う。IPL-ROM はブートセクタを
// 読むとき、SASI へ READ コマンドを送った後に DMAC のチャネル 1 を設定して
// 起動し ($FF9944)、転送が終わるのを待つ。DMAC が無いとブートセクタが
// メモリへ届かず、IPL-ROM の "X68K" 検査 ($FF91FA) で必ず失敗する。
//
// FDC はチャネル 0 を使う。IPL-ROM の $FF8F3C が
//   $E84000 (CSR) へ $FF     … 溜まったステータスを落とす
//   $E84005 (OCR) へ $B2     … bit7 = デバイス → メモリ
//   $E8400C (MAR) へ転送先
//   $E8400A (MTC) へ転送バイト数
//   $E84007 (CCR) へ $80     … 起動
// と書き、$FF9014 が $E84000 の bit4 (ERR) を見て成否を判定する。
//
// 実装範囲: チャネル 0 (FDC) と チャネル 1 (SASI) の、メモリとの単純転送。
// チェイン転送や複数チャネルの同時進行の調停は実装しない (実機は 1 バイトずつ
// バスを取り合うが、本エミュレータは起動された時点で一気に転送し切るので、
// 2 チャネルが同時に走っている状態そのものが存在しない)。

#ifndef X68K_CORE_DEV_DMAC_H
#define X68K_CORE_DEV_DMAC_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

// DMAC が転送するデータの出どころ。SASI と FDC がこれを実装する。
class DmaDevice
{
public:
    virtual ~DmaDevice() = default;

    // デバイスからメモリへ 1 バイト渡す。転送できるものが無ければ false。
    virtual bool dmaRead(u8* value) = 0;
    // メモリからデバイスへ 1 バイト受け取る。
    virtual bool dmaWrite(u8 value) = 0;

    // 転送が終わった (ターミナルカウントに達した、または打ち切られた)。
    //
    // これが無いと、自分では転送長を知らないデバイスが終わりを検知できない。
    // FDC がまさにそれで、READ/WRITE DATA は「EOT に達するか DMAC が
    // 止めるまで」続く。DMAC が黙って呼ぶのをやめると、FDC は実行フェーズに
    // 居座ったままメインステータスの CB を立て続け、次のコマンド送出
    // ($FF9036 の CB 待ち) がそこで永久に止まる。
    //
    // SASI は自分で転送長を知っている (コマンドのセクタ数から決まる) ので
    // 何もしなくてよい。既定実装を空にしてあるのはそのため。
    virtual void dmaComplete(bool /*isComplete*/) {}
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
    // 各チャネルのレジスタ。先頭からのオフセット。
    static constexpr u32 kChannelStride = 0x40;
    static constexpr u32 kChannelCount = 4;
    static constexpr u32 kFdcChannel = 0;
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
    static constexpr u8 kCsrError = 0x10;                     // ERR

    // CER のエラーコード。要求量に届かないまま打ち切ったときに立てる。
    //
    // 実機の CER は原因ごとに細かいコードを持つが、本エミュレータでは
    // デバイスがデータを出せなくなった場合しか中断が起きないので、
    // バス由来の中断を表す 0x09 (bus error / メモリ側) ではなく
    // 0x0A (デバイス側のバスエラー) をひとつだけ使う。
    static constexpr u8 kCerBusErrorDevice = 0x0A;

    // OCR bit7 (DIR): 1 = デバイス → メモリ、0 = メモリ → デバイス。
    //
    // IPL-ROM はブートセクタを読むとき OCR に $B2 を書く ($FF994E)。
    // bit7 が立っているのでこれが「読み出し」の向き。逆に取ると
    // 1 バイトも転送されず、"X68K" の検査で必ず失敗する。
    static constexpr u8 kOcrDirectionToMemory = 0x80;

    // CCR bit7: 転送開始。
    static constexpr u8 kCcrStart = 0x80;

    void reset();

    // チャネルに繋ぐデバイスを指定する。
    //
    // Why not 単一のデバイスに戻すか: 以前は「デバイスは 1 つ」で、
    // SASI だけが繋がっていた。FDC (チャネル 0) を足すと、どちらへ
    // バイトを渡すかをチャネル番号でしか区別できない。呼び出し側
    // (Machine) に「今どっちが動いているか」を持たせると、転送の
    // 途中でその状態がずれたときに黙って相手を取り違える。
    void setDevice(u32 channel, DmaDevice* device);

    // 後方互換の入口。チャネル 1 (SASI) へ繋ぐ。
    void setDevice(DmaDevice* device)
    {
        setDevice(kSasiChannel, device);
    }

    void setMemory(DmaMemory* memory)
    {
        memory_ = memory;
    }

    [[nodiscard]] u8 read(u32 offset) const;
    void write(u32 offset, u8 value);

private:
    // 指定チャネルの転送を最後まで行う。
    //
    // 実機は 1 バイトずつバスを取り合いながら進むが、本エミュレータでは
    // 起動された時点で一気に転送し切る。IPL-ROM は完了を待つだけなので
    // 差が出ない。バスを止めないぶん、むしろ速い。
    void runTransfer(u32 channel);

    // チャネルごとのレジスタ実体。
    std::array<std::array<u8, kChannelStride>, kChannelCount> channels_{};

    std::array<DmaDevice*, kChannelCount> devices_{};
    DmaMemory* memory_ = nullptr;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_DMAC_H
