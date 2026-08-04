// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: ディスクイメージ側の「起動可能の作法」を満たしたイメージが、
// IOCS を経由せずとも SASI から読み出せ、ブートコードとして実行できること。
//
// 実物の Human68k は著作物なのでリポジトリにもテストにも置けない。ここで
// 確かめるのは HUMAN.SYS の中身ではなく、tools/make_sasi_image.py が組む
// **イメージの構造** が満たすべき約束のほうである:
//
//   1. LBA 4 の先頭 4 バイトが "X68K"。IPL-ROM ($FF91FA の CMPI.L) が
//      ここを見て「起動できるディスク」と判定する
//   2. LBA 0 から 4 セクタ (1024 バイト) が $002000 へ読み込まれ実行される
//   3. その先頭バイトが $60 (BRA)。IPL-ROM ($FF02BE の CMPI.B #$60,(A1))
//      が検査し、違えば JMP せず次の起動デバイスへ行ってしまう
//
// LBA 4 が識別用で LBA 0 が実行される、という順序が直感に反する。ここを
// 取り違えると「イメージは出来ているのに起動しない」という、症状から
// 原因へ辿りにくい壊れ方をする。だから機械的に固定する。
//
// Why not 実物の Human68k イメージを読ませるか:
//   著作物であることに加え、配布物の置き場所 (/tmp など) に依存した
//   テストは他の環境で必ず落ちる。構造の約束だけならイメージを合成でき、
//   どの環境でも同じ結果になる。

#include <cstring>
#include <vector>

#include "doctest.h"
#include "machine.h"

namespace
{

// SASI の 1 セクタ。X68000 の SASI HDD は 256 バイト固定。
constexpr x68k::u32 kSectorSize = 256;

// tools/make_sasi_image.py と同じ配置。値を共有していないので、
// ツール側を変えたらこちらも変える必要がある (意図的に二重に書いている。
// 「気付かずに片方だけ変わる」ことをテストの失敗として検出したい)。
constexpr x68k::u32 kBootCodeLba = 0;
constexpr x68k::u32 kBootCodeSectors = 4;
constexpr x68k::u32 kBootIdLba = 4;

// メモリ上に持つだけのディスク。ファイル I/O を挟まないので、
// テストが実行環境のファイル配置に依存しない。
class MemoryDisk final : public x68k::DiskImage
{
public:
    explicit MemoryDisk(x68k::u32 sectorCount) : data_(sectorCount * kSectorSize, 0) {}

    // 指定 LBA へ生バイト列を置く。イメージを組み立てるための口。
    void poke(x68k::u32 lba, const std::vector<x68k::u8>& bytes)
    {
        const std::size_t offset = static_cast<std::size_t>(lba) * kSectorSize;
        REQUIRE(offset + bytes.size() <= data_.size());
        std::memcpy(data_.data() + offset, bytes.data(), bytes.size());
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
        lastReadLba = lba;
        lastReadCount = sectorCount;
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
        return true;
    }

    x68k::u32 lastReadLba = 0;
    x68k::u32 lastReadCount = 0;
    int readCount = 0;

private:
    std::vector<x68k::u8> data_;
};

// 起動可能なイメージを合成する。make_sasi_image.py の構造だけを真似る。
class BootableImage
{
public:
    static constexpr x68k::u32 kSectorCount = 0xA000;  // IPL-ROM の下限 $9FD9 を超える大きさ

    BootableImage() : disk(kSectorCount)
    {
        writeIdSector();
        writeBootCode();
    }

    MemoryDisk disk;

private:
    // LBA 4: 識別子。IPL-ROM が最初に読んで検査する。
    void writeIdSector()
    {
        std::vector<x68k::u8> sector(kSectorSize, 0);
        sector[0] = 'X';
        sector[1] = '6';
        sector[2] = '8';
        sector[3] = 'K';
        // +$08 に総セクタ数。IPL-ROM が $FF920A で読み、諸元テーブルを選ぶ。
        sector[8] = static_cast<x68k::u8>(kSectorCount >> 24);
        sector[9] = static_cast<x68k::u8>(kSectorCount >> 16);
        sector[10] = static_cast<x68k::u8>(kSectorCount >> 8);
        sector[11] = static_cast<x68k::u8>(kSectorCount);
        disk.poke(kBootIdLba, sector);
    }

    // LBA 0: $002000 へ読み込まれて実行されるコード。
    void writeBootCode()
    {
        std::vector<x68k::u8> code(kBootCodeSectors * kSectorSize, 0);
        // BRA.W *+4 。先頭が $60 でなければ IPL-ROM は実行してくれない。
        code[0] = 0x60;
        code[1] = 0x00;
        code[2] = 0x00;
        code[3] = 0x02;
        // 目印を $002100 へ書く MOVE.W #$1234,$00002100 。
        code[4] = 0x33;
        code[5] = 0xFC;
        code[6] = 0x12;
        code[7] = 0x34;
        code[8] = 0x00;
        code[9] = 0x00;
        code[10] = 0x21;
        code[11] = 0x00;
        // BRA 自分自身。暴走して別の場所を壊さないよう、ここで止める。
        code[12] = 0x60;
        code[13] = 0xFE;
        disk.poke(kBootCodeLba, code);
    }
};

}  // namespace

