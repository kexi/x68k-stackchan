// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: CPU がメインメモリを仮想関数を通さずに触る「直接経路」が、
// バスを通した場合とまったく同じ結果になること。
//
// この経路は速度のためだけに入れたもので、意味を変えてはいけない。
// CPU 側の read/write は private なので、実際に 68000 の命令を走らせて
// 外から見える結果で確かめる (実機と同じ経路を通す)。押さえるのは:
//
//   1. $000000 に IPL-ROM が写像されている間、命令フェッチもデータ読みも
//      ROM 側に当たること (素通しすると RAM を読んでしまい起動しない)
//   2. 写像が外れたら RAM 側に当たること
//   3. 写像中でも書き込みは RAM に届くこと (ROM は書けないので実機も RAM 行き)
//   4. CPU が書いた内容が DMA (バス経由) から見えること、およびその逆
//      (直接経路が写しではなく同じ実体を指していることの検査)
//   5. 窓の外 (テキスト VRAM 等) へのアクセスが直接経路に吸われないこと
//
// 4 が壊れると SASI の転送結果が CPU から見えず、ディスクが読めなくなる。
// 症状が「起動途中で止まる」としか出ないので、ここで直接押さえる。

#include <vector>

#include "bus.h"
#include "cpu/m68k.h"
#include "doctest.h"
#include "machine.h"
#include "memmap.h"

namespace
{

// 68000 の命令とデータを並べるための小さな道具。
void poke16(std::vector<x68k::u8>& mem, std::size_t at, x68k::u16 value)
{
    mem[at] = static_cast<x68k::u8>(value >> 8);
    mem[at + 1] = static_cast<x68k::u8>(value & 0xFFu);
}

void poke32(std::vector<x68k::u8>& mem, std::size_t at, x68k::u32 value)
{
    poke16(mem, at, static_cast<x68k::u16>(value >> 16));
    poke16(mem, at + 2, static_cast<x68k::u16>(value & 0xFFFFu));
}

// $000000 へ写像される ROM の、ROM 内でのオフセット ($FF0000 側)。
constexpr std::size_t kRomOffsetOfFF0000 = 0xFF0000u - x68k::kIplromBase;

// CPU を走らせるための一式。ROM にプログラムを置き、リセットで起動する。
struct Rig
{
    std::vector<x68k::u8> mainRam;
    std::vector<x68k::u8> iplRom;
    x68k::Machine machine;

    Rig() : mainRam(x68k::kMainRamSize, 0), iplRom(x68k::kIplromSize, 0)
    {
        x68k::MemoryMap memory;
        memory.mainRam = mainRam.data();
        memory.iplRom = iplRom.data();
        machine.setMemory(memory);
    }

    // ROM 内 ($FF0000 側) のオフセットへ 1 ワード置く。
    void romPoke16(std::size_t offsetFromFF0000, x68k::u16 value)
    {
        poke16(iplRom, kRomOffsetOfFF0000 + offsetFromFF0000, value);
    }

    // リセットベクタを張って $FF0010 から実行を始める。
    // (実機と同じ起点。memmap.h の kResetPc / kResetSsp を見よ)
    void bootFromRom()
    {
        poke32(iplRom, kRomOffsetOfFF0000 + 0, x68k::kResetSsp);
        poke32(iplRom, kRomOffsetOfFF0000 + 4, x68k::kResetPc);
        machine.reset();
    }
};

}  // namespace

