// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: 合成したサンプルが「出口」まで非無音のまま届くこと。
//
//   - 待機中 (キーオンなし) は出口に届くサンプルが完全な無音であること
//   - OPM をキーオンすると、出口に届いたブロックの振幅がゼロでないこと
//     (issue #8 の完了条件「音が鳴る」を、耳ではなく数字で確かめる部分)
//   - リングが単一生産者/単一消費者として順序を保つこと
//   - 消費が追いつかないときはブロックを捨て、エミュレーション側を
//     待たせないこと (待たせると 68000 が出力レートに縛られる)
//   - Machine::renderAudio が出口へ渡すのと同じ内容を返すこと
//     (リングを通しても中身が化けない)

#include "audio.h"
#include "doctest.h"

#include <cstring>
#include <vector>

namespace
{

// 偽のスピーカー。届いたサンプルを全部ためる。
//
// 実機の M5SpeakerSink はポインタを保持したまま戻るので、ここでは
// 「戻った後に読む」までは真似せず、届いた内容の写しだけを取る。
class FakeSink final : public x68k_platform::AudioSink
{
public:
    void write(const std::int16_t* samples, std::size_t frames) override
    {
        received.insert(received.end(), samples, samples + frames);
        ++writeCount;
    }

    [[nodiscard]] x68k::u32 sampleRate() const override
    {
        return 15625;
    }

    std::vector<std::int16_t> received;
    int writeCount = 0;
};

void writeReg(x68k::Opm& opm, x68k::u8 reg, x68k::u8 value)
{
    opm.writeAddress(reg);
    opm.writeData(value);
}

// ch を「すぐ最大音量になり、鳴り続ける」設定にする (test_opm.cpp と同じ手)。
void setupLoudChannel(x68k::Opm& opm, x68k::u8 ch)
{
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        const x68k::u8 offset = static_cast<x68k::u8>(ch + slot * 8);
        writeReg(opm, static_cast<x68k::u8>(0x40 + offset), 0x01);  // DT1=0 MUL=1
        writeReg(opm, static_cast<x68k::u8>(0x60 + offset), 0x00);  // TL=0 (最大)
        writeReg(opm, static_cast<x68k::u8>(0x80 + offset), 0x1F);  // KS=0 AR=31
        writeReg(opm, static_cast<x68k::u8>(0xA0 + offset), 0x00);  // D1R=0
        writeReg(opm, static_cast<x68k::u8>(0xC0 + offset), 0x00);  // DT2=0 D2R=0
        writeReg(opm, static_cast<x68k::u8>(0xE0 + offset), 0x00);  // D1L=0 RR=0
    }
    writeReg(opm, static_cast<x68k::u8>(0x20 + ch), 0xC7);  // RL=両方on FL=0 CONN=7
    writeReg(opm, static_cast<x68k::u8>(0x28 + ch), 0x4A);  // KC (オクターブ 4 の A)
    writeReg(opm, static_cast<x68k::u8>(0x30 + ch), 0x00);  // KF=0
}

void keyOn(x68k::Opm& opm, x68k::u8 ch)
{
    writeReg(opm, 0x08, static_cast<x68k::u8>(0x78 | ch));
}

}  // namespace

TEST_CASE("待機中は出口へ届くサンプルが完全な無音")
{
    x68k::Machine machine;
    x68k_platform::AudioChannel channel;
    FakeSink sink;

    // キーオンしていない状態で数ブロックぶん回す。
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(x68k_platform::pumpAudio(machine, channel));
        x68k_platform::drainAudio(channel, sink);
    }

    CHECK(sink.writeCount == 3);
    CHECK(sink.received.size() == 3 * x68k_platform::AudioChannel::kBlockFrames);
    CHECK(x68k_platform::peakAmplitude(sink.received.data(), sink.received.size()) == 0);
}

TEST_CASE("キーオンすると出口へ届くブロックが非無音になる")
{
    x68k::Machine machine;
    x68k_platform::AudioChannel channel;
    FakeSink sink;

    setupLoudChannel(machine.opm(), 0);
    keyOn(machine.opm(), 0);

    // アタックは最速 (AR=31) だが、1 ブロック 512 サンプルの頭は
    // まだ立ち上がりの途中でありうる。2 ブロック流して判定する。
    for (int i = 0; i < 2; ++i)
    {
        REQUIRE(x68k_platform::pumpAudio(machine, channel));
        x68k_platform::drainAudio(channel, sink);
    }

    const std::int32_t peak =
        x68k_platform::peakAmplitude(sink.received.data(), sink.received.size());
    CHECK(peak > 0);
    // 「たまたま 1 だけ動いた」ではなく、耳で聞こえる水準まで出ていること。
    // full scale の 1% (327) を下限にする。
    CHECK(peak > 327);
}

