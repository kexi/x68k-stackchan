// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: SASI 経由でブートセクタが読み出せること。
//
// IPL-ROM は起動デバイスを探し、SASI へ 6 バイトのコマンドを送って
// ブートセクタ (論理セクタ 0 から 1024 バイト) を $002000 へ読み込む。
// この経路が通らないと Human68k のロードまで辿り着けない。
//
// SASI を選んでいるのは FDC より実装が単純なため。FDC はデータ転送が
// DMAC 経由になるので DMAC の実装も要る。

#include <cstring>
#include <vector>

#include "doctest.h"
#include "machine.h"

namespace
{

// テスト用のディスク。セクタごとに識別できる中身を入れておく。
class FakeDisk final : public x68k::DiskImage
{
public:
    static constexpr x68k::u32 kSectorSize = 256;

    explicit FakeDisk(x68k::u32 sectorCount) : data_(sectorCount * kSectorSize, 0)
    {
        // 各セクタの先頭にセクタ番号を書いて、どのセクタが読まれたか分かるようにする。
        for (x68k::u32 s = 0; s < sectorCount; ++s)
        {
            data_[s * kSectorSize] = static_cast<x68k::u8>(s);
            data_[s * kSectorSize + 1] = 0xA5;
        }
    }

    bool readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount) override
    {
        const std::size_t offset = static_cast<std::size_t>(lba) * kSectorSize;
        const std::size_t length = static_cast<std::size_t>(sectorCount) * kSectorSize;
        if (offset + length > data_.size())
        {
            return false;
        }
        std::memcpy(buffer, data_.data() + offset, length);
        ++readCount;
        return true;
    }

    bool writeSector(x68k::u32 lba, const x68k::u8* buffer, x68k::u32 sectorCount) override
    {
        const std::size_t offset = static_cast<std::size_t>(lba) * kSectorSize;
        const std::size_t length = static_cast<std::size_t>(sectorCount) * kSectorSize;
        if (offset + length > data_.size())
        {
            return false;
        }
        std::memcpy(data_.data() + offset, buffer, length);
        return true;
    }

    [[nodiscard]] bool isPresent() const override
    {
        return present;
    }

    std::vector<x68k::u8> data_;
    int readCount = 0;
    bool present = true;
};

// SASI のレジスタ。
constexpr x68k::u32 kSasiData = x68k::kSasiBase + 1;
constexpr x68k::u32 kSasiStatus = x68k::kSasiBase + 3;

// ステータスのビット。
constexpr x68k::u8 kBusy = 0x01;
constexpr x68k::u8 kRequest = 0x02;
constexpr x68k::u8 kCommandData = 0x08;  // C/D: 1 = コマンド/ステータス
constexpr x68k::u8 kInputOutput = 0x10;  // I/O: 1 = ターゲット → イニシエータ

// SASI へ 6 バイトのコマンドを送る手順をなぞる。
void sendCommand(x68k::Machine& m, x68k::u8 opcode, x68k::u32 lba, x68k::u8 count)
{
    // セレクション。ターゲット ID を書く。
    m.ioWrite8(kSasiData, 0x01);

    const x68k::u8 command[6] = {
        opcode,
        static_cast<x68k::u8>((lba >> 16) & 0x1Fu),
        static_cast<x68k::u8>((lba >> 8) & 0xFFu),
        static_cast<x68k::u8>(lba & 0xFFu),
        count,
        0,
    };
    for (const x68k::u8 b : command)
    {
        m.ioWrite8(kSasiData, b);
    }
}

}  // namespace

TEST_CASE("バスフリー状態ではビジーが立たない")
{
    x68k::Machine m;
    const x68k::u8 status = m.ioRead8(kSasiStatus);
    CHECK((status & kBusy) == 0);
}

TEST_CASE("READ コマンドで指定セクタが読める")
{
    x68k::Machine m;
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08 /* READ */, 0, 1);

    // データ転送フェーズになる。ターゲットからイニシエータへの向き。
    const x68k::u8 status = m.ioRead8(kSasiStatus);
    CHECK((status & kBusy) != 0);
    CHECK((status & kRequest) != 0);
    CHECK((status & kInputOutput) != 0);

    // セクタ 0 の中身が読める。
    CHECK(m.ioRead8(kSasiData) == 0x00);  // セクタ番号
    CHECK(m.ioRead8(kSasiData) == 0xA5);  // 目印
    CHECK(disk.readCount == 1);
}

