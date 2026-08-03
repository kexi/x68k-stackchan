// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: アドレスデコードが各領域を正しく振り分けること。
//
// バスは 68000 が命令をフェッチするたびに通る経路で、ここが間違っていると
// 「なぜか変な命令を実行している」という形でしか症状が出ない。境界値を押さえる。

#include <vector>

#include "bus.h"
#include "doctest.h"

namespace
{

// I/O アクセスを記録するだけのハンドラ。
class RecordingIo final : public x68k::IoHandler
{
public:
    x68k::u32 lastReadAddr = 0;
    x68k::u32 lastWriteAddr = 0;
    x68k::u8 lastWriteValue = 0;
    int readCount = 0;
    int writeCount = 0;

    x68k::u8 ioRead8(x68k::u32 addr) override
    {
        lastReadAddr = addr;
        ++readCount;
        return 0x5A;
    }

    void ioWrite8(x68k::u32 addr, x68k::u8 value) override
    {
        lastWriteAddr = addr;
        lastWriteValue = value;
        ++writeCount;
    }

    x68k::u16 ioRead16(x68k::u32 addr) override
    {
        lastReadAddr = addr;
        ++readCount;
        return 0x5A5A;
    }

    void ioWrite16(x68k::u32 addr, x68k::u16 value) override
    {
        lastWriteAddr = addr;
        lastWriteValue = static_cast<x68k::u8>(value);
        ++writeCount;
    }
};

// テスト用の一式。
struct Fixture
{
    std::vector<x68k::u8> mainRam;
    std::vector<x68k::u8> textVram;
    std::vector<x68k::u8> iplRom;
    x68k::Sram sram;
    RecordingIo io;
    x68k::SystemBus bus;

    Fixture()
        : mainRam(x68k::kMainRamSize, 0),
          textVram(x68k::kTvramSize, 0),
          iplRom(x68k::kIplromSize, 0),
          bus(x68k::MemoryMap{}, sram, io)
    {
        x68k::MemoryMap memory;
        memory.mainRam = mainRam.data();
        memory.textVram = textVram.data();
        memory.iplRom = iplRom.data();
        bus.setMemory(memory);
    }
};

}  // namespace

TEST_CASE("リセット直後は $FF0000 の内容が $000000 に見える")
{
    Fixture f;
    // 写像されるのは ROM の先頭 ($FE0000) ではなく $FF0000 側。
    // リセットベクタはそこにあり、実行は $FF0010 から始まる。
    // ここを取り違えるとベクタが読めず即座に暴走する。
    constexpr std::size_t kRomOffsetOfFF0000 = 0xFF0000u - x68k::kIplromBase;
    f.iplRom[kRomOffsetOfFF0000 + 0] = 0x00;
    f.iplRom[kRomOffsetOfFF0000 + 1] = 0x00;
    f.iplRom[kRomOffsetOfFF0000 + 2] = 0x20;
    f.iplRom[kRomOffsetOfFF0000 + 3] = 0x00;
    // ROM 先頭 ($FE0000) には別の値を置き、そちらが読まれないことを確かめる。
    f.iplRom[0] = 0xEE;
    f.mainRam[0] = 0xFF;

    f.bus.setRomMappedAtZero(true);
    CHECK(f.bus.read8(0x000000) == 0x00);
    CHECK(f.bus.read8(0x000002) == 0x20);
    CHECK(f.bus.read16(0x000002) == 0x2000);
    // 32bit のリセットベクタとして読める。
    CHECK(f.bus.read16(0x000000) == 0x0000);
}

TEST_CASE("エリアセット後は $000000 が RAM を指す")
{
    Fixture f;
    f.iplRom[0] = 0xAA;
    f.mainRam[0] = 0x55;

    f.bus.setRomMappedAtZero(false);
    CHECK(f.bus.read8(0x000000) == 0x55);
}

