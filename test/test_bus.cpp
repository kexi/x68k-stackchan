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

// $000000 へ写像される ROM の、ROM 内でのオフセット。
// 写像元は $FF0000 側 (IPL-ROM 128KB の後半 64KB)。
constexpr std::size_t RomOffsetOfFF0000()
{
    return 0xFF0000u - x68k::kIplromBase;
}

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
    // 工場出荷状態のマジックがバス経由でも見える (先頭は全角「Ｘ」の 1 バイト目)。
    CHECK(f.bus.read8(x68k::kSramBase) == 0x82);

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

// --- バスエラー --------------------------------------------------------------
//
// X68000 の IPL-ROM は「バスエラーベクタを差し替えてから読みに行き、
// エラーが起きれば装置が無い」という方法で SCSI ROM ($FC0000) の有無を
// 調べる ($FF0236)。ここで 0 を返してしまうと「ROM がある」ことになり、
// その先頭を JSR で呼んで暴走する。
//
// バスエラーは「起きること」と「起きないこと」の両方が同じくらい重要で、
// どちらを間違えても起動しない。

TEST_CASE("応答しない領域の読み出しはバスエラーになる")
{
    // 保証すること: 何も繋がっていないアドレスが 0 ではなくエラーを返すこと。
    //
    // 壊れると: IPL-ROM が SCSI ROM が有ると誤認し、
    // 中身の無いアドレスを JSR で呼んで暴走する。
    Fixture f;
    f.bus.setRomMappedAtZero(false);

    f.bus.read8(0xFC0000);
    CHECK(f.bus.lastAccessFaulted());
}

TEST_CASE("read16 でもバスエラーが立つ")
{
    // 保証すること: ワード読みでも同じ判定になること。
    // IPL-ROM の検査はワードで読みに来る。
    //
    // 壊れると: バイト読みだけ守られていて、実際に使われるワード読みが
    // すり抜ける。
    Fixture f;
    f.bus.setRomMappedAtZero(false);

    f.bus.read16(0xFC0000);
    CHECK(f.bus.lastAccessFaulted());
}

TEST_CASE("read32 相当が read16 2 回に分かれてもエラーが伝わる")
{
    // 保証すること: 上位ワードでエラーが起きたとき、下位ワードの読み出しが
    // 成功してもエラーが消えないこと。
    //
    // 壊れると: faulted_ が後の成功で上書きされ、エラーが握りつぶされる。
    // 「装置が無い」ことを検出できなくなる。
    Fixture f;
    f.bus.setRomMappedAtZero(false);

    // 上位ワードは応答しない領域、下位ワードも同じ領域。
    f.bus.read16(0xFC0000);
    const bool hiFaulted = f.bus.lastAccessFaulted();
    f.bus.read16(0xFC0002);
    const bool loFaulted = f.bus.lastAccessFaulted();
    CHECK(hiFaulted);
    CHECK(loFaulted);
}

TEST_CASE("ワードが応答する領域とまたぐときもエラーが伝わる")
{
    // 保証すること: read16 が read8 2 回に分かれる経路で、
    // 片方だけがエラーでもワード全体がエラーになること。
    //
    // 壊れると: 2 回目の read8 が faulted_ を false に戻し、
    // 直前のエラーが消える。境界のすぐ手前だけ検出できなくなる。
    Fixture f;
    f.bus.setRomMappedAtZero(false);

    // $FDFFFF は応答しない領域、$FE0000 は IPL-ROM。
    // read16 は read8(FDFFFF) と read8(FE0000) に分かれる。
    f.bus.read16(0xFDFFFF);
    CHECK(f.bus.lastAccessFaulted());
}

TEST_CASE("応答する領域の読み出しではバスエラーが立たない")
{
    // 保証すること: RAM / ROM / SRAM / VRAM / I/O のどれもエラーにしないこと。
    //
    // 壊れると: 正常なアクセスでバスエラー例外が起き、起動が進まない。
    // 「応答しない領域を検出する」の裏返しで、こちらの方が症状が派手。
    Fixture f;
    f.bus.setRomMappedAtZero(false);

    f.bus.read8(0x000000);  // メイン RAM
    CHECK_FALSE(f.bus.lastAccessFaulted());

    f.bus.read8(x68k::kIplromBase);  // IPL-ROM
    CHECK_FALSE(f.bus.lastAccessFaulted());

    f.bus.read8(x68k::kSramBase);  // SRAM
    CHECK_FALSE(f.bus.lastAccessFaulted());

    f.bus.read8(x68k::kTvramBase);  // テキスト VRAM
    CHECK_FALSE(f.bus.lastAccessFaulted());

    f.bus.read8(x68k::kMfpBase + 1);  // I/O
    CHECK_FALSE(f.bus.lastAccessFaulted());

    f.bus.read8(x68k::kGvramBase);  // グラフィック VRAM
    CHECK_FALSE(f.bus.lastAccessFaulted());
}