TEST_CASE("セクタを読み切るとステータスフェーズへ移る")
{
    x68k::Machine m;
    FakeDisk disk(4);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 0, 1);

    // 256 バイト読み切る。
    for (int i = 0; i < 256; ++i)
    {
        m.ioRead8(kSasiData);
    }

    const x68k::u8 status = m.ioRead8(kSasiStatus);
    // ステータスフェーズは C/D と I/O の両方が立つ。
    CHECK((status & kCommandData) != 0);
    CHECK((status & kInputOutput) != 0);

    // ステータスは 0 (正常終了)。
    CHECK(m.ioRead8(kSasiData) == 0x00);
}

TEST_CASE("LBA を指定して別のセクタが読める")
{
    x68k::Machine m;
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 5, 1);
    CHECK(m.ioRead8(kSasiData) == 0x05);
    CHECK(m.ioRead8(kSasiData) == 0xA5);
}

TEST_CASE("ディスクが無ければエラーステータスを返す")
{
    x68k::Machine m;
    // setDisk を呼ばない = ディスクなし。

    sendCommand(m, 0x08, 0, 1);

    // データフェーズには入らず、ステータスフェーズでエラーが返る。
    const x68k::u8 status = m.ioRead8(kSasiStatus);
    CHECK((status & kCommandData) != 0);
    CHECK(m.ioRead8(kSasiData) != 0x00);  // 非ゼロ = エラー
}

TEST_CASE("TEST UNIT READY でディスクの有無が分かる")
{
    x68k::Machine m;
    FakeDisk disk(4);
    m.setDisk(&disk);

    sendCommand(m, 0x00 /* TEST UNIT READY */, 0, 0);
    CHECK(m.ioRead8(kSasiData) == 0x00);  // ステータス: 正常
    // メッセージフェーズを消化してバスフリーへ戻す。
    // これを飛ばすと次のセレクションが受け付けられない。
    m.ioRead8(kSasiData);

    disk.present = false;
    sendCommand(m, 0x00, 0, 0);
    CHECK(m.ioRead8(kSasiData) != 0x00);  // ステータス: エラー
}

TEST_CASE("REQUEST SENSE がセンスデータを返す")
{
    x68k::Machine m;
    FakeDisk disk(4);
    m.setDisk(&disk);

    sendCommand(m, 0x03 /* REQUEST SENSE */, 0, 0);

    // 4 バイトのセンスデータ。エラー無しなので全部 0。
    for (int i = 0; i < 4; ++i)
    {
        CHECK(m.ioRead8(kSasiData) == 0x00);
    }
}

TEST_CASE("ブートセクタ 4 つぶん (1024 バイト) を順に読める")
{
    // IPL-ROM はブートセクタとして 1024 バイトを読む。
    // SASI は 256 バイト/セクタなので 4 回に分かれる。
    x68k::Machine m;
    FakeDisk disk(16);
    m.setDisk(&disk);

    for (x68k::u32 sector = 0; sector < 4; ++sector)
    {
        sendCommand(m, 0x08, sector, 1);
        CHECK(m.ioRead8(kSasiData) == static_cast<x68k::u8>(sector));
        // 残りを読み飛ばしてステータスまで進める。
        for (int i = 1; i < 256; ++i)
        {
            m.ioRead8(kSasiData);
        }
        CHECK(m.ioRead8(kSasiData) == 0x00);  // ステータス
        m.ioRead8(kSasiData);                 // メッセージ
    }

    CHECK(disk.readCount == 4);
}

TEST_CASE("未対応コマンドはエラーを返して固まらない")
{
    x68k::Machine m;
    FakeDisk disk(4);
    m.setDisk(&disk);

    // 実装していないコマンド。ここで状態機械が止まると
    // 以降のアクセスが全部おかしくなる。
    sendCommand(m, 0x1A /* MODE SENSE */, 0, 0);
    CHECK(m.ioRead8(kSasiData) != 0x00);

    // 次のコマンドは正常に受け付けられる。
    m.ioRead8(kSasiData);  // メッセージフェーズを消化
    sendCommand(m, 0x08, 1, 1);
    CHECK(m.ioRead8(kSasiData) == 0x01);
}
