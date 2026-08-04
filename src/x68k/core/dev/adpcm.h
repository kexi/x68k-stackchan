// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ADPCM 音源 MSM6258V (OKI) ($E92000)。
//
// X68000 の PCM 音源。4bit の OKI ADPCM (いわゆる VOX/Dialogic ADPCM) を
// 1 バイトに 2 サンプル詰めて再生する。
//
// レジスタは 2 本:
//   $E92001 コマンド (書き) / ステータス (読み)
//   $E92003 データ (書き込むと 1 バイト = 2 サンプルぶん積まれる)
//
// IPL-ROM の実際の使い方 (rom/iplrom.dat のファイル先頭 = $FE0000):
//     FF9A68: 13FC 0004 00E92001   MOVE.B #$04,$E92001   ; 停止
//     FF9A8C: 13FC 0002 00E92001   MOVE.B #$02,$E92001   ; 再生開始
// コマンドはビットの意味で、bit0=録音, bit1=再生, bit2=停止。
//
// サンプリングレートは PPI ($E9A000) のポート C と、ADPCM 自身の分周設定で
// 決まる。X68000 では 3.9 / 5.2 / 7.8 / 10.4 / 15.6 kHz が選べる。
// このクラスはレートを「外から与えられる値」として持ち、実際の分周設定の
// 解釈は呼び出し側 (Machine と PPI) の責務にしてある。
//
// 【実装範囲と近似】
// 実装したもの:
//   - OKI ADPCM のデコーダ (ステップテーブル、差分の組み立て、飽和)
//   - コマンド (再生/停止) と、FIFO へのデータ投入
//   - pull 型のサンプル生成
// 近似したもの:
//   - DMA (DMAC ch3) 経由の連続転送は張っていない。CPU が $E92003 へ
//     書く経路だけを実装する。
//   - パン (PPI ポート A) は保持しない。モノラルで出す。
//   - 再生レートと出力レートが違う場合は最近傍で伸ばす。

#ifndef X68K_CORE_DEV_ADPCM_H
#define X68K_CORE_DEV_ADPCM_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Adpcm
{
public:
    // コマンドレジスタ ($E92001) のビット。
    static constexpr u8 kCommandRecord = 0x01;
    static constexpr u8 kCommandPlay = 0x02;
    static constexpr u8 kCommandStop = 0x04;

    // ステータスレジスタ ($E92001) のビット。
    // bit7 = 再生中 (BUSY 相当)。IOCS はこれを見て転送の完了を待つ。
    static constexpr u8 kStatusPlaying = 0x80;

    // FIFO の深さ。実チップの FIFO は浅い (数バイト) が、ここでは
    // CPU が書いた分を取りこぼさないよう余裕を持たせる。
    //
    // Why not 実チップと同じ深さにするか: 実機は DMAC が一定間隔で供給し、
    // FIFO が空になる前に次が来る。DMA を張っていない本実装では CPU が
    // まとめて書くので、浅いと落ちる。深くして困るのは遅延だけ。
    static constexpr std::size_t kFifoBytes = 256;

    // ADPCM の既定サンプリングレート。X68000 で最もよく使われる設定。
    static constexpr u32 kDefaultSampleRate = 15625;

    Adpcm();

    void reset();

    // --- CPU から見える口 ---

    // $E92001 への書き込み (コマンド)。
    void writeCommand(u8 value);

    // $E92001 の読み出し (ステータス)。
    [[nodiscard]] u8 readStatus() const;

    // $E92003 への書き込み (データ 1 バイト = 2 サンプル)。
    void writeData(u8 value);

    // --- デコーダ単体 ---
    //
    // 4bit のニブルを 1 サンプルへ変換し、内部状態を進める。
    // 戻り値は 12bit 相当の符号付き値 (-2048..2047)。実チップの DAC も 12bit。
    //
    // 実チップと同じく、状態は「直前の信号レベル」と「ステップインデックス」の
    // 2 つだけ。これが OKI ADPCM が軽い理由。
    std::int16_t decodeNibble(u8 nibble);

    // デコーダの状態を初期化する (信号レベル 0、ステップ 0)。
    void resetDecoder();

    [[nodiscard]] std::int32_t signalLevel() const
    {
        return signal_;
    }

    [[nodiscard]] u32 stepIndex() const
    {
        return stepIndex_;
    }

    // --- サンプル生成 ---

    // frames サンプルぶん合成して out に書く。
    // FIFO が空、または停止中なら 0 で埋める。
    void renderSamples(std::int16_t* out, std::size_t frames);

    [[nodiscard]] std::int16_t renderOneSample();

    [[nodiscard]] bool isPlaying() const
    {
        return playing_;
    }

    // 出力レートと ADPCM のレートを設定する。
    void setSampleRate(u32 outputRate, u32 adpcmRate);

    [[nodiscard]] std::size_t fifoCount() const
    {
        return fifoCount_;
    }

private:
    // 次のニブルを取り出してデコードする。FIFO が空なら false。
    bool nextNibble(u8* nibble);

    // --- デコーダの状態 ---
    std::int32_t signal_ = 0;  // 直前の信号レベル (12bit 相当)
    u32 stepIndex_ = 0;        // ステップテーブルの位置 (0-48)

    // --- FIFO ---
    std::array<u8, kFifoBytes> fifo_{};
    std::size_t fifoHead_ = 0;
    std::size_t fifoCount_ = 0;
    // 1 バイトのうち上位ニブルを次に読むか。
    // OKI ADPCM は上位ニブルが先。
    bool highNibblePending_ = false;
    u8 currentByte_ = 0;

    bool playing_ = false;

    // 出力レートと ADPCM レートの比。最近傍で伸ばすための位相。
    u32 outputRate_ = kDefaultSampleRate;
    u32 adpcmRate_ = kDefaultSampleRate;
    u32 resamplePhase_ = 0;
    std::int16_t lastSample_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_ADPCM_H
