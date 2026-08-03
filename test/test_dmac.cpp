// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: DMAC が「どちらへ・何バイト・どこまで」転送したかを
// 正しく反映すること。
//
// X68000 の SASI はデータ転送を DMAC 経由で行う。IPL-ROM はブートセクタを
// 読むとき SASI へ READ を送った後 DMAC のチャネル 1 を起動し ($FF9944)、
// CSR の完了ビットが立つのを待つ。
//
// ここが静かに壊れると症状が出にくい。転送方向を取り違えれば 1 バイトも
// 動かず、転送量がずれれば尻切れのブートコードを実行することになるが、
// どちらも「完了した」ように見えてしまう。だから「動いたか」だけでなく
// 「レジスタが実際の転送量と辻褄が合っているか」まで見る。

#include <cstring>
#include <vector>

#include "dev/dmac.h"
#include "doctest.h"
#include "machine.h"

namespace
{

// 決まったバイト列を出し入れするだけのデバイス。
// SASI を通さずに DMAC 単体の振る舞いを見るために使う。
class FakeDmaDevice final : public x68k::DmaDevice
{
public:
    // 読み出しで渡せるデータ。空になったら false を返す (デバイス側が尽きた状態)。
    std::vector<x68k::u8> readable;
    // 書き込みで受け取ったデータ。
    std::vector<x68k::u8> written;
    // 受け取れる上限。超えたら false を返す。
    std::size_t writeCapacity = 0;
    std::size_t readPos = 0;

    bool dmaRead(x68k::u8* value) override
    {
        if (readPos >= readable.size())
        {
            return false;
        }
        *value = readable[readPos++];
        return true;
    }

    bool dmaWrite(x68k::u8 value) override
    {
        if (written.size() >= writeCapacity)
        {
            return false;
        }
        written.push_back(value);
        return true;
    }
};

// 単純なバイト配列のメモリ。
class FakeDmaMemory final : public x68k::DmaMemory
{
public:
    std::vector<x68k::u8> bytes = std::vector<x68k::u8>(0x10000, 0);

    x68k::u8 dmaMemRead(x68k::u32 addr) override
    {
        return addr < bytes.size() ? bytes[addr] : 0u;
    }

    void dmaMemWrite(x68k::u32 addr, x68k::u8 value) override
    {
        if (addr < bytes.size())
        {
            bytes[addr] = value;
        }
    }
};

// チャネル 1 を設定して起動する。IPL-ROM がやる手順と同じ順序。
struct DmacFixture
{
    x68k::Dmac dmac;
    FakeDmaDevice device;
    FakeDmaMemory memory;

    DmacFixture()
    {
        dmac.setDevice(&device);
        dmac.setMemory(&memory);
    }

    // チャネル 1 のレジスタは先頭から 0x40 の位置に並ぶ。
    static constexpr x68k::u32 kCh1 = x68k::Dmac::kChannelStride * x68k::Dmac::kSasiChannel;

    void setAddress(x68k::u32 addr)
    {
        dmac.write(kCh1 + x68k::Dmac::kRegMar + 0, static_cast<x68k::u8>(addr >> 24));
        dmac.write(kCh1 + x68k::Dmac::kRegMar + 1, static_cast<x68k::u8>(addr >> 16));
        dmac.write(kCh1 + x68k::Dmac::kRegMar + 2, static_cast<x68k::u8>(addr >> 8));
        dmac.write(kCh1 + x68k::Dmac::kRegMar + 3, static_cast<x68k::u8>(addr));
    }

    void setCount(x68k::u32 count)
    {
        dmac.write(kCh1 + x68k::Dmac::kRegMtc + 0, static_cast<x68k::u8>(count >> 8));
        dmac.write(kCh1 + x68k::Dmac::kRegMtc + 1, static_cast<x68k::u8>(count));
    }

