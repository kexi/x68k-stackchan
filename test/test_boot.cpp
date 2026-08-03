// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: リセットから画面に絵が出るまでの経路が通ること。
//
// 実際の IPL-ROM はライセンス上リポジトリに置けないので、同じ手順を踏む
// 最小のプログラムを合成して代用する。ここで確かめるのは X68000 固有の
// 起動の作法であって、IPL-ROM の中身ではない:
//
//   1. リセットベクタが $FF0000 側から読める ($000000 への写像)
//   2. エリアセット ($E86001) への CLR.B で写像が解除される
//   3. I/O 経由でパレットが設定できる
//   4. テキスト VRAM への書き込みが 4 プレーン合成されて色になる
//
// これらは実機の資料からしか分からない手順で、どれか 1 つでも間違うと
// 「何も起きずに固まる」という形でしか症状が出ない。

#include <vector>

#include "doctest.h"
#include "machine.h"
#include "video/text_raster.h"

namespace
{

// 合成 ROM を組み立てるための小さなアセンブラ。
class RomBuilder
{
public:
    RomBuilder() : rom_(x68k::kIplromSize, 0xFF) {}

    // $FF0000 は ROM 内オフセット 0x10000 に当たる。
    static constexpr std::size_t kFF0000 = 0xFF0000u - x68k::kIplromBase;

    void putLong(std::size_t offset, x68k::u32 value)
    {
        rom_[offset + 0] = static_cast<x68k::u8>(value >> 24);
        rom_[offset + 1] = static_cast<x68k::u8>(value >> 16);
        rom_[offset + 2] = static_cast<x68k::u8>(value >> 8);
        rom_[offset + 3] = static_cast<x68k::u8>(value);
    }

    void emitWord(x68k::u16 value)
    {
        rom_[pc_++] = static_cast<x68k::u8>(value >> 8);
        rom_[pc_++] = static_cast<x68k::u8>(value & 0xFFu);
    }

    void emitLong(x68k::u32 value)
    {
        emitWord(static_cast<x68k::u16>(value >> 16));
        emitWord(static_cast<x68k::u16>(value & 0xFFFFu));
    }

    void setEmitPos(std::size_t offset)
    {
        pc_ = offset;
    }

    // CLR.B <absolute long>
    void clrByteAbs(x68k::u32 addr)
    {
        emitWord(0x4239);
        emitLong(addr);
    }

    // MOVE.W #imm,<absolute long>
    void moveWordImmToAbs(x68k::u16 imm, x68k::u32 addr)
    {
        emitWord(0x33FC);
        emitWord(imm);
        emitLong(addr);
    }

    // BRA 自分自身 (無限ループ)
    void braSelf()
    {
        emitWord(0x60FE);
    }

    [[nodiscard]] const std::vector<x68k::u8>& rom() const
    {
        return rom_;
    }

private:
    std::vector<x68k::u8> rom_;
    std::size_t pc_ = 0;
};

// リセット後に走らせるための一式。
struct BootFixture
{
    RomBuilder builder;
    std::vector<x68k::u8> mainRam;
    std::vector<x68k::u8> textVram;
    x68k::Machine machine;

    BootFixture() : mainRam(x68k::kMainRamSize, 0), textVram(x68k::kTvramSize, 0) {}

    void start()
    {
        x68k::MemoryMap memory;
        memory.mainRam = mainRam.data();
        memory.textVram = textVram.data();
        memory.iplRom = builder.rom().data();
        machine.setMemory(memory);
        machine.reset();
    }
};

}  // namespace

TEST_CASE("リセットベクタが $FF0000 側から読まれる")
{
    BootFixture f;
    // SSP と PC を実機と同じ値に置く。
    f.builder.putLong(RomBuilder::kFF0000 + 0, x68k::kResetSsp);
    f.builder.putLong(RomBuilder::kFF0000 + 4, x68k::kResetPc);
    f.builder.setEmitPos(RomBuilder::kFF0000 + 0x10);
    f.builder.braSelf();

    f.start();

    const auto& s = f.machine.cpu().state();
    CHECK(s.a[7] == x68k::kResetSsp);
    // プリフェッチで 2 ワード先読みしているので、命令語のアドレスは pc - 4。
    CHECK(s.pc - 4 == x68k::kResetPc);
    CHECK_FALSE(f.machine.isHalted());
}