TEST_SUITE("m68k-fastram")
{
    TEST_CASE("ROM 写像中はデータ読みも ROM 側に当たる")
    {
        Rig rig;
        // $000100 に、ROM と RAM で別の値を置く。どちらが読まれたかで判別する。
        // ROM 写像中に RAM 側が読まれると IPL-ROM が自分のテーブルを読めない。
        poke16(rig.iplRom, kRomOffsetOfFF0000 + 0x100, 0xABCD);
        poke16(rig.mainRam, 0x100, 0x1122);

        // $FF0010: MOVE.W $0100,D0   (0x3038 0x0100 = MOVE.W (xxx).W,D0)
        rig.romPoke16(0x10, 0x3038);
        rig.romPoke16(0x12, 0x0100);

        rig.bootFromRom();
        // 写像はリセット直後に立っている。
        REQUIRE(rig.machine.bus().romMappedAtZero());

        rig.machine.step();
        CHECK((rig.machine.cpu().state().d[0] & 0xFFFFu) == 0xABCD);
    }

    TEST_CASE("写像が外れるとデータ読みは RAM 側に当たる")
    {
        Rig rig;
        poke16(rig.iplRom, kRomOffsetOfFF0000 + 0x100, 0xABCD);
        poke16(rig.mainRam, 0x100, 0x1122);

        rig.romPoke16(0x10, 0x3038);
        rig.romPoke16(0x12, 0x0100);

        rig.bootFromRom();
        // IPL-ROM がエリアセットへ書くのと同じことを外から起こす。
        rig.machine.bus().setRomMappedAtZero(false);

        rig.machine.step();
        CHECK((rig.machine.cpu().state().d[0] & 0xFFFFu) == 0x1122);
    }

    TEST_CASE("ROM 写像中でも書き込みは RAM に届く")
    {
        Rig rig;
        // $FF0010: MOVE.W #$55AA,$0200
        //   0x31FC = MOVE.W #imm,(xxx).W
        rig.romPoke16(0x10, 0x31FC);
        rig.romPoke16(0x12, 0x55AA);
        rig.romPoke16(0x14, 0x0200);

        rig.bootFromRom();
        REQUIRE(rig.machine.bus().romMappedAtZero());

        rig.machine.step();
        // 実機でも ROM は書けないので、行き先は RAM。
        CHECK(rig.mainRam[0x200] == 0x55);
        CHECK(rig.mainRam[0x201] == 0xAA);
    }

    TEST_CASE("CPU が書いた内容が DMA から見え、DMA が書いた内容を CPU が読む")
    {
        // 直接経路が「写し」ではなく同じ実体を指していることの検査。
        // 写しにすると SASI の転送結果が CPU から見えなくなる。
        Rig rig;

        // $FF0010: MOVE.W $1000,D0
        rig.romPoke16(0x10, 0x3038);
        rig.romPoke16(0x12, 0x1000);
        // $FF0014: MOVE.W #$BEEF,$1002
        rig.romPoke16(0x14, 0x31FC);
        rig.romPoke16(0x16, 0xBEEF);
        rig.romPoke16(0x18, 0x1002);

        // プログラムを置いてから起動する。
        // 先に reset() すると空の ROM をプリフェッチしてしまう。
        rig.bootFromRom();
        rig.machine.bus().setRomMappedAtZero(false);

        // DMA が書いたものを CPU が読めること。
        rig.machine.dmaMemWrite(0x1000, 0x3C);
        rig.machine.dmaMemWrite(0x1001, 0xD2);

        rig.machine.step();
        CHECK((rig.machine.cpu().state().d[0] & 0xFFFFu) == 0x3CD2);

        // CPU が書いたものを DMA が読めること。
        rig.machine.step();
        CHECK(rig.machine.dmaMemRead(0x1002) == 0xBE);
        CHECK(rig.machine.dmaMemRead(0x1003) == 0xEF);
        // 実体そのものにも届いている (写しなら 0 のまま)。
        CHECK(rig.mainRam[0x1002] == 0xBE);
        CHECK(rig.mainRam[0x1003] == 0xEF);
    }

    TEST_CASE("IPL-ROM の直接読みがバス経由と一致する")
    {
        // アクセスの 79% が IPL-ROM なので、ここが最頻の経路になる
        // (分布は docs/knowledge/cores3-emulator-runtime.md)。
        Rig rig;
        // ROM の端に見分けの付く値を置く。
        rig.iplRom[0] = 0x11;                      // $FE0000
        rig.iplRom[1] = 0x22;                      // $FE0001
        rig.iplRom[x68k::kIplromSize - 2] = 0x99;  // $FFFFFE
        rig.iplRom[x68k::kIplromSize - 1] = 0xAA;  // $FFFFFF
        poke32(rig.iplRom, 0x40, 0xDEADBEEF);      // $FE0040

        // $FF0010: MOVE.L $00FE0040,D0  (0x2039 = MOVE.L (xxx).L,D0)
        rig.romPoke16(0x10, 0x2039);
        rig.romPoke16(0x12, 0x00FE);
        rig.romPoke16(0x14, 0x0040);

        rig.bootFromRom();
        rig.machine.step();
        CHECK(rig.machine.cpu().state().d[0] == 0xDEADBEEF);

        // バス経由でも同じ値が見えること (窓が別物になっていない)。
        CHECK(rig.machine.bus().read16(0xFE0000) == 0x1122);
        CHECK(rig.machine.bus().read16(0xFFFFFE) == 0x99AA);
    }

    TEST_CASE("ROM の終端をまたぐワードは直接経路に吸われない")
    {
        // $FFFFFF は ROM の最後のバイト。そこから始まるワードは
        // 2 バイト目が窓の外なので、直接経路を通してはいけない。
        //
        // 【この検査の限界を明記しておく】
        //
        // 境界の判定 (off + size > len) を off > len へ変えても、この検査は
        // 通り続ける。mutation で確認済み。値では縛れない。
        //
        // 理由: kIplromBase + kIplromSize はちょうど 24bit 空間の上端
        // ($1000000) で、その次のアドレスは折り返して $000000 になる。
        // つまり「1 バイト踏み越した先」に到達できる masked アドレスが
        // 存在しない。現時点では踏み越しが観測可能な形で現れない。
        //
        // したがってここが守っているのは「上位バイトが最終バイトである」
        // ことだけで、境界判定そのものではない。窓を上端に接していない
        // 領域へ流用したら、その時点で実際の領域外読み出しになる。
        // 流用するときは、この検査では捕まらないと承知して進めること。
        //
        // Why not ASan に任せないか: macOS では ASan が使えない
        // (dyld とのデッドロックで test/CMakeLists.txt が UBSan だけに
        // 落としてある)。Linux の CI なら捕まる可能性はあるが、
        // 上のとおり踏み越し自体が到達不能なので現状は無意味。
        Rig rig;
        // ROM の最終バイトに目印を置く。$FFFFFF = ROM 先頭 + kIplromSize-1。
        rig.iplRom[x68k::kIplromSize - 1] = 0x5A;
        rig.bootFromRom();

        // 最終バイトの読み出しは窓の中なので直接経路でよい。
        CHECK(rig.machine.bus().read8(0xFFFFFF) == 0x5A);

        // そこから始まるワードは 2 バイト目が窓の外。直接経路を通ると
        // 配列の 1 バイト外を読む。バス側へ落ちれば、応答しない領域として
        // 扱われる (この 24bit 空間の上端の次は折り返して $000000)。
        // どちらにせよ「上位バイトが目印」であることは保たれる。
        const x68k::u16 word = rig.machine.bus().read16(0xFFFFFF);
        CHECK(((word >> 8) & 0xFFu) == 0x5A);
    }

    TEST_CASE("メインメモリの外へのアクセスは直接経路に吸われない")
    {
        Rig rig;
        std::vector<x68k::u8> textVram(x68k::kTvramSize, 0);
        x68k::MemoryMap memory;
        memory.mainRam = rig.mainRam.data();
        memory.iplRom = rig.iplRom.data();
        memory.textVram = textVram.data();
        rig.machine.setMemory(memory);

        // $FF0010: MOVE.W #$1234,$00E00000   (0x33FC = MOVE.W #imm,(xxx).L)
        rig.romPoke16(0x10, 0x33FC);
        rig.romPoke16(0x12, 0x1234);
        rig.romPoke16(0x14, 0x00E0);
        rig.romPoke16(0x16, 0x0000);

        rig.bootFromRom();
        rig.machine.bus().setRomMappedAtZero(false);

        rig.machine.step();
        // テキスト VRAM へ届いていること。直接経路が窓の外まで拾っていれば
        // ここが 0 のままで、代わりにメインメモリ側が汚れる。
        CHECK(textVram[0] == 0x12);
        CHECK(textVram[1] == 0x34);
        // ダーティ追跡も動いている (バスの write を通った証拠)。
        CHECK(rig.machine.bus().isTextRowDirty(0));
    }
}
