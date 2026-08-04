// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// このファイルは ESP-IDF に触らない。M5Unified のスピーカーを叩くのは
// speaker_m5.h (ヘッダオンリー) の方で、そちらは実機ビルドからしか
// インクルードされない。分けてあるのは、リングの受け渡しと
// 「キーオンで非無音のサンプルが出口へ届くか」をホストのテストで
// 確かめられるようにするため。

#include "audio.h"

namespace x68k_platform
{

const std::int16_t* AudioChannel::pop()
{
    const std::size_t read = readIndex_.load(std::memory_order_relaxed);

    // acquire で読むのは、生産側が commit (release) するより前に書いた
    // サンプルがこちらから見えることを保証するため。これが無いと、
    // インデックスだけ進んで中身が古いままのブロックを鳴らしうる。
    const std::size_t write = writeIndex_.load(std::memory_order_acquire);
    if (read == write)
    {
        return nullptr;
    }

    const std::int16_t* const block = blocks_[read % kBlockCount];
    readIndex_.store(read + 1, std::memory_order_release);
    return block;
}

std::int16_t* AudioChannel::writeBlock()
{
    const std::size_t write = writeIndex_.load(std::memory_order_relaxed);
    const std::size_t read = readIndex_.load(std::memory_order_acquire);

    // 空きが無ければ捨てる。
    //
    // Why not 空くまで待つか: ここはエミュレーションのホットループ。
    // 待つと 68000 の実行が音声タスクの進み方に縛られ、実効クロックが
    // 出力レートで頭打ちになる。表示側 (frame_channel.cpp の publish) と
    // 同じ判断で、追いつかないぶんは落とす。
    //
    // 段数 kBlockCount に対して 1 枚は必ず空けておく。埋め切ると
    // write == read となり、空と満杯が区別できなくなる。
    const bool isFull = write - read >= kBlockCount - 1;
    if (isFull)
    {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    return blocks_[write % kBlockCount];
}

void AudioChannel::commit()
{
    const std::size_t write = writeIndex_.load(std::memory_order_relaxed);
    // release で公開する。上の pop の acquire と対になる。
    writeIndex_.store(write + 1, std::memory_order_release);
}

std::size_t AudioChannel::pending() const
{
    return writeIndex_.load(std::memory_order_acquire) - readIndex_.load(std::memory_order_acquire);
}

bool pumpAudio(x68k::Machine& machine, AudioChannel& channel)
{
    std::int16_t* const block = channel.writeBlock();
    if (block == nullptr)
    {
        return false;
    }

    machine.renderAudio(block, AudioChannel::kBlockFrames);
    channel.commit();
    return true;
}

std::int32_t peakAmplitude(const std::int16_t* samples, std::size_t frames)
{
    if (samples == nullptr)
    {
        return 0;
    }

    std::int32_t peak = 0;
    for (std::size_t i = 0; i < frames; ++i)
    {
        // -32768 の符号反転は int16 に収まらない。int32 へ広げてから取る。
        std::int32_t value = samples[i];
        if (value < 0)
        {
            value = -value;
        }
        if (value > peak)
        {
            peak = value;
        }
    }
    return peak;
}

std::size_t drainAudio(AudioChannel& channel, AudioSink& sink)
{
    std::size_t written = 0;
    while (const std::int16_t* block = channel.pop())
    {
        sink.write(block, AudioChannel::kBlockFrames);
        ++written;
    }
    return written;
}

}  // namespace x68k_platform