    [[nodiscard]] x68k::u32 address()
    {
        return (static_cast<x68k::u32>(dmac.read(kCh1 + x68k::Dmac::kRegMar + 0)) << 24) |
               (static_cast<x68k::u32>(dmac.read(kCh1 + x68k::Dmac::kRegMar + 1)) << 16) |
               (static_cast<x68k::u32>(dmac.read(kCh1 + x68k::Dmac::kRegMar + 2)) << 8) |
               static_cast<x68k::u32>(dmac.read(kCh1 + x68k::Dmac::kRegMar + 3));
    }

    [[nodiscard]] x68k::u32 count()
    {
        return (static_cast<x68k::u32>(dmac.read(kCh1 + x68k::Dmac::kRegMtc + 0)) << 8) |
               static_cast<x68k::u32>(dmac.read(kCh1 + x68k::Dmac::kRegMtc + 1));
    }

    [[nodiscard]] x68k::u8 csr()
    {
        return dmac.read(kCh1 + x68k::Dmac::kRegCsr);
    }

    // デバイス → メモリ (READ) の向きで起動する。IPL-ROM は OCR に $B2 を書く。
    void startToMemory()
    {
        dmac.write(kCh1 + x68k::Dmac::kRegOcr, 0xB2);
        dmac.write(kCh1 + x68k::Dmac::kRegCcr, x68k::Dmac::kCcrStart);
    }

    // メモリ → デバイス (WRITE) の向きで起動する。
    void startToDevice()
    {
        dmac.write(kCh1 + x68k::Dmac::kRegOcr, 0x32);
        dmac.write(kCh1 + x68k::Dmac::kRegCcr, x68k::Dmac::kCcrStart);
    }
};

}  // namespace

TEST_CASE("デバイスからメモリへ指定バイト数だけ転送する")
{
    // 保証すること: 転送量が MTC の値ちょうどであること。
    //
    // 壊れると: 1 バイト多い/少ないぶんだけブートコードがずれる。
    // 実行はされるので「なぜか変な命令を実行している」という形でしか出ない。
    DmacFixture f;
    f.device.readable = {0x11, 0x22, 0x33, 0x44};
    f.setAddress(0x1000);
    f.setCount(3);
    f.startToMemory();

    CHECK(f.memory.bytes[0x1000] == 0x11);
    CHECK(f.memory.bytes[0x1001] == 0x22);
    CHECK(f.memory.bytes[0x1002] == 0x33);
    // 4 バイト目は転送されない。
    CHECK(f.memory.bytes[0x1003] == 0x00);
}

TEST_CASE("メモリからデバイスへ転送できる")
{
    // 保証すること: OCR の bit7 が 0 なら向きが逆になること。
    // SASI の WRITE コマンドがこの向きを使う。
    //
    // 壊れると: 書き込みでメモリを壊す (逆向きに転送してしまう)。
    DmacFixture f;
    f.memory.bytes[0x2000] = 0xAA;
    f.memory.bytes[0x2001] = 0xBB;
    f.device.writeCapacity = 16;
    f.setAddress(0x2000);
    f.setCount(2);
    f.startToDevice();

    CHECK(f.device.written.size() == 2);
    CHECK(f.device.written[0] == 0xAA);
    CHECK(f.device.written[1] == 0xBB);
}

TEST_CASE("転送方向は OCR の bit7 だけで決まる")
{
    // 保証すること: OCR の他のビットが立っていても向きの判定に影響しないこと。
    // IPL-ROM が書く $B2 は bit7 以外にも複数のビットが立っている。
    //
    // 壊れると: マスクの取り方を間違えて向きが反転し、1 バイトも
    // 転送されないまま "X68K" の検査で失敗する。
    DmacFixture f;
    f.device.readable = {0x5A};
    f.setAddress(0x3000);
    f.setCount(1);
    // bit7 だけ立てた最小の値でも「デバイス → メモリ」になる。
    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegOcr, x68k::Dmac::kOcrDirectionToMemory);
    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegCcr, x68k::Dmac::kCcrStart);

    CHECK(f.memory.bytes[0x3000] == 0x5A);
}