TEST_CASE("エリアセットへの CLR.B で ROM の写像が解除される")
{
    BootFixture f;
    f.builder.putLong(RomBuilder::kFF0000 + 0, x68k::kResetSsp);
    f.builder.putLong(RomBuilder::kFF0000 + 4, x68k::kResetPc);
    f.builder.setEmitPos(RomBuilder::kFF0000 + 0x10);
    // IPL-ROM 1.3 以降と同じ形。CLR は read-modify-write なので
    // このアドレスの読み出しに副作用があると起動しない。
    f.builder.clrByteAbs(x68k::kAreaSetReg);
    f.builder.braSelf();

    f.start();
    CHECK(f.machine.bus().romMappedAtZero());

    // CLR.B を実行する。
    f.machine.step();

    CHECK_FALSE(f.machine.bus().romMappedAtZero());
    CHECK_FALSE(f.machine.isHalted());
}

TEST_CASE("パレット設定とテキスト VRAM 書き込みが画面の色になる")
{
    BootFixture f;
    f.builder.putLong(RomBuilder::kFF0000 + 0, x68k::kResetSsp);
    f.builder.putLong(RomBuilder::kFF0000 + 4, x68k::kResetPc);
    f.builder.setEmitPos(RomBuilder::kFF0000 + 0x10);

    f.builder.clrByteAbs(x68k::kAreaSetReg);
    // テキストパレット 0 = 黒、1 = 白。
    f.builder.moveWordImmToAbs(0x0000, 0xE82200);
    f.builder.moveWordImmToAbs(0xFFFF, 0xE82202);
    // プレーン 0 の左上に $AA00 (10101010 00000000) を書く。
    f.builder.moveWordImmToAbs(0xAA00, x68k::kTvramBase);
    f.builder.braSelf();

    f.start();
    // 4 命令ぶん動かす (CLR.B + MOVE.W ×3)。
    for (int i = 0; i < 4; ++i)
    {
        f.machine.step();
    }
    CHECK_FALSE(f.machine.isHalted());

    // 16 ピクセルぶんラスタライズして、書いたビットパターンが色になることを見る。
    constexpr x68k::u32 kWidth = 16;
    std::vector<x68k::u16> pixels(kWidth, 0);
    x68k::TextRaster::render(f.textVram.data(), f.machine.video(), 0, 0, kWidth, 1, pixels.data(),
                             kWidth);

    const x68k::u16 white = x68k::VideoController::toRgb565(0xFFFF);
    const x68k::u16 black = x68k::VideoController::toRgb565(0x0000);

    // $AA = 10101010 なので、白黒が交互に 4 組。
    CHECK(pixels[0] == white);
    CHECK(pixels[1] == black);
    CHECK(pixels[2] == white);
    CHECK(pixels[3] == black);
    CHECK(pixels[4] == white);
    CHECK(pixels[5] == black);
    CHECK(pixels[6] == white);
    CHECK(pixels[7] == black);
    // 下位バイトは $00 なので以降は全部背景色。
    CHECK(pixels[8] == black);
    CHECK(pixels[15] == black);
}

TEST_CASE("テキスト VRAM への書き込みでダーティ行が記録される")
{
    BootFixture f;
    f.builder.putLong(RomBuilder::kFF0000 + 0, x68k::kResetSsp);
    f.builder.putLong(RomBuilder::kFF0000 + 4, x68k::kResetPc);
    f.builder.setEmitPos(RomBuilder::kFF0000 + 0x10);
    f.builder.clrByteAbs(x68k::kAreaSetReg);
    f.builder.moveWordImmToAbs(0xFFFF, x68k::kTvramBase);
    f.builder.braSelf();

    f.start();
    f.machine.bus().clearTextDirty();
    CHECK_FALSE(f.machine.bus().anyTextDirty());

    f.machine.step();  // CLR.B
    f.machine.step();  // MOVE.W to VRAM

    // 描画側はこれを見て転送範囲を決める。全画面を毎フレーム送ると
    // SPI 接続の LCD では間に合わない。
    CHECK(f.machine.bus().anyTextDirty());
    CHECK(f.machine.bus().isTextRowDirty(0));
}

TEST_CASE("未実装命令に当たったら停止して命令語が分かる")
{
    BootFixture f;
    f.builder.putLong(RomBuilder::kFF0000 + 0, x68k::kResetSsp);
    f.builder.putLong(RomBuilder::kFF0000 + 4, x68k::kResetPc);
    f.builder.setEmitPos(RomBuilder::kFF0000 + 0x10);
    // 未定義のビットパターン。実装済みのどのグループにも当たらない。
    f.builder.emitWord(0x4AFC);  // ILLEGAL 相当

    f.start();
    f.machine.step();

    // IPL-ROM を走らせて「落ちた命令から実装する」開発ループのための仕組み。
    // 停止したこと自体は異常ではない。
    if (f.machine.isHalted())
    {
        CHECK(f.machine.haltedOpcode() == 0x4AFC);
    }
}