TEST_CASE("識別子セクタが LBA 4 にあり \"X68K\" で始まる")
{
    BootableImage image;
    std::vector<x68k::u8> buffer(kSectorSize, 0);
    REQUIRE(image.disk.readSector(kBootIdLba, buffer.data(), 1));

    // IPL-ROM $FF91FA の CMPI.L が見るのはこの 4 バイト。
    CHECK(buffer[0] == 'X');
    CHECK(buffer[1] == '6');
    CHECK(buffer[2] == '8');
    CHECK(buffer[3] == 'K');
}

TEST_CASE("ブートコードの先頭が $60 (BRA) である")
{
    BootableImage image;
    std::vector<x68k::u8> buffer(kBootCodeSectors * kSectorSize, 0);
    REQUIRE(image.disk.readSector(kBootCodeLba, buffer.data(), kBootCodeSectors));

    // IPL-ROM $FF02BE の CMPI.B #$60,(A1)。壊れたセクタを実行しないための約束。
    CHECK(buffer[0] == 0x60);
}

TEST_CASE("識別子は LBA 4、実行されるのは LBA 0 で、両者は別のセクタ")
{
    // 順序が直感に反するので、取り違えを明示的に固定する。
    // 「識別子のあるセクタを実行する」と誤解すると、$5836384B ("X68K") を
    // 命令として実行して即座に暴走する。
    CHECK(kBootIdLba != kBootCodeLba);

    BootableImage image;
    std::vector<x68k::u8> id(kSectorSize, 0);
    std::vector<x68k::u8> boot(kSectorSize, 0);
    REQUIRE(image.disk.readSector(kBootIdLba, id.data(), 1));
    REQUIRE(image.disk.readSector(kBootCodeLba, boot.data(), 1));

    // 識別子セクタは実行できる形をしていない (先頭が $60 ではない)。
    CHECK(id[0] != 0x60);
    CHECK(boot[0] == 0x60);
}

TEST_CASE("$002000 へ読み込んだブートコードが実行できる")
{
    // IPL-ROM そのものは使わず、ROM がやることを手で行う:
    // ブートコードを $002000 へ置き、そこから実行する。IPL-ROM を
    // 持たない環境でも「ディスク上のコードが動く形になっているか」を
    // 確かめられる。
    BootableImage image;

    std::vector<x68k::u8> mainRam(x68k::kMainRamSize, 0);
    std::vector<x68k::u8> textVram(x68k::kTvramSize, 0);
    std::vector<x68k::u8> iplRom(x68k::kIplromSize, 0xFF);

    // リセットベクタ。SSP はブートコードの真下に置く。
    // $FF0000 は ROM 内オフセット 0x10000。
    const std::size_t ff0000 = 0xFF0000u - x68k::kIplromBase;
    const x68k::u32 kEntry = 0x002000;
    iplRom[ff0000 + 0] = 0x00;
    iplRom[ff0000 + 1] = 0x00;
    iplRom[ff0000 + 2] = 0x20;
    iplRom[ff0000 + 3] = 0x00;
    iplRom[ff0000 + 4] = 0x00;
    iplRom[ff0000 + 5] = 0xFF;
    iplRom[ff0000 + 6] = 0x00;
    iplRom[ff0000 + 7] = 0x10;
    // $FF0010: ROM の写像を解除してから $002000 へ飛ぶ。
    // CLR.B $E86001 — 写像したままだと $002000 の読み出しが ROM 側へ行く。
    std::size_t pc = ff0000 + 0x10;
    const x68k::u8 prologue[] = {
        0x42, 0x39, 0x00, 0xE8, 0x60, 0x01,  // CLR.B $00E86001
        0x4E, 0xF9, 0x00, 0x00, 0x20, 0x00,  // JMP $00002000
    };
    std::memcpy(iplRom.data() + pc, prologue, sizeof(prologue));

    // ディスクから $002000 へ読み込む。IPL-ROM の SASI 転送に相当する部分を
    // ホスト側で行い、ここでは「読み込んだ後に実行できるか」だけを見る。
    REQUIRE(image.disk.readSector(kBootCodeLba, mainRam.data() + kEntry, kBootCodeSectors));
    CHECK(mainRam[kEntry] == 0x60);

    x68k::Machine machine;
    x68k::MemoryMap memory;
    memory.mainRam = mainRam.data();
    memory.textVram = textVram.data();
    memory.iplRom = iplRom.data();
    machine.setMemory(memory);
    machine.reset();

    machine.run(10000);

    // ブートコードが $002100 へ書いた目印を確かめる。
    // これが立っていれば、ディスク上のバイト列が命令として実行された。
    const x68k::u16 mark = static_cast<x68k::u16>((mainRam[0x2100] << 8) | mainRam[0x2101]);
    CHECK(mark == 0x1234);
    CHECK_FALSE(machine.isHalted());
}