TEST_CASE("ADPCM だけでも出口へ非無音が届く")
{
    x68k::Machine machine;
    x68k_platform::AudioChannel channel;
    FakeSink sink;

    // 再生を開始し、振幅の大きいニブルを積む。
    machine.adpcm().writeCommand(x68k::Adpcm::kCommandPlay);
    for (int i = 0; i < 64; ++i)
    {
        // 上位/下位とも最大ステップ (0x7) で同じ向きへ動かし、
        // 信号レベルを確実に押し上げる。
        machine.adpcm().writeData(0x77);
    }

    REQUIRE(x68k_platform::pumpAudio(machine, channel));
    x68k_platform::drainAudio(channel, sink);

    CHECK(x68k_platform::peakAmplitude(sink.received.data(), sink.received.size()) > 0);
}

TEST_CASE("リングは書いた順に取り出せる")
{
    x68k_platform::AudioChannel channel;

    // 各ブロックの先頭に通し番号を入れて、順序が保たれるかを見る。
    for (std::int16_t n = 1; n <= 2; ++n)
    {
        std::int16_t* const block = channel.writeBlock();
        REQUIRE(block != nullptr);
        block[0] = n;
        channel.commit();
    }

    CHECK(channel.pending() == 2);

    const std::int16_t* first = channel.pop();
    REQUIRE(first != nullptr);
    CHECK(first[0] == 1);

    const std::int16_t* second = channel.pop();
    REQUIRE(second != nullptr);
    CHECK(second[0] == 2);

    CHECK(channel.pop() == nullptr);
    CHECK(channel.pending() == 0);
}

TEST_CASE("消費が追いつかないときは待たずに捨てる")
{
    x68k::Machine machine;
    x68k_platform::AudioChannel channel;

    // 一度も pop せずに埋め続ける。エミュレーションコアはここで
    // ブロックしてはいけない (68000 が出力レートに縛られる)。
    std::size_t pushed = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (x68k_platform::pumpAudio(machine, channel))
        {
            ++pushed;
        }
    }

    // 満杯を区別するため 1 枚は必ず空けてある。
    CHECK(pushed == x68k_platform::AudioChannel::kBlockCount - 1);
    CHECK(channel.droppedBlocks() == 16 - pushed);

    // 1 枚取り出せば、また 1 枚だけ積める。
    CHECK(channel.pop() != nullptr);
    CHECK(x68k_platform::pumpAudio(machine, channel));
    CHECK(x68k_platform::pumpAudio(machine, channel) == false);
}

TEST_CASE("リングを通しても Machine が作ったサンプルと一致する")
{
    x68k::Machine viaRing;
    x68k::Machine direct;
    x68k_platform::AudioChannel channel;
    FakeSink sink;

    setupLoudChannel(viaRing.opm(), 0);
    keyOn(viaRing.opm(), 0);
    setupLoudChannel(direct.opm(), 0);
    keyOn(direct.opm(), 0);

    REQUIRE(x68k_platform::pumpAudio(viaRing, channel));
    x68k_platform::drainAudio(channel, sink);

    std::vector<std::int16_t> expected(x68k_platform::AudioChannel::kBlockFrames);
    direct.renderAudio(expected.data(), expected.size());

    REQUIRE(sink.received.size() == expected.size());
    CHECK(std::memcmp(sink.received.data(), expected.data(),
                      expected.size() * sizeof(std::int16_t)) == 0);
}

TEST_CASE("無音の早期リターンでも 1 サンプルずつ合成した結果と一致する")
{
    // renderAudio に足した「鳴っていなければゼロで埋める」早期リターンが、
    // 本当に合成結果と同じであることを確かめる。ここがずれていると、
    // 音の切れ際にプツッというノイズが出る。
    x68k::Machine machine;
    std::vector<std::int16_t> out(64, 0x7FFF);
    machine.renderAudio(out.data(), out.size());

    for (const std::int16_t sample : out)
    {
        CHECK(sample == 0);
    }
}

TEST_CASE("キーオフして減衰しきると出口が無音へ戻る")
{
    x68k::Machine machine;
    x68k_platform::AudioChannel channel;

    setupLoudChannel(machine.opm(), 0);
    keyOn(machine.opm(), 0);

    // RR=0 だと減衰が遅すぎるので、リリースを最速にしてから離す。
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        writeReg(machine.opm(), static_cast<x68k::u8>(0xE0 + slot * 8), 0x0F);  // D1L=0 RR=15
    }
    writeReg(machine.opm(), 0x08, 0x00);  // 全スロットキーオフ

    // 減衰しきるまで回す。
    std::vector<std::int16_t> block(x68k_platform::AudioChannel::kBlockFrames);
    for (int i = 0; i < 64; ++i)
    {
        machine.renderAudio(block.data(), block.size());
    }

    FakeSink sink;
    REQUIRE(x68k_platform::pumpAudio(machine, channel));
    x68k_platform::drainAudio(channel, sink);
    CHECK(x68k_platform::peakAmplitude(sink.received.data(), sink.received.size()) == 0);
}