TEST_CASE("転送後に MAR が進み MTC が 0 になる")
{
    // 保証すること: 転送した量とレジスタの残量が一致すること。
    //
    // 壊れると: 転送量と残量がずれ、「DMA が途中で止まったまま成功に見える」
    // 状態を後から検出できなくなる。実際にこの種のずれが退行として起きた。
    DmacFixture f;
    f.device.readable.assign(256, 0x77);
    f.setAddress(0x1000);
    f.setCount(256);
    f.startToMemory();

    CHECK(f.address() == 0x1000 + 256);
    CHECK(f.count() == 0);
}

TEST_CASE("完了すると CSR の COC が立ち ACT が落ちる")
{
    // 保証すること: IPL-ROM が待つ完了ビットが立つこと ($FF9944 以降)。
    //
    // 壊れると: IPL-ROM が完了待ちのループから出られず、
    // 起動が DMA の直後で止まる。
    DmacFixture f;
    f.device.readable = {0x01, 0x02};
    f.setAddress(0x1000);
    f.setCount(2);
    // 起動前に ACT を立てておき、完了で落ちることを見る。
    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegCsr, 0xFF);  // 一旦クリア
    f.startToMemory();

    CHECK((f.csr() & x68k::Dmac::kCsrChannelOperationComplete) != 0);
    CHECK((f.csr() & x68k::Dmac::kCsrChannelActive) == 0);
}

TEST_CASE("CSR は書き込んだビットがクリアされる")
{
    // 保証すること: CSR が write-1-to-clear であること。IPL-ROM は転送前に
    // $FF を書いて全ビットを落とす ($FF9944)。
    //
    // 壊れると: 前回の完了ビットが残ったままになり、IPL-ROM が
    // 「もう終わっている」と誤認して転送前のメモリを読む。
    DmacFixture f;
    f.device.readable = {0x01};
    f.setAddress(0x1000);
    f.setCount(1);
    f.startToMemory();
    CHECK((f.csr() & x68k::Dmac::kCsrChannelOperationComplete) != 0);

    // 該当ビットを 1 で書くと落ちる。
    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegCsr, x68k::Dmac::kCsrChannelOperationComplete);
    CHECK((f.csr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
}

TEST_CASE("CSR へ 0 を書いても既に立っているビットは消えない")
{
    // 保証すること: write-1-to-clear の裏返し。0 のビットは触らない。
    //
    // 壊れると: 通常の代入で実装してしまい、CSR の一部を読み書きする
    // コードが他のビットを巻き添えで消す。
    DmacFixture f;
    f.device.readable = {0x01};
    f.setAddress(0x1000);
    f.setCount(1);
    f.startToMemory();

    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegCsr, 0x00);
    CHECK((f.csr() & x68k::Dmac::kCsrChannelOperationComplete) != 0);
}

TEST_CASE("CCR の bit7 が立っていなければ転送は始まらない")
{
    // 保証すること: 起動ビット以外の書き込みで誤って転送が走らないこと。
    //
    // 壊れると: レジスタ設定の途中で転送が始まり、MAR や MTC が
    // 未設定のまま関係ないアドレスを書き潰す。
    DmacFixture f;
    f.device.readable = {0x99};
    f.setAddress(0x1000);
    f.setCount(1);
    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegOcr, 0xB2);
    // bit7 を落とした値を CCR へ書く。
    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegCcr, 0x7F);

    CHECK(f.memory.bytes[0x1000] == 0x00);
    CHECK(f.count() == 1);
}

TEST_CASE("デバイスが尽きたら転送はそこで止まる")
{
    // 保証すること: 要求量より少ないデータしか無いとき、
    // 転送を打ち切って MTC に残量が残ること。
    //
    // 壊れると: 尽きた後も 0 を書き続けてメモリを潰すか、
    // 無限ループになる。
    DmacFixture f;
    f.device.readable = {0x01, 0x02};  // 2 バイトしかない
    f.setAddress(0x1000);
    f.setCount(10);
    f.startToMemory();

    CHECK(f.memory.bytes[0x1000] == 0x01);
    CHECK(f.memory.bytes[0x1001] == 0x02);
    CHECK(f.memory.bytes[0x1002] == 0x00);
    // 残量がレジスタに残る。ここがずれると「途中で止まったのに
    // 完走したように見える」状態になる。
    CHECK(f.count() == 8);
    CHECK(f.address() == 0x1000 + 2);
}