TEST_CASE("エラーの後で正常なアクセスをするとフラグが下りる")
{
    // 保証すること: faulted_ が「直前のアクセス」の結果だけを表すこと。
    //
    // 壊れると: 一度エラーが起きた後すべてのアクセスがエラー扱いになり、
    // CPU がバスエラー例外から抜けられなくなる。
    Fixture f;
    f.bus.setRomMappedAtZero(false);

    f.bus.read8(0xFC0000);
    CHECK(f.bus.lastAccessFaulted());

    f.bus.read16(x68k::kIplromBase);
    CHECK_FALSE(f.bus.lastAccessFaulted());
}

TEST_CASE("メモリが未設定でも応答する領域はエラーにしない")
{
    // 保証すること: 「実体がまだ無い」と「アドレスが繋がっていない」を
    // 区別すること。前者は 0 を返し、後者だけエラーにする。
    //
    // 壊れると: グラフィック VRAM を確保しない構成で起動できなくなる。
    x68k::Sram sram;
    RecordingIo io;
    x68k::SystemBus bus(x68k::MemoryMap{}, sram, io);
    bus.setRomMappedAtZero(false);

    CHECK(bus.read8(x68k::kGvramBase) == 0);
    CHECK_FALSE(bus.lastAccessFaulted());

    CHECK(bus.read8(x68k::kTvramBase) == 0);
    CHECK_FALSE(bus.lastAccessFaulted());

    CHECK(bus.read8(x68k::kCgromBase) == 0);
    CHECK_FALSE(bus.lastAccessFaulted());
}

TEST_CASE("グラフィック VRAM は 512KB で折り返す")
{
    // 保証すること: アドレス空間は 2MB あるが実 VRAM は 512KB なので、
    // 折り返して同じ場所が見えること。
    //
    // 壊れると: 配列外アクセスになる。ESP32 では PSRAM の別の領域を
    // 壊すので、症状が全く関係ない場所に出る。
    std::vector<x68k::u8> gvram(x68k::kTvramSize, 0);
    x68k::Sram sram;
    RecordingIo io;
    x68k::SystemBus bus(x68k::MemoryMap{}, sram, io);
    x68k::MemoryMap memory;
    memory.graphicVram = gvram.data();
    bus.setMemory(memory);

    bus.write8(x68k::kGvramBase, 0x42);
    // 512KB 先は同じ場所を指す。
    CHECK(bus.read8(x68k::kGvramBase + x68k::kTvramSize) == 0x42);
    CHECK_FALSE(bus.lastAccessFaulted());
}

TEST_CASE("$000000 への ROM 写像は 64KB ぶんだけ")
{
    // 保証すること: 写像されるのは $FF0000 からの 64KB で、
    // それより上のメイン RAM は写像中でも RAM が見えること。
    //
    // 壊れると: 写像の範囲を取り違え、IPL-ROM が RAM を使い始めた途端に
    // ROM の中身を読んでしまう。
    Fixture f;
    f.iplRom[0] = 0xEE;
    f.mainRam[0x90000] = 0x77;
    f.bus.setRomMappedAtZero(true);

    // 写像の外なので RAM が見える。
    CHECK(f.bus.read8(0x90000) == 0x77);
}

TEST_CASE("ROM 写像中でも書き込みは RAM 側へ行く")
{
    // 保証すること: 写像は読み出しだけに効くこと。ROM は書けない。
    //
    // 壊れると: 起動直後にスタックへ積んだ値が消える
    // (SSP は $002000 で写像範囲の中にある)。
    Fixture f;
    f.bus.setRomMappedAtZero(true);
    f.iplRom[RomOffsetOfFF0000() + 0x2000] = 0xEE;

    f.bus.write8(0x2000, 0x55);
    // 読むと写像された ROM が見える。
    CHECK(f.bus.read8(0x2000) == 0xEE);
    // 写像を外すと書いた値が残っている。
    f.bus.setRomMappedAtZero(false);
    CHECK(f.bus.read8(0x2000) == 0x55);
}
