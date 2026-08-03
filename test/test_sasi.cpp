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
constexpr x68k::u32 kSasiSelect = x68k::kSasiBase + 7;

// ステータスレジスタが返すフェーズの値。IPL-ROM はこの値との一致を待つので、
// ビットの意味ではなく値そのものが仕様になる。
constexpr x68k::u8 kPhaseValueBusFree = 0x00;
constexpr x68k::u8 kPhaseValueDataOut = 0x0B;  // CPU からターゲットへ送る
constexpr x68k::u8 kPhaseValueDataIn = 0x07;   // ターゲットから CPU へ返す
constexpr x68k::u8 kPhaseValueStatus = 0x0F;   // 終了ステータスを読む
constexpr x68k::u8 kPhaseValueMessage = 0x1F;  // メッセージを読む

// SASI へ 6 バイトのコマンドを送る手順をなぞる。
void sendCommand(x68k::Machine& m, x68k::u8 opcode, x68k::u32 lba, x68k::u8 count)
{
    // セレクションは $E96007 へターゲット ID を書く。実機の IPL-ROM は
    // その後 $E96003 の bit1 が 0 になるのを待ってからコマンドを送る。
    m.ioWrite8(kSasiSelect, 0x01);
    m.ioRead8(kSasiStatus);

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

TEST_CASE("バスフリー状態では何のフェーズも示さない")
{
    x68k::Machine m;
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueBusFree);
}

TEST_CASE("READ コマンドで指定セクタが読める")
{
    x68k::Machine m;
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08 /* READ */, 0, 1);

    // ターゲットから CPU へ返す向きのデータフェーズになる。
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);

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

    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);

    // ステータスは 0 (正常終了)。読むとメッセージフェーズへ移る。
    CHECK(m.ioRead8(kSasiData) == 0x00);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueMessage);
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

    // ディスクが無ければセレクションが成立しないので、バスフリーのまま。
    // IPL-ROM 側はセレクション待ちがタイムアウトして「装置なし」と判断する。
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueBusFree);
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

    // ディスクを抜くとセレクションが成立しなくなる。実機ではここで
    // IPL-ROM のセレクション待ちがタイムアウトし、「装置なし」と判断される。
    disk.present = false;
    sendCommand(m, 0x00, 0, 0);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueBusFree);
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

TEST_CASE("READ は要求されたぶんのセクタをまとめて返す")
{
    // IPL-ROM はブートセクタを 4 セクタ (1024 バイト) まとめて要求する。
    // 1 セクタずつしか返さないと DMA が 256 バイトで止まり、
    // $002000 に読み込まれるブートコードが尻切れになる。
    x68k::Machine m;
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 0, 4);

    for (x68k::u32 sector = 0; sector < 4; ++sector)
    {
        CHECK(m.ioRead8(kSasiData) == static_cast<x68k::u8>(sector));
        CHECK(m.ioRead8(kSasiData) == 0xA5);
        // このセクタの残りを読み飛ばす。
        for (int i = 2; i < 256; ++i)
        {
            m.ioRead8(kSasiData);
        }
    }

    // 4 セクタ読み切ったのでステータスフェーズへ移る。
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
}

TEST_CASE("DMAC がセクタをメモリへ転送する")
{
    // X68000 の SASI はデータ転送を DMAC 経由で行う。IPL-ROM は
    // READ を発行した後 DMAC のチャネル 1 を起動し、転送が終わるのを待つ。
    // DMAC が無いとブートセクタがメモリへ届かない。
    x68k::Machine m;
    std::vector<x68k::u8> ram(x68k::kMainRamSize, 0);
    x68k::MemoryMap memory;
    memory.mainRam = ram.data();
    m.setMemory(memory);

    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 3, 1);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);

    // DMAC チャネル 1 のレジスタ。転送先は使っていないメインメモリの適当な場所。
    constexpr x68k::u32 kDmaChannel1 = x68k::kDmacBase + 0x40;
    constexpr x68k::u32 kDest = 0x00010000;

    m.ioWrite8(kDmaChannel1 + 0x00, 0xFF);  // CSR をクリア
    m.ioWrite8(kDmaChannel1 + 0x05, 0xB2);  // OCR: デバイス → メモリ
    // MAR (転送先アドレス、4 バイト)
    m.ioWrite8(kDmaChannel1 + 0x0C, static_cast<x68k::u8>(kDest >> 24));
    m.ioWrite8(kDmaChannel1 + 0x0D, static_cast<x68k::u8>(kDest >> 16));
    m.ioWrite8(kDmaChannel1 + 0x0E, static_cast<x68k::u8>(kDest >> 8));
    m.ioWrite8(kDmaChannel1 + 0x0F, static_cast<x68k::u8>(kDest));
    // MTC (転送バイト数、2 バイト)
    m.ioWrite8(kDmaChannel1 + 0x0A, 0x01);
    m.ioWrite8(kDmaChannel1 + 0x0B, 0x00);
    // CCR bit7 で起動。
    m.ioWrite8(kDmaChannel1 + 0x07, 0x80);

    // セクタ 3 の中身がメモリへ届いている。
    CHECK(m.bus().read8(kDest) == 0x03);
    CHECK(m.bus().read8(kDest + 1) == 0xA5);

    // 転送し切ったので DMAC は完了を知らせ、SASI はステータスフェーズへ移る。
    CHECK((m.ioRead8(kDmaChannel1 + 0x00) & 0x80) != 0);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
}

TEST_CASE("DMAC の転送方向は OCR の bit7 で決まる")
{
    // bit7 が立っていれば「デバイス → メモリ」。逆に取ると 1 バイトも
    // 転送されず、IPL-ROM の "X68K" 検査で必ず失敗する。
    x68k::Machine m;
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 1, 1);

    constexpr x68k::u32 kDmaChannel1 = x68k::kDmacBase + 0x40;
    constexpr x68k::u32 kDest = 0x00010000;

    m.ioWrite8(kDmaChannel1 + 0x00, 0xFF);
    // bit7 を落とす = メモリ → デバイス。READ 中なので転送は起きない。
    m.ioWrite8(kDmaChannel1 + 0x05, 0x32);
    m.ioWrite8(kDmaChannel1 + 0x0C, static_cast<x68k::u8>(kDest >> 24));
    m.ioWrite8(kDmaChannel1 + 0x0D, static_cast<x68k::u8>(kDest >> 16));
    m.ioWrite8(kDmaChannel1 + 0x0E, static_cast<x68k::u8>(kDest >> 8));
    m.ioWrite8(kDmaChannel1 + 0x0F, static_cast<x68k::u8>(kDest));
    m.ioWrite8(kDmaChannel1 + 0x0A, 0x01);
    m.ioWrite8(kDmaChannel1 + 0x0B, 0x00);
    m.ioWrite8(kDmaChannel1 + 0x07, 0x80);

    // 転送されていないので、データフェーズのまま。
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);
}