TEST_CASE("デバイスもメモリも繋がっていなければ何も起きない")
{
    // 保証すること: 未接続の状態で起動しても落ちないこと。
    // Machine の構築順によっては setDevice の前に write が来うる。
    //
    // 壊れると: nullptr 参照でクラッシュする。
    x68k::Dmac dmac;
    dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegMtc + 1, 4);
    dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegOcr, 0xB2);
    dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegCcr, x68k::Dmac::kCcrStart);
    // 転送カウントは触られない。
    CHECK(dmac.read(DmacFixture::kCh1 + x68k::Dmac::kRegMtc + 1) == 4);
}

TEST_CASE("チャネル 1 以外のレジスタはチャネル 1 と混ざらない")
{
    // 保証すること: FDC が使うチャネル 0 への書き込みが SASI の
    // チャネル 1 を壊さないこと。
    //
    // 壊れると: IOCS が FDC 側を初期化した拍子に SASI の転送設定が
    // 消え、ブートセクタが読めなくなる。
    DmacFixture f;
    // チャネル 0 の MAR。
    f.dmac.write(x68k::Dmac::kRegMar, 0x11);
    // チャネル 1 の MAR。
    f.dmac.write(DmacFixture::kCh1 + x68k::Dmac::kRegMar, 0x22);

    CHECK(f.dmac.read(x68k::Dmac::kRegMar) == 0x11);
    CHECK(f.dmac.read(DmacFixture::kCh1 + x68k::Dmac::kRegMar) == 0x22);
}

TEST_CASE("reset() で転送設定が初期化される")
{
    // 保証すること: リセット後に前回の MAR/MTC が残らないこと。
    //
    // 壊れると: ウォームブートで前回のアドレスへ転送してしまう。
    DmacFixture f;
    f.setAddress(0x1234);
    f.setCount(0x100);
    f.dmac.reset();

    CHECK(f.address() == 0);
    CHECK(f.count() == 0);
    CHECK(f.csr() == 0);
}

TEST_CASE("SASI からメモリへの転送で SASI がステータスフェーズへ進む")
{
    // 保証すること: DMAC と SASI が繋がっており、転送し切ると SASI 側の
    // 状態機械も次へ進むこと。片方だけ進むと以降の手順が噛み合わない。
    //
    // 壊れると: DMA は完了したのに SASI がデータインのままになり、
    // IPL-ROM のステータス待ちがタイムアウトする。
    x68k::Machine m;
    std::vector<x68k::u8> sasiBuf(x68k::Machine::kSasiBufferBytes, 0);
    m.setSasiBuffer(sasiBuf.data());
    std::vector<x68k::u8> ram(x68k::kMainRamSize, 0);
    x68k::MemoryMap memory;
    memory.mainRam = ram.data();
    m.setMemory(memory);

    // セクタごとに識別できる中身のディスク。
    class Disk final : public x68k::DiskImage
    {
    public:
        bool readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount) override
        {
            std::memset(buffer, static_cast<int>(lba), sectorCount * 256);
            return true;
        }
        bool writeSector(x68k::u32, const x68k::u8*, x68k::u32) override
        {
            return true;
        }
        [[nodiscard]] bool isPresent() const override
        {
            return true;
        }
    } disk;
    m.setDisk(&disk);

    // READ コマンドを送る。
    m.ioWrite8(x68k::kSasiBase + 7, 0x01);
    m.ioRead8(x68k::kSasiBase + 3);
    const x68k::u8 command[6] = {0x08, 0x00, 0x00, 0x05, 0x01, 0x00};
    for (const x68k::u8 b : command)
    {
        m.ioWrite8(x68k::kSasiBase + 1, b);
    }

    constexpr x68k::u32 kCh1Io = x68k::kDmacBase + 0x40;
    constexpr x68k::u32 kDest = 0x00010000;
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegCsr, 0xFF);
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegOcr, 0xB2);
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegMar + 0, static_cast<x68k::u8>(kDest >> 24));
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegMar + 1, static_cast<x68k::u8>(kDest >> 16));
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegMar + 2, static_cast<x68k::u8>(kDest >> 8));
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegMar + 3, static_cast<x68k::u8>(kDest));
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegMtc + 0, 0x01);
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegMtc + 1, 0x00);
    m.ioWrite8(kCh1Io + x68k::Dmac::kRegCcr, x68k::Dmac::kCcrStart);

    // セクタ 5 の中身が届いている。
    CHECK(m.bus().read8(kDest) == 0x05);
    CHECK(m.bus().read8(kDest + 255) == 0x05);
    // 転送量ちょうどで止まる。
    CHECK(m.bus().read8(kDest + 256) == 0x00);
    // SASI 側もステータスフェーズへ進む。
    CHECK(m.ioRead8(x68k::kSasiBase + 3) == 0x0F);
}

