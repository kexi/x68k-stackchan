// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// エミュレーションコアと音声出力の間でサンプルを受け渡し、スピーカーへ流す。
//
// なぜ要るか:
//   Machine (と その中の Opm / Adpcm) を触ってよいのは Core1 だけ。
//   frame_channel.h と同じ理由で、別コアから Machine::renderAudio を呼ぶと
//   ゲストが $E90003 を書いている最中のレジスタを読むことになり、
//   エンベロープや FIFO の途中の状態が混ざる。
//
//   加えて M5Unified の Speaker_Class::playRaw は渡したポインタを
//   コピーせずに保持し、しかも「前の 2 枚がまだ再生待ちなら空くまで待つ」
//   というブロッキングの口を持つ (Speaker_Class.cpp の _set_next_wav)。
//   これをエミュレーションのホットループから直に呼ぶと、68000 の実行が
//   スピーカーの再生速度に引きずられて止まる。
//
//   合成は Core1 で行い、できたブロックをリングへ積む。取り出して
//   playRaw へ渡すのは別タスク。待つのはそのタスクだけになる。
//
// 受け渡しの形 (frame_channel.h の 2 枚バッファと同じ発想):
//   固定長のブロックを kBlockCount 枚のリングにする。単一生産者
//   (Core1) / 単一消費者 (音声タスク) なので、head/tail を atomic に
//   すればロックが要らない。ロックを取ると、合成が 1 ブロック 512 サンプル
//   ぶんの間ミューテックスを持つことになり、消費側の playRaw と噛み合う。
//
//   Why not FreeRTOS の Queue にコピーで積まないか: 1 ブロック 1KB を
//   キューへ入れると、積むときと降ろすときの 2 回コピーが要る。
//   リングなら生産側が直接書ける。消費側は readBlock/releaseRead で
//   コピー完了まで領域を保持し、非同期再生用のコピーは sink が所有する。
//   段数だけでは、再生待ち中の生産者による上書きを防げない。

#ifndef X68K_PLATFORM_AUDIO_H
#define X68K_PLATFORM_AUDIO_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "machine.h"

namespace x68k_platform
{

// サンプルの行き先。実機では M5Unified のスピーカー、テストでは偽物。
//
// Why not AudioChannel が直接 M5.Speaker を呼ばないか: そうするとホストの
// テストからリングの受け渡しを一切確かめられない。「鳴らす先」を抽象に
// しておけば、キーオンしたときに本当に非無音のサンプルが出口まで届くかを
// ホストで検査できる (test/test_audio.cpp)。
class AudioSink
{
public:
    virtual ~AudioSink() = default;

    // frames サンプル (モノラル 16bit) を鳴らす。
    //
    // samples は呼び出し中だけ有効。非同期出力は sink 自身の領域へコピーする。
    virtual void write(const std::int16_t* samples, std::size_t frames) = 0;

    // Called by the sole output owner on a stream discontinuity. Async sinks must
    // finish releasing all retained PCM before accepting buffers from the new epoch.
    virtual bool restartStream()
    {
        return true;
    }

    // 出力サンプリングレート (Hz)。
    [[nodiscard]] virtual x68k::u32 sampleRate() const = 0;
};

// エミュレーションコア (生産) と音声タスク (消費) の間のリング。
//
// ESP-IDF に触らないので、そのままホストのテストでビルドできる。
class AudioChannel
{
public:
    // 1 ブロックのサンプル数。
    //
    // 15625Hz なら 1 ブロック = 32.8ms。エミュレーションのスライス
    // (20000 サイクル ≒ 6.1ms) より長いので、1 スライスで 1 ブロックを
    // 埋めきることはない。埋まったときだけ push する形にすると、
    // 音声タスクへの受け渡しが 5-6 スライスに 1 回で済む。
    //
    // Why not もっと短くするか: 短いと push の回数が増え、消費側の
    // playRaw の呼び出しも増える。Speaker は 1 回の playRaw ごとに
    // タスク通知を出すので、細かくすると通知の分だけ Core0 を食う。
    // Why not もっと長くするか: 遅延がそのまま伸びる。512 で 33ms は
    // ゲームの効果音として許せる上限に近い。
    static constexpr std::size_t kBlockFrames = 512;

    // リングの段数。
    //
    // 消費中を含む最大3枚と予約1枚。非同期再生中の領域は別途 sink が持つ。
    static constexpr std::size_t kBlockCount = 4;

    // 単一消費者が先頭を借りる。releaseRead まで生産者は上書きしない。
    // 再度呼ぶと同じ先頭を返す。空なら nullptr。
    [[nodiscard]] const std::int16_t* readBlock();
    void releaseRead();

    // 生産側が書き込む先。1 ブロックぶん (kBlockFrames サンプル)。
    //
    // 空きが無ければ nullptr。合成そのものを省けるよう、書く前に問える形に
    // してある (無音でも 512 サンプルぶんの合成は無駄になる)。
    [[nodiscard]] std::int16_t* writeBlock();

    // writeBlock へ書き終えたことを伝える。以後 readBlock で取り出せる。
    void commit();

    // Producer-only, between commits. Do not rewind the consumer index or revoke
    // a read lease; the consumer discards the old prefix when it next acquires.
    void restartStream(bool paused);
    [[nodiscard]] std::uint32_t streamState() const
    {
        return streamState_.load(std::memory_order_acquire);
    }

    // 溜まっているブロック数。
    [[nodiscard]] std::size_t pending() const;

    // 空きが無くて捨てたブロックの累計。実機で「音が途切れているか」を
    // 数字で見るために持つ (増え続けるなら合成が追いついていない)。
    [[nodiscard]] std::uint32_t droppedBlocks() const
    {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    std::int16_t blocks_[kBlockCount][kBlockFrames]{};

    // 生産側だけが書き、消費側だけが読む (と、その逆)。
    // 単一生産者 / 単一消費者なので CAS は要らない。
    std::atomic<std::size_t> writeIndex_{0};
    std::atomic<std::size_t> readIndex_{0};
    std::atomic<std::uint32_t> dropped_{0};
    std::atomic<std::size_t> discardBefore_{0};
    // Low bit is paused; upper bits distinguish resets and pause/resume epochs.
    std::atomic<std::uint32_t> streamState_{0};
    bool readLeased_ = false;  // Consumer-owned, never read by the producer.
};

// Machine から 1 ブロックぶん合成してリングへ積む。エミュレーションコアから呼ぶ。
//
// 有効でない (enabled == false) か、リングに空きが無ければ何もしない。
// 積んだら true。
//
// Why not AudioChannel のメソッドにしないか: AudioChannel は「サンプルの
// 受け渡し」だけを持つ器で、Machine を知らない方が偽の生産者でテストできる。
bool pumpAudio(x68k::Machine& machine, AudioChannel& channel);

// ブロックの振幅の最大値 (絶対値)。0 なら完全な無音。
//
// 実機では音を耳で確かめられないので、これをログへ出して
// 「キーオン後に非ゼロ、待機中はゼロ」を数字で確認する。
[[nodiscard]] std::int32_t peakAmplitude(const std::int16_t* samples, std::size_t frames);

// リングから取り出して sink へ流す。音声タスクから呼ぶ。
// 流したブロック数を返す。
std::size_t drainAudio(AudioChannel& channel, AudioSink& sink);

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_AUDIO_H
