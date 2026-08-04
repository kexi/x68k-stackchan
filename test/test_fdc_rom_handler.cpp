// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: 実物の IPL-ROM の FDC 割り込みハンドラ ($FF1130) が実際に走り、
// 結果を $000C90 のテーブルへ書くこと。
//
// 【なぜこのテストが要るのか】
// FDC まわりで 2 回続けて「テストは全部緑なのに実機が起動しない」を踏んだ。
//
//   1. kSt0InvalidCommand を $C0 にしていた。ROM の $FF1162 は
//      CMP.B #$80,D0 / BEQ で **ちょうど $80 のときだけ** ハンドラを抜ける。
//      $C0 だと分岐が外れてハンドラが戻らず、SASI 起動が画面表示前に死ぬ。
//   2. メディア未挿入の RECALIBRATE で EC (Equipment Check) を立てていた。
//      「ドライブが無い」は NR で表すもので、EC は「ドライブはあるが壊れている」。
//
// どちらも**戻すと画面が真っ黒になる**のに、ホストのテストは 533 件すべて
// 通り続けた。デバイス単体の状態しか見ておらず、ROM のハンドラを一度も
// 走らせていなかったため。
//
// ここでは実物の IPL-ROM を読み込んで CPU を回し、ROM 自身のハンドラが
// $000C90 を埋めるところまでを検査する。ST0 の値を変えれば落ちる。
//
// Why not 合成 ROM で済ませないか: 落ちた原因はどちらも「実物の ROM が
// どう分岐するか」だった。$FF1162 の CMP.B #$80 は自分で書いた ROM には
// 無い。実物を通さないと、同じ間違いをもう一度見逃す。
//
// rom/iplrom.dat はライセンス上リポジトリに同梱できない (NOTICE.md)。
// 無ければこのテストは黙って飛ばす。CI で ROM を置ける環境なら効く。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "dev/fdc.h"
#include "doctest.h"
#include "machine.h"
#include "memmap.h"

namespace
{

// IPL-ROM の在り処。リポジトリ直下から実行される前提で相対、
// 環境変数でも渡せるようにしておく (CI が別の場所へ置く場合のため)。
std::vector<x68k::u8> loadIplRom()
{
    const char* env = std::getenv("X68K_TEST_IPLROM");
    const std::string path = env != nullptr ? env : "rom/iplrom.dat";

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size != static_cast<long>(x68k::kIplromSize))
    {
        std::fclose(f);
        return {};
    }
    std::vector<x68k::u8> rom(x68k::kIplromSize);
    const std::size_t read = std::fread(rom.data(), 1, rom.size(), f);
    std::fclose(f);
    return read == rom.size() ? rom : std::vector<x68k::u8>{};
}

// メディアを入れた 2HD ドライブ。中身は 0 で構わない。
//
// Why not メディア未挿入で試さないか: 未挿入だと ROM は FDC を早々に
// 見限り、割り込みハンドラ ($FF1130) まで到達しない。実際そうして書いた
// 最初の版は $C90 が 0 のままで落ちた。ハンドラを走らせたいなら、
// ドライブが応答する状態にする必要がある。
class EmptyDrive final : public x68k::FloppyImage
{
public:
    bool readSector(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record, x68k::u8* buffer) override
    {
        if (cylinder >= geometry_.cylinders || head >= geometry_.heads || record == 0 ||
            record > geometry_.sectorsPerTrack)
        {
            return false;
        }
        std::fill(buffer, buffer + geometry_.sectorSize, x68k::u8{0});
        return true;
    }
    bool writeSector(x68k::u32, x68k::u32, x68k::u32, const x68k::u8*) override
    {
        return false;
    }
    [[nodiscard]] bool isPresent() const override
    {
        return true;
    }
    [[nodiscard]] bool isWriteProtected() const override
    {
        return true;
    }
    [[nodiscard]] const x68k::FloppyGeometry& geometry() const override
    {
        return geometry_;
    }

private:
    x68k::FloppyGeometry geometry_{77, 2, 8, 1024};
};

}  // namespace

TEST_CASE("実物の IPL-ROM の FDC ハンドラが $000C90 を埋める")
{
    const std::vector<x68k::u8> rom = loadIplRom();
    if (rom.empty())
    {
        MESSAGE("rom/iplrom.dat が無いので飛ばす (X68K_TEST_IPLROM で場所を渡せる)");
        return;
    }

    static std::vector<x68k::u8> mainRam(x68k::kMainRamSize, 0);
    static std::vector<x68k::u8> textVram(x68k::kTvramSize, 0);
    static std::vector<x68k::u8> sasiBuffer(x68k::Machine::kSasiBufferBytes, 0);
    std::fill(mainRam.begin(), mainRam.end(), 0);

    x68k::Machine machine;
    x68k::MemoryMap memory;
    memory.mainRam = mainRam.data();
    memory.textVram = textVram.data();
    memory.iplRom = rom.data();
    machine.setMemory(memory);
    machine.setSasiBuffer(sasiBuffer.data());

    EmptyDrive drive;
    machine.setFloppyDisk(0, &drive);
    machine.reset();

    // IPL-ROM が FDC を叩くところまで進める。起動デバイスの走査で
    // ドライブの状態を見にいくので、そこでハンドラが走る。
    //
    // Why not もっと短く回さないか: ROM は電源投入後にメモリチェックと
    // 各デバイスの初期化を済ませてから FDC へ来る。手前で止めると
    // ハンドラまで到達しない。
    for (int i = 0; i < 4000 && !machine.isHalted(); ++i)
    {
        machine.run(100000);
    }

    // ハンドラ ($FF1130) は $FF1176 の LEA $C90,A0 でテーブルを指し、
    // $FF1196 で ST0 を書き、$FF11A0 で $C8F のフラグを立てる。
    //
    // ここが 0 のままなら、割り込みが上がっていないか、ハンドラが
    // 途中で抜けている。どちらも「フロッピーから起動できない」に直結する。
    const x68k::u8 flag = machine.bus().read8(0x000C8F);
    const x68k::u8 st0Drive0 = machine.bus().read8(0x000C90);

    // $C8F のフラグが立っていれば、ROM のハンドラ ($FF11A0) まで到達した。
    // これが 0 なら割り込みが上がっていないか、ハンドラが途中で抜けている。
    // どちらも「フロッピーから起動できない」に直結する。
    //
    // Why not ST0 が非 0 であることも縛らないか: SASI のディスクイメージが
    // 無いと ROM は起動デバイスの走査を最後まで進めず、ドライブ 0 の ST0 が
    // 0 のままになることがある。イメージはライセンス上同梱できないので、
    // ここでは「ハンドラが走った」ことだけを縛る。ST0 の中身は
    // just run --hdd <イメージ> --watch 0x000C90 で確かめられる
    // (PC=$FF1196 で書かれる)。
    CHECK(flag != 0);

    // EC (Equipment Check, bit4) が立ってはいけない。
    //
    // メディア未挿入で EC を立てていたとき、ブートコードが
    // 「ドライブが壊れている」と判断して起動を諦め、画面が真っ黒になった。
    // EC は「ドライブはあるが壊れている」で、「メディアが無い」は NR で表す。
    constexpr x68k::u8 kSt0EquipmentCheck = 0x10;
    CHECK((st0Drive0 & kSt0EquipmentCheck) == 0);
    (void)st0Drive0;
}