// --- 実装の問題を再現するテスト ----------------------------------------------
//
// 以下 2 つは現状の実装では失敗する。どちらも「DMA が途中で止まったまま
// 成功に見える」という、過去に退行として起きたのと同じ形の問題。
// 直すまで DOCTEST_MAY_FAIL で印を付けておく。

TEST_CASE("1 バイトも転送できなかったとき完了ビットを立てない" * doctest::may_fail(true))
{
    // 保証したいこと: デバイスが 1 バイトも渡せなかったとき、
    // CSR の完了ビット (COC) を立てないこと。
    //
    // 壊れると: IPL-ROM は COC を見て「転送が終わった」と判断するので、
    // 転送前のメモリの中身をブートセクタとして扱う。DMA が全く動いて
    // いないのに成功に見えるので、原因の切り分けが極めて難しくなる。
    //
    // 現状: 転送量が 0 でも無条件に COC を立てている
    // (dmac.cpp の runTransfer() 末尾)。
    DmacFixture f;
    f.device.readable.clear();  // デバイスは何も渡せない
    f.setAddress(0x1000);
    f.setCount(16);
    f.startToMemory();

    CHECK(f.count() == 16);  // 1 バイトも進んでいない
    CHECK((f.csr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
}

TEST_CASE("要求量に届かなかったとき完了ビットを立てない" * doctest::may_fail(true))
{
    // 保証したいこと: MTC で要求したバイト数に届かないまま打ち切ったとき、
    // 完了ではなくエラーとして扱うこと。
    //
    // 壊れると: 尻切れのブートコードを「完全に読めた」ものとして実行する。
    // これは「READ のセクタ数を黙って切り詰めていた」退行と同じ症状で、
    // 転送量と残量がずれたまま成功に見える。
    //
    // 現状: 途中で break しても COC を立てている。
    DmacFixture f;
    f.device.readable.assign(256, 0xAA);  // 256 バイトしかない
    f.setAddress(0x1000);
    f.setCount(1024);  // 1024 バイト要求する
    f.startToMemory();

    CHECK(f.count() == 1024 - 256);  // 768 バイト残っている
    CHECK((f.csr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
}

TEST_CASE("MTC=0 は 65536 バイトを意味する" * doctest::may_fail(true))
{
    // 保証したいこと: HD63450 の MTC は 0 を 65536 として扱うこと。
    //
    // 壊れると: 65536 バイトの転送要求が 0 バイトの転送として完了し、
    // 大きな読み出しが黙って何もしないまま成功する。
    //
    // 現状: count = 0 でループに入らず、そのまま COC を立てている。
    DmacFixture f;
    f.device.readable.assign(4, 0x5A);
    f.setAddress(0x1000);
    f.setCount(0);
    f.startToMemory();

    // 65536 バイトのつもりなので、4 バイトだけ転送して残りが残る。
    CHECK(f.memory.bytes[0x1000] == 0x5A);
}