TEST_CASE("IPL-ROM は $FE0000 から読める")
{
    Fixture f;
    f.iplRom[0x10] = 0x4E;
    f.iplRom[0x11] = 0x71;

    CHECK(f.bus.read8(0xFE0010) == 0x4E);
    CHECK(f.bus.read16(0xFE0010) == 0x4E71);
    // リセット後の実行開始点。
    CHECK(f.bus.read16(x68k::kResetPc) == f.bus.read16(0xFF0010));
}

TEST_CASE("ROM 領域への書き込みは無視される")
{
    Fixture f;
    f.iplRom[0x20] = 0x12;
    f.bus.write8(0xFE0020, 0xFF);
    CHECK(f.bus.read8(0xFE0020) == 0x12);
}

TEST_CASE("SRAM は $ED0000 から読み書きできる")
{
    Fixture f;
    // 工場出荷状態のマジックがバス経由でも見える。
    CHECK(f.bus.read8(x68k::kSramBase) == 'X');

    f.bus.write8(x68k::kSramBase + 0x100, 0x42);
    CHECK(f.bus.read8(x68k::kSramBase + 0x100) == 0x42);
}

TEST_CASE("テキスト VRAM の 4 プレーンがそれぞれ独立に読み書きできる")
{
    Fixture f;
    for (x68k::u32 plane = 0; plane < x68k::kTvramPlaneCount; ++plane)
    {
        const x68k::u32 addr = x68k::kTvramBase + plane * x68k::kTvramPlaneSize;
        f.bus.write8(addr, static_cast<x68k::u8>(0x10 + plane));
    }
    for (x68k::u32 plane = 0; plane < x68k::kTvramPlaneCount; ++plane)
    {
        const x68k::u32 addr = x68k::kTvramBase + plane * x68k::kTvramPlaneSize;
        CHECK(f.bus.read8(addr) == 0x10 + plane);
    }
}

TEST_CASE("テキスト VRAM への書き込みでダーティ行が立つ")
{
    Fixture f;
    f.bus.clearTextDirty();
    CHECK_FALSE(f.bus.anyTextDirty());

    // ライン 0 (タイル行 0) への書き込み。
    f.bus.write8(x68k::kTvramBase, 0xFF);
    CHECK(f.bus.isTextRowDirty(0));

    // ライン 100 は 1 ライン 128 バイトなのでオフセット 12800。タイル行 6。
    f.bus.clearTextDirty();
    f.bus.write8(x68k::kTvramBase + 100 * x68k::kTvramBytesPerLine, 0xFF);
    CHECK(f.bus.isTextRowDirty(100 / x68k::SystemBus::kDirtyTileHeight));
    CHECK_FALSE(f.bus.isTextRowDirty(0));
}

TEST_CASE("I/O 空間へのアクセスがハンドラへ渡る")
{
    Fixture f;
    const x68k::u8 value = f.bus.read8(x68k::kMfpBase + 1);
    CHECK(value == 0x5A);
    CHECK(f.io.lastReadAddr == x68k::kMfpBase + 1);

    f.bus.write8(x68k::kCrtcBase, 0x33);
    CHECK(f.io.lastWriteAddr == x68k::kCrtcBase);
    CHECK(f.io.lastWriteValue == 0x33);
}

TEST_CASE("未実装領域の読み出しは 0 を返しバスエラーにしない")
{
    Fixture f;
    // IPL-ROM と IOCS は存在しないデバイスも初期化しに来る。
    // ここでバスエラーにすると起動が進まない。
    CHECK(f.bus.read8(0x900000) == 0);
    CHECK(f.bus.read16(0xA00000) == 0);
    f.bus.write8(0x900000, 0xFF);  // 落ちないこと
}

TEST_CASE("アドレスは 24bit に折り返される")
{
    Fixture f;
    f.iplRom[0x30] = 0x77;
    // 上位 8bit は 68000 が出力しないので無視される。
    CHECK(f.bus.read8(0xFFFE0030) == 0x77);
    CHECK(f.bus.read8(0x00FE0030) == 0x77);
}
