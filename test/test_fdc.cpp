// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: FDC がイメージの有無で二通りに正しく振る舞うこと。
//
//   イメージ無し — FD ドライブが繋がっていないことを IPL-ROM が諦められる形で
//     返し、かつメインステータスが「コマンドを受け付けられる状態」へ必ず戻る。
//   イメージ有り — ドライブがレディを返し、READ/WRITE DATA が DMAC 経由で
//     実際のセクタを運ぶ (単一・複数セクタとも)。
//
// 本エミュレータの既定の起動デバイスは SASI だが、IPL-ROM は起動デバイスに
// 関わらず FDC を初期化しに来る。その待ちループはどれもタイムアウトを持たない
// ので、メインステータスが一度でも塞がったままになると起動がそこで永久に止まる。
//
// 特に危ないのが結果フェーズの置き去りである。IPL-ROM は DMA を使う
// READ/WRITE DATA の結果を、コマンド送出のその場では読まず FDC 割り込み
// ハンドラ ($FF1130) に任せている。本エミュレータは FDC の割り込み線を
// 配線していないので、結果バイトを積むと誰も読まない。すると次のコマンド
// 送出ルーチン ($FF9036) が CB (bit4) の落ちるのを待ち続けて止まる。
// この退行は「FDC のテストが通っている」だけでは捕まらず、起動を最後まで
// 走らせて初めて分かるので、ここで直接ステータスを見る。
//
// 転送経路は Machine + DMAC を通して検査する。FDC 単体で dmaRead を叩くだけ
// では「DMAC のチャネル 0 に繋がっていない」という配線ミスが素通りする。
// 実際 SASI はチャネル 1、FDC はチャネル 0 (IPL-ROM $FF8F3C が $E84005 /
// $E8400A / $E8400C / $E84007 を叩く) で、取り違えると 1 バイトも流れない。

#include <cstring>
#include <vector>

#include "dev/fdc.h"
#include "doctest.h"
#include "machine.h"

namespace
{

// テスト用のフロッピー。X68000 標準の 2HD (77C x 2H x 8S x 1024B)。
//
// セクタごとに識別できる中身を入れておく。どのセクタが読まれたのかを
// 転送先のメモリから逆算できないと、「1 セクタずれて読んだ」ことに
// 気付けない。
class FakeFloppy final : public x68k::FloppyImage
{
public:
    FakeFloppy()
    {
        data_.assign(geometry_.totalBytes(), 0);
        for (x68k::u32 lba = 0; lba * geometry_.sectorSize < data_.size(); ++lba)
        {
            x68k::u8* const sector = data_.data() + lba * geometry_.sectorSize;
            sector[0] = static_cast<x68k::u8>(lba);
            sector[1] = static_cast<x68k::u8>(lba >> 8);
            sector[2] = 0xA5;
            // 末尾にも印を置く。セクタ長を取り違えたら必ず崩れる位置。
            sector[geometry_.sectorSize - 1] = 0x5A;
        }
    }

    bool readSector(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record, x68k::u8* buffer) override
    {
        std::size_t offset = 0;
        if (!locate(cylinder, head, record, &offset))
        {
            return false;
        }
        std::memcpy(buffer, data_.data() + offset, geometry_.sectorSize);
        ++readCount;
        return true;
    }

    bool writeSector(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record,
                     const x68k::u8* buffer) override
    {
        if (writeProtected)
        {
            return false;
        }
        std::size_t offset = 0;
        if (!locate(cylinder, head, record, &offset))
        {
            return false;
        }
        std::memcpy(data_.data() + offset, buffer, geometry_.sectorSize);
        ++writeCount;
        return true;
    }

    [[nodiscard]] bool isPresent() const override
    {
        return present;
    }
    [[nodiscard]] bool isWriteProtected() const override
    {
        return writeProtected;
    }
    [[nodiscard]] const x68k::FloppyGeometry& geometry() const override
    {
        return geometry_;
    }

    // CHS を LBA へ。テスト側が期待値を組むのにも使う。
    [[nodiscard]] x68k::u32 lbaOf(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record) const
    {
        return ((cylinder * geometry_.heads) + head) * geometry_.sectorsPerTrack + (record - 1);
    }

    [[nodiscard]] const x68k::u8* sector(x68k::u32 lba) const
    {
        return data_.data() + static_cast<std::size_t>(lba) * geometry_.sectorSize;
    }

    bool present = true;
    bool writeProtected = false;
    int readCount = 0;
    int writeCount = 0;

private:
    bool locate(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record, std::size_t* offset) const
    {
        const bool inRange = cylinder < geometry_.cylinders && head < geometry_.heads &&
                             record >= 1 && record <= geometry_.sectorsPerTrack;
        if (!inRange)
        {
            return false;
        }
        *offset = static_cast<std::size_t>(lbaOf(cylinder, head, record)) * geometry_.sectorSize;
        return true;
    }

    x68k::FloppyGeometry geometry_{77, 2, 8, 1024};
    std::vector<x68k::u8> data_;
};

// コマンド 1 バイトとパラメータを順に送る。
void sendCommand(x68k::Fdc& fdc, std::initializer_list<x68k::u8> bytes)
{
    for (const x68k::u8 byte : bytes)
    {
        fdc.writeData(byte);
    }
}

// メインステータスが「コマンド待ち」を示しているか。
// IPL-ROM の $FF9006 は下位 5bit が 0 になるのを完了条件にしている。
bool isIdle(const x68k::Fdc& fdc)
{
    return (fdc.readStatus() & 0x1Fu) == 0;
}

// --- Machine 経由の検査に使う道具 -------------------------------------------

constexpr x68k::u32 kFdcStatusPort = 0xE94001;
constexpr x68k::u32 kFdcDataPort = 0xE94003;
constexpr x68k::u32 kFdcDriveSelectPort = 0xE94007;

// DMAC のチャネル 0 (FDC 用)。IPL-ROM の $FF8F3C が叩くレジスタ。
constexpr x68k::u32 kDmac0Csr = 0xE84000;
constexpr x68k::u32 kDmac0Ocr = 0xE84005;
constexpr x68k::u32 kDmac0Ccr = 0xE84007;
constexpr x68k::u32 kDmac0Mtc = 0xE8400A;
constexpr x68k::u32 kDmac0Mar = 0xE8400C;

// テスト用に組み立てた Machine 一式。メモリの実体を持つ。
struct Rig
{
    Rig()
    {
        x68k::MemoryMap memory;
        memory.mainRam = mainRam.data();
        memory.textVram = textVram.data();
        memory.graphicVram = graphicVram.data();
        memory.iplRom = iplRom.data();
        machine.setMemory(memory);
        machine.reset();
        // リセットで $000000 に ROM が写像されるので外す。
        // 残したままだとメインメモリへの DMA 転送が ROM に吸われる。
        machine.bus().setRomMappedAtZero(false);
    }

    void sendFdcCommand(std::initializer_list<x68k::u8> bytes)
    {
        for (const x68k::u8 byte : bytes)
        {
            machine.ioWrite8(kFdcDataPort, byte);
        }
    }

    // IPL-ROM の $FF8F3C と同じ順序で DMAC のチャネル 0 を起動する。
    //   $E84000 <- $FF   (溜まったステータスを落とす)
    //   $E84005 <- $B2   (bit7 = デバイス → メモリ)
    //   $E8400C <- 転送先アドレス
    //   $E8400A <- 転送バイト数
    //   $E84007 <- $80   (起動)
    void startDma(x68k::u32 addr, x68k::u32 bytes, bool toMemory)
    {
        machine.ioWrite8(kDmac0Csr, 0xFF);
        machine.ioWrite8(kDmac0Ocr, toMemory ? 0xB2 : 0x32);
        machine.ioWrite8(kDmac0Mar, static_cast<x68k::u8>(addr >> 24));
        machine.ioWrite8(kDmac0Mar + 1, static_cast<x68k::u8>(addr >> 16));
        machine.ioWrite8(kDmac0Mar + 2, static_cast<x68k::u8>(addr >> 8));
        machine.ioWrite8(kDmac0Mar + 3, static_cast<x68k::u8>(addr));
        machine.ioWrite8(kDmac0Mtc, static_cast<x68k::u8>(bytes >> 8));
        machine.ioWrite8(kDmac0Mtc + 1, static_cast<x68k::u8>(bytes));
        machine.ioWrite8(kDmac0Ccr, 0x80);
    }

    [[nodiscard]] x68k::u8 dmaCsr()
    {
        return machine.ioRead8(kDmac0Csr);
    }

    // $FF9014 と同じ判定。COC が立ち ERR が落ちていれば転送成功。
    [[nodiscard]] bool dmaSucceeded()
    {
        const x68k::u8 csr = dmaCsr();
        return (csr & x68k::Dmac::kCsrChannelOperationComplete) != 0 &&
               (csr & x68k::Dmac::kCsrError) == 0;
    }

    std::vector<x68k::u8> mainRam{std::vector<x68k::u8>(x68k::kMainRamSize, 0)};
    std::vector<x68k::u8> textVram{std::vector<x68k::u8>(x68k::kTvramSize, 0)};
    std::vector<x68k::u8> graphicVram{std::vector<x68k::u8>(x68k::kTvramSize, 0)};
    std::vector<x68k::u8> iplRom{std::vector<x68k::u8>(x68k::kIplromSize, 0)};
    x68k::Machine machine;
};

// READ DATA / WRITE DATA のパラメータは 9 バイト。$FF8B06 が送るのと同じ並び。
//   [0] MT|MF|SK|opcode  [1] HD|US  [2] C  [3] H  [4] R  [5] N
//   [6] EOT  [7] GPL  [8] DTL
// 各テストがこの並びをそのまま書く。ヘルパで隠すと「どの位置が C で
// どこが R か」がテストから読めなくなり、パラメータの取り違えを
// テスト自身が再現してしまう。

}  // namespace

// --- イメージ無し (従来の振る舞いを保つ) ------------------------------------

TEST_CASE("FDC はリセット直後にコマンドを受け付けられる")
{
    x68k::Fdc fdc;
    fdc.reset();

    // RQM が立ち、CB と DIO は落ちている ($FF904C-$FF9060 が待つ条件)。
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusRqm) != 0);
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusCb) == 0);
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusDio) == 0);
    CHECK(isIdle(fdc));
}

TEST_CASE("イメージが無ければ SENSE DRIVE STATUS はレディを立てない")
{
    x68k::Fdc fdc;
    fdc.reset();

    // $FF89CC が発行するもの: コマンド $04 とドライブ番号。
    sendCommand(fdc, {0x04, 0x00});

    // 結果フェーズ。IPL-ROM の $FF89DE は RQM|DIO|CB ($D0) が揃うのを待つ。
    CHECK((fdc.readStatus() & 0xD0u) == 0xD0u);

    // ST3。bit5 (レディ) が落ちていることがドライブ未接続の表明。
    // $FF90BC の btst #29,d0 がこのビットを見て、立っていなければ諦める。
    const x68k::u8 st3 = fdc.readData();
    CHECK((st3 & 0x20u) == 0);

    // 結果を読み切ったらコマンド待ちへ戻る。
    CHECK(isIdle(fdc));
}

TEST_CASE("イメージが無ければ READ DATA は結果フェーズを残さずコマンド待ちへ戻る")
{
    x68k::Fdc fdc;
    fdc.reset();

    // $FF8B06 が送る 9 バイト。先頭は MT/MF 付きの READ DATA ($46)。
    sendCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x06, 0x03, 0x06, 0x35, 0xFF});

    // ここで結果バイトを積むと、割り込みで拾う相手がいないため
    // CB が立ちっぱなしになり $FF904C で止まる。
    CHECK(isIdle(fdc));
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusCb) == 0);

    // 続けて次のコマンドを送れる。
    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readStatus() & 0xD0u) == 0xD0u);
}

TEST_CASE("イメージが無ければ WRITE DATA も結果フェーズを残さない")
{
    x68k::Fdc fdc;
    fdc.reset();

    sendCommand(fdc, {0x45, 0x00, 0x00, 0x00, 0x06, 0x03, 0x06, 0x35, 0xFF});
    CHECK(isIdle(fdc));
}

TEST_CASE("イメージが無ければ RECALIBRATE は装置チェックで終わる")
{
    x68k::Fdc fdc;
    fdc.reset();

    // RECALIBRATE ($07)。$FF8C26 が発行する。
    sendCommand(fdc, {0x07, 0x00});
    CHECK(isIdle(fdc));
    CHECK(fdc.hasInterrupt());

    // SENSE INTERRUPT STATUS ($08) で異常終了を受け取る。
    // ST0 の bit7-6 = 01 が異常終了、bit4 が装置チェック。
    sendCommand(fdc, {0x08});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0x40u);
    CHECK((st0 & 0x10u) != 0);
    (void)fdc.readData();  // シリンダ番号
    CHECK(isIdle(fdc));
    CHECK_FALSE(fdc.hasInterrupt());
}

TEST_CASE("SPECIFY は結果を返さない")
{
    x68k::Fdc fdc;
    fdc.reset();

    // $03 とタイミングパラメータ 2 バイト。結果フェーズを持たないコマンド。
    sendCommand(fdc, {0x03, 0xD0, 0x10});
    CHECK(isIdle(fdc));

    // イメージを繋いでも SPECIFY の振る舞いは変わらない (タイミングの設定
    // でしかない)。ここで結果を積むようになると $FF9036 が止まる。
    FakeFloppy floppy;
    fdc.setImage(0, &floppy);
    sendCommand(fdc, {0x03, 0xD0, 0x10});
    CHECK(isIdle(fdc));
}

TEST_CASE("割り込みが無いのに SENSE INTERRUPT STATUS を投げたら無効コマンド")
{
    x68k::Fdc fdc;
    fdc.reset();

    sendCommand(fdc, {0x08});

    // ST0 の bit7-6 = 11 が無効コマンド。ここで正常終了を返すと
    // IPL-ROM が「まだ処理すべき割り込みがある」と判断して回り続ける。
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0xC0u);
    CHECK(isIdle(fdc));
}

TEST_CASE("結果を読み切らないまま次のコマンドが来たら受け付ける")
{
    x68k::Fdc fdc;
    fdc.reset();

    // SENSE DRIVE STATUS の結果を残したまま放置する。
    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusCb) != 0);

    // 次のコマンドで古い結果は捨てられる。ここで無視すると
    // CB が落ちず、$FF9036 のコマンド送出が永久に止まる。
    sendCommand(fdc, {0x04, 0x01});
    CHECK((fdc.readStatus() & 0xD0u) == 0xD0u);
    const x68k::u8 st3 = fdc.readData();
    CHECK((st3 & 0x20u) == 0);
    CHECK((st3 & 0x03u) == 0x01u);  // 新しいコマンドのドライブ番号
    CHECK(isIdle(fdc));
}

TEST_CASE("未知のコマンドは無効コマンドとして返す")
{
    x68k::Fdc fdc;
    fdc.reset();

    sendCommand(fdc, {0x1F});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0xC0u);
    CHECK(isIdle(fdc));
}

// --- ジオメトリと形式の判定 --------------------------------------------------

TEST_CASE("XDF は長さからジオメトリを引く")
{
    x68k::FloppyGeometry geo;
    x68k::u32 offset = 0xFFFF;

    // X68000 標準の 2HD。77 x 2 x 8 x 1024 = 1,261,568。
    CHECK(x68k::detectFloppyFormat(1261568, &offset, &geo) == x68k::FloppyFormat::Xdf);
    CHECK(offset == 0);
    CHECK(geo.cylinders == 77);
    CHECK(geo.heads == 2);
    CHECK(geo.sectorsPerTrack == 8);
    CHECK(geo.sectorSize == 1024);
    CHECK(geo.totalBytes() == 1261568);

    // 2DD 9 セクタ。80 x 2 x 9 x 512 = 737,280。
    CHECK(x68k::detectFloppyFormat(737280, &offset, &geo) == x68k::FloppyFormat::Xdf);
    CHECK(offset == 0);
    CHECK(geo.sectorSize == 512);
    CHECK(geo.sectorsPerTrack == 9);
}

TEST_CASE("DIM は 256 バイトのヘッダを飛ばす")
{
    x68k::FloppyGeometry geo;
    x68k::u32 offset = 0;

    // 2HD + 256 バイトヘッダ。
    CHECK(x68k::detectFloppyFormat(1261568 + 256, &offset, &geo) == x68k::FloppyFormat::Dim);
    CHECK(offset == x68k::kDimHeaderBytes);
    CHECK(geo.sectorSize == 1024);
    CHECK(geo.totalBytes() == 1261568);
}

TEST_CASE("長さが未知のイメージは受け付けない")
{
    x68k::FloppyGeometry geo;
    x68k::u32 offset = 0;

    // 半端な長さ。ここを「とりあえず 2HD」として受け入れると、末尾の
    // トラックが読めないまま「ディスクはある」と見え、Human68k が
    // FAT を読んだ時点で原因の遠い壊れ方をする。
    CHECK(x68k::detectFloppyFormat(1000000, &offset, &geo) == x68k::FloppyFormat::Unknown);
    CHECK(x68k::detectFloppyFormat(0, &offset, &geo) == x68k::FloppyFormat::Unknown);
    CHECK(x68k::detectFloppyFormat(1261568 - 1, &offset, &geo) == x68k::FloppyFormat::Unknown);
    // ヘッダぶんちょうどでも中身が無ければ駄目。
    CHECK(x68k::detectFloppyFormat(256, &offset, &geo) == x68k::FloppyFormat::Unknown);
}

// --- イメージ有り: ステータス系 ----------------------------------------------

TEST_CASE("イメージを入れると SENSE DRIVE STATUS がレディを返す")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;
    fdc.setImage(0, &floppy);

    sendCommand(fdc, {0x04, 0x00});
    const x68k::u8 st3 = fdc.readData();

    // bit5 = レディ。$FF90BC の btst #29,d0 がこれを見る。
    CHECK((st3 & 0x20u) != 0);
    // bit4 = トラック 0。リセット直後はヘッドがそこにいる。
    CHECK((st3 & 0x10u) != 0);
    // bit3 = 両面ドライブ。
    CHECK((st3 & 0x08u) != 0);
    // bit6 = ライトプロテクト。書き込めるイメージなので落ちている。
    CHECK((st3 & 0x40u) == 0);
    CHECK(isIdle(fdc));
}

TEST_CASE("ライトプロテクトは ST3 の bit6 に出る")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;
    floppy.writeProtected = true;
    fdc.setImage(0, &floppy);

    sendCommand(fdc, {0x04, 0x00});
    const x68k::u8 st3 = fdc.readData();
    CHECK((st3 & 0x20u) != 0);  // レディではある
    CHECK((st3 & 0x40u) != 0);  // が、書き込めない
}

TEST_CASE("イメージを外すとレディが落ちる")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;
    fdc.setImage(0, &floppy);

    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readData() & 0x20u) != 0);

    // ディスクを抜く。isPresent が false になるだけで、ドライブ自体は残る。
    floppy.present = false;
    sendCommand(fdc, {0x04, 0x00});
    const x68k::u8 st3 = fdc.readData();
    CHECK((st3 & 0x20u) == 0);
    // ドライブは繋がっているので両面ビットは立ったまま。
    CHECK((st3 & 0x08u) != 0);
}

TEST_CASE("ドライブ 1 のイメージはドライブ 0 と混ざらない")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;
    fdc.setImage(1, &floppy);

    // ドライブ 0 にはディスクが無い。
    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readData() & 0x20u) == 0);

    // ドライブ 1 にはある。
    sendCommand(fdc, {0x04, 0x01});
    const x68k::u8 st3 = fdc.readData();
    CHECK((st3 & 0x20u) != 0);
    CHECK((st3 & 0x03u) == 0x01u);
}

TEST_CASE("SEEK はシリンダを動かし SENSE INTERRUPT STATUS がそれを返す")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;
    fdc.setImage(0, &floppy);

    // SEEK ($0F)、HD|US = 0、NCN = 10。
    sendCommand(fdc, {0x0F, 0x00, 0x0A});
    CHECK(isIdle(fdc));
    CHECK(fdc.hasInterrupt());

    sendCommand(fdc, {0x08});
    const x68k::u8 st0 = fdc.readData();
    const x68k::u8 pcn = fdc.readData();
    // bit5 = シーク終了、bit7-6 = 00 が正常終了。
    CHECK((st0 & 0x20u) != 0);
    CHECK((st0 & 0xC0u) == 0x00u);
    CHECK(pcn == 10);
    CHECK(isIdle(fdc));

    // シリンダが動いたので Track0 は落ちる。
    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readData() & 0x10u) == 0);
}

TEST_CASE("RECALIBRATE はトラック 0 へ戻す")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;
    fdc.setImage(0, &floppy);

    sendCommand(fdc, {0x0F, 0x00, 0x28});  // SEEK 40
    sendCommand(fdc, {0x08});
    (void)fdc.readData();
    CHECK(fdc.readData() == 40);

    sendCommand(fdc, {0x07, 0x00});  // RECALIBRATE
    sendCommand(fdc, {0x08});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0x20u) != 0);      // シーク終了
    CHECK((st0 & 0xC0u) == 0x00u);  // 正常終了 (メディアがあるので)
    CHECK((st0 & 0x10u) == 0);      // 装置チェックではない
    CHECK(fdc.readData() == 0);     // トラック 0 に戻った
    CHECK(isIdle(fdc));
}

TEST_CASE("範囲外のシリンダへの SEEK は異常終了しヘッドを動かさない")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;  // 77 シリンダ (0-76)
    fdc.setImage(0, &floppy);

    sendCommand(fdc, {0x0F, 0x00, 0x0A});  // まず 10 へ
    sendCommand(fdc, {0x08});
    (void)fdc.readData();
    (void)fdc.readData();

    sendCommand(fdc, {0x0F, 0x00, 0x4D});  // 77 は範囲外
    sendCommand(fdc, {0x08});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0x40u);  // 異常終了

    // ヘッドは動いていない。動かすと以後の SENSE DRIVE STATUS が
    // 「存在しない位置」を返し続ける。
    CHECK(fdc.readData() == 10);
}

TEST_CASE("READ ID は結果フェーズを残さずコマンド待ちへ戻る")
{
    x68k::Fdc fdc;
    fdc.reset();
    FakeFloppy floppy;
    fdc.setImage(0, &floppy);

    sendCommand(fdc, {0x0F, 0x00, 0x05});  // SEEK 5
    sendCommand(fdc, {0x08});
    (void)fdc.readData();
    (void)fdc.readData();

    // READ ID ($4A)。IPL-ROM はこの結果をその場では読まず、コマンド送出の
    // あと $FF8A50 → $FF9014 → $FF9006 で「下位 5bit が 0」を待ち、実際の
    // 7 バイトは割り込みハンドラ ($FF1130) が積む $C90 から引く。
    // 本エミュレータは FDC の割り込み線を配線していないので、ここで結果を
    // 積むと誰も読まないまま CB が立ち続け、$FF9006 が永久に回る。
    //
    // これは実際に踏んだ: 空の 2HD を挿すと SPECIFY → SENSE DRIVE STATUS
    // → RECALIBRATE → SEEK と進み、READ ID で status=$D0 のまま停止した。
    sendCommand(fdc, {0x4A, 0x00});
    CHECK(isIdle(fdc));
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusCb) == 0);

    // 割り込みだけ上げる。SENSE INTERRUPT STATUS で正常終了を受け取れる。
    CHECK(fdc.hasInterrupt());
    sendCommand(fdc, {0x08});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0x00u);
    CHECK(fdc.readData() == 5);  // シークした先のシリンダ
    CHECK(isIdle(fdc));
}

TEST_CASE("イメージが無ければ READ ID は異常終了として割り込みを上げる")
{
    x68k::Fdc fdc;
    fdc.reset();

    sendCommand(fdc, {0x4A, 0x00});
    // 結果を積まないこと。積むと $FF9006 が回り続ける。
    CHECK(isIdle(fdc));

    sendCommand(fdc, {0x08});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0x40u);
    CHECK((st0 & 0x08u) != 0);  // ノットレディ
    (void)fdc.readData();
    CHECK(isIdle(fdc));
}

// --- イメージ有り: DMA 転送 (Machine + DMAC 経由) ----------------------------

TEST_CASE("READ DATA が DMAC のチャネル 0 経由で 1 セクタ運ぶ")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kDest = 0x002000;
    constexpr x68k::u32 kSectorSize = 1024;

    // ドライブ 0 を選ぶ ($FF909E が $E94007 へ書くのと同じ)。
    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);

    // READ DATA ($46 = MF|READ。MT は bit7 なので $46 には入っていない)、
    // HD|US=0、C=0、H=0、R=1、N=3、EOT=8。
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});

    // 実行フェーズに入り、DIO (FDC → CPU) が立っている。
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & x68k::Fdc::kStatusCb) != 0);
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & x68k::Fdc::kStatusDio) != 0);

    rig.startDma(kDest, kSectorSize, true);

    // $FF9014 の判定。COC が立ち ERR が落ちていれば成功。
    CHECK(rig.dmaSucceeded());

    // 中身が LBA 0 のセクタと一致する。
    const x68k::u8* expected = floppy.sector(floppy.lbaOf(0, 0, 1));
    CHECK(std::memcmp(rig.mainRam.data() + kDest, expected, kSectorSize) == 0);
    // セクタ長を取り違えていないこと。末尾の印まで届いている。
    CHECK(rig.mainRam[kDest + kSectorSize - 1] == 0x5A);
    CHECK(floppy.readCount == 1);

    // 転送が終わったらコマンド待ちへ戻る。結果を積むと $FF9036 が止まる。
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
}

TEST_CASE("READ DATA が複数セクタを連続して運ぶ")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kDest = 0x004000;
    constexpr x68k::u32 kSectorSize = 1024;
    constexpr x68k::u32 kSectors = 4;

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // R=1 から EOT=8 まで。DMAC のターミナルカウント (4 セクタぶん) で
    // 止まる。FDC 側が EOT まで進み切ってしまうと 8 セクタ読んでしまう。
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, kSectorSize * kSectors, true);

    CHECK(rig.dmaSucceeded());
    CHECK(floppy.readCount == static_cast<int>(kSectors));

    // R=1..4 が順に並んでいる。1 セクタずれていたらここで落ちる。
    for (x68k::u32 s = 0; s < kSectors; ++s)
    {
        const x68k::u8* expected = floppy.sector(floppy.lbaOf(0, 0, s + 1));
        CHECK(std::memcmp(rig.mainRam.data() + kDest + s * kSectorSize, expected, kSectorSize) ==
              0);
    }
}

TEST_CASE("MT が立っていれば EOT の先でヘッドを替えて続ける")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kDest = 0x006000;
    constexpr x68k::u32 kSectorSize = 1024;

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // $C6 = MT|MF|READ。MT は bit7 なので、$46 (MF|READ) では立たない。
    // R=7 から始めて EOT=8。表面に残るのは 2 セクタなので、3 セクタ要求すると
    // 3 つ目は裏面 (H=1) の R=1 になる。
    rig.sendFdcCommand({0xC6, 0x00, 0x00, 0x00, 0x07, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, kSectorSize * 3, true);

    CHECK(rig.dmaSucceeded());
    CHECK(std::memcmp(rig.mainRam.data() + kDest, floppy.sector(floppy.lbaOf(0, 0, 7)),
                      kSectorSize) == 0);
    CHECK(std::memcmp(rig.mainRam.data() + kDest + kSectorSize,
                      floppy.sector(floppy.lbaOf(0, 0, 8)), kSectorSize) == 0);
    // 3 つ目が裏面の先頭。ここが表面の R=1 になっていたら MT の実装が
    // ヘッドを替えず R だけ巻き戻している。
    CHECK(std::memcmp(rig.mainRam.data() + kDest + 2 * kSectorSize,
                      floppy.sector(floppy.lbaOf(0, 1, 1)), kSectorSize) == 0);
}

TEST_CASE("MT が無ければ EOT を超えた時点で転送が止まる")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kDest = 0x008000;
    constexpr x68k::u32 kSectorSize = 1024;

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // MT 無しの READ DATA ($06)。R=8 = EOT なので 1 セクタで終わる。
    rig.sendFdcCommand({0x06, 0x00, 0x00, 0x00, 0x08, 0x03, 0x08, 0x35, 0xFF});
    // 2 セクタ要求する。1 セクタで尽きるので DMAC は打ち切る。
    rig.startDma(kDest, kSectorSize * 2, true);

    // 要求量に届いていないので COC は立たず ERR が立つ。
    // ここで COC が立つと、転送前のメモリを読めたつもりで先へ進む。
    const x68k::u8 csr = rig.dmaCsr();
    CHECK((csr & x68k::Dmac::kCsrChannelOperationComplete) == 0);
    CHECK((csr & x68k::Dmac::kCsrError) != 0);

    // 流れた 1 セクタは正しい。
    CHECK(std::memcmp(rig.mainRam.data() + kDest, floppy.sector(floppy.lbaOf(0, 0, 8)),
                      kSectorSize) == 0);
    // 2 セクタ目は触られていない。
    CHECK(rig.mainRam[kDest + kSectorSize] == 0);
    // FDC はコマンド待ちへ戻っている。
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
}

TEST_CASE("WRITE DATA が DMAC 経由でセクタを書き戻す")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kSrc = 0x00A000;
    constexpr x68k::u32 kSectorSize = 1024;

    // 書き込む中身をメモリに用意する。
    for (x68k::u32 i = 0; i < kSectorSize; ++i)
    {
        rig.mainRam[kSrc + i] = static_cast<x68k::u8>(i ^ 0x3C);
    }

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // WRITE DATA ($45 = MF|WRITE)、C=0、R=3、N=3、EOT=8。
    rig.sendFdcCommand({0x45, 0x00, 0x00, 0x00, 0x03, 0x03, 0x08, 0x35, 0xFF});

    // 実行フェーズ。書き込みなので DIO は落ちている (CPU → FDC)。
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & x68k::Fdc::kStatusCb) != 0);
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & x68k::Fdc::kStatusDio) == 0);

    rig.startDma(kSrc, kSectorSize, false);

    CHECK(rig.dmaSucceeded());
    CHECK(floppy.writeCount == 1);

    // イメージ側の R=3 のセクタが書き換わっている。
    const x68k::u8* written = floppy.sector(floppy.lbaOf(0, 0, 3));
    CHECK(std::memcmp(written, rig.mainRam.data() + kSrc, kSectorSize) == 0);

    // 隣のセクタ (R=2) は無傷。1 セクタずれて書くと静かなデータ破壊になる。
    CHECK(floppy.sector(floppy.lbaOf(0, 0, 2))[2] == 0xA5);
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
}

TEST_CASE("WRITE DATA が複数セクタを書き戻す")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kSrc = 0x00C000;
    constexpr x68k::u32 kSectorSize = 1024;
    constexpr x68k::u32 kSectors = 3;

    for (x68k::u32 i = 0; i < kSectorSize * kSectors; ++i)
    {
        rig.mainRam[kSrc + i] = static_cast<x68k::u8>((i * 7) & 0xFF);
    }

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x45, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kSrc, kSectorSize * kSectors, false);

    CHECK(rig.dmaSucceeded());
    // ここが 1 だと「最初のセクタしか書かない」退行。SASI で一度やった
    // 壊れ方で、ファイルシステムが静かに壊れる。
    CHECK(floppy.writeCount == static_cast<int>(kSectors));

    for (x68k::u32 s = 0; s < kSectors; ++s)
    {
        CHECK(std::memcmp(floppy.sector(floppy.lbaOf(0, 0, s + 1)),
                          rig.mainRam.data() + kSrc + s * kSectorSize, kSectorSize) == 0);
    }
}

TEST_CASE("セクタの途中で DMA が終わったら書き戻さない")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kSrc = 0x01C000;
    constexpr x68k::u32 kSectorSize = 1024;

    for (x68k::u32 i = 0; i < kSectorSize; ++i)
    {
        rig.mainRam[kSrc + i] = 0xEE;
    }

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x45, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    // 半端な長さ (セクタ長の半分) で止める。
    rig.startDma(kSrc, kSectorSize / 2, false);

    // 前半だけが新しく後半が古いセクタを作らない。ファイルシステムから
    // 見ると「読めるが中身が混ざっている」状態になり、エラーとして
    // 表面化しないまま壊れる。
    CHECK(floppy.writeCount == 0);
    CHECK(floppy.sector(floppy.lbaOf(0, 0, 1))[0] == 0);
    CHECK(floppy.sector(floppy.lbaOf(0, 0, 1))[2] == 0xA5);

    // それでも FDC はコマンド待ちへ戻る。
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);

    // 状態は異常終了。要求された長さを運べていないので、正常終了を
    // 返すとホストは「書けたつもり」で先へ進む。
    rig.sendFdcCommand({0x08});
    const x68k::u8 st0 = rig.machine.ioRead8(kFdcDataPort);
    CHECK((st0 & 0xC0u) == 0x40u);
    (void)rig.machine.ioRead8(kFdcDataPort);
}

TEST_CASE("セクタ境界で止まった書き込みも異常終了として残る")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kSrc = 0x020000;
    constexpr x68k::u32 kSectorSize = 1024;

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // R=8 = EOT なので 1 セクタで尽きる。2 セクタ要求すると、1 セクタ目は
    // きっちり書けたうえで DMAC が要求量に届かず打ち切られる。
    rig.sendFdcCommand({0x05, 0x00, 0x00, 0x00, 0x08, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kSrc, kSectorSize * 2, false);

    CHECK(floppy.writeCount == 1);
    CHECK((rig.dmaCsr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);

    rig.sendFdcCommand({0x08});
    const x68k::u8 st0 = rig.machine.ioRead8(kFdcDataPort);
    CHECK((st0 & 0xC0u) == 0x40u);
    (void)rig.machine.ioRead8(kFdcDataPort);
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
}

TEST_CASE("セクタ境界ちょうどで終わった転送は正常終了として残る")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(0x01E000, 1024 * 2, true);
    CHECK(rig.dmaSucceeded());

    // 転送そのものが割り込みを上げる (実機は IRQ レベル 1)。
    // SENSE INTERRUPT STATUS が正常終了を返せること。ここが異常終了だと
    // ROM 側は読めたデータを捨てて再試行に回る。
    CHECK(rig.machine.fdc().hasInterrupt());
    rig.sendFdcCommand({0x08});
    const x68k::u8 st0 = rig.machine.ioRead8(kFdcDataPort);
    CHECK((st0 & 0xC0u) == 0x00u);
    (void)rig.machine.ioRead8(kFdcDataPort);
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
}

TEST_CASE("ライトプロテクトされたイメージへは書けない")
{
    Rig rig;
    FakeFloppy floppy;
    floppy.writeProtected = true;
    rig.machine.setFloppyDisk(0, &floppy);

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x45, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});

    // 実行フェーズに入らず即コマンド待ちへ戻る。
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);

    rig.startDma(0x00E000, 1024, false);
    // 1 バイトも受け取れないので COC は立たない。
    CHECK((rig.dmaCsr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
    CHECK(floppy.writeCount == 0);
}

TEST_CASE("シリンダが合っていない READ DATA は失敗する")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // SEEK していないのでヘッドはシリンダ 0。C=5 を要求しても ID が合わない。
    // ここを素通りさせると、シークせずに読んだ別トラックの中身を
    // 目的のセクタとして返してしまう。
    rig.sendFdcCommand({0x46, 0x00, 0x05, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});

    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
    rig.startDma(0x010000, 1024, true);
    CHECK((rig.dmaCsr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
    CHECK(floppy.readCount == 0);
}

TEST_CASE("SEEK した先のシリンダなら READ DATA が通る")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kDest = 0x012000;
    constexpr x68k::u32 kSectorSize = 1024;

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x0F, 0x00, 0x05});  // SEEK 5
    rig.sendFdcCommand({0x08});              // SENSE INTERRUPT STATUS
    (void)rig.machine.ioRead8(kFdcDataPort);
    (void)rig.machine.ioRead8(kFdcDataPort);

    rig.sendFdcCommand({0x46, 0x00, 0x05, 0x00, 0x02, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, kSectorSize, true);

    CHECK(rig.dmaSucceeded());
    CHECK(std::memcmp(rig.mainRam.data() + kDest, floppy.sector(floppy.lbaOf(5, 0, 2)),
                      kSectorSize) == 0);
}

TEST_CASE("N がイメージのセクタ長と食い違えば転送しない")
{
    Rig rig;
    FakeFloppy floppy;  // 1024 バイト/セクタ (N=3)
    rig.machine.setFloppyDisk(0, &floppy);

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // N=2 (512 バイト) を要求する。黙って 1024 で読むと、転送量と
    // セクタ境界がずれてファイルシステムが静かに壊れる。
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x01, 0x02, 0x08, 0x35, 0xFF});

    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
    rig.startDma(0x014000, 512, true);
    CHECK((rig.dmaCsr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
    CHECK(floppy.readCount == 0);
}

TEST_CASE("範囲外のセクタ番号は読めない")
{
    Rig rig;
    FakeFloppy floppy;  // 1 トラック 8 セクタ (R は 1-8)
    rig.machine.setFloppyDisk(0, &floppy);

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    // R=9 は存在しない。EOT も 9 にして FDC 側の EOT 判定を素通りさせ、
    // イメージ側の範囲検査だけに委ねる。
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x09, 0x03, 0x09, 0x35, 0xFF});
    rig.startDma(0x016000, 1024, true);

    CHECK((rig.dmaCsr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);
}

TEST_CASE("イメージが無いドライブへの READ DATA は DMA を空振りさせる")
{
    Rig rig;
    // イメージを繋がない。従来のスタブと同じ振る舞いに留まること。

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0x1Fu) == 0);

    rig.startDma(0x018000, 1024, true);
    CHECK((rig.dmaCsr() & x68k::Dmac::kCsrChannelOperationComplete) == 0);
    CHECK(rig.mainRam[0x018000] == 0);

    // 次のコマンドを送れる。ここが塞がると $FF9036 が永久に止まる。
    rig.sendFdcCommand({0x04, 0x00});
    CHECK((rig.machine.ioRead8(kFdcStatusPort) & 0xD0u) == 0xD0u);
}

TEST_CASE("コマンドの US フィールドが転送先のドライブを決める")
{
    Rig rig;
    FakeFloppy fd0;
    FakeFloppy fd1;
    rig.machine.setFloppyDisk(0, &fd0);
    rig.machine.setFloppyDisk(1, &fd1);

    constexpr x68k::u32 kDest = 0x022000;
    constexpr x68k::u32 kSectorSize = 1024;

    // $FF909E は「$80 | ドライブ番号」を $E94007 へ書いてからコマンドを送る。
    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x81);

    // READ DATA。HD|US の US は 1 (ドライブ 1)。
    // 実機の uPD72065 はコマンドの US で選ぶ。IPL-ROM も $FF8FEE の
    // and.b #$3,d1 でドライブ番号をコマンドの 2 バイト目へ入れてから送る。
    // ここを見ずに $E94007 だけで選ぶ実装にすると、US だけを差し替えて
    // 2 台目を読むコード (Human68k がやる) が常に 1 台目を読む。
    rig.sendFdcCommand({0x46, 0x01, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, kSectorSize, true);

    CHECK(rig.dmaSucceeded());
    CHECK(fd1.readCount == 1);
    CHECK(fd0.readCount == 0);
    CHECK(std::memcmp(rig.mainRam.data() + kDest, fd1.sector(fd1.lbaOf(0, 0, 1)), kSectorSize) ==
          0);

    // US = 0 に戻せばドライブ 0 を見る。$E94007 の値 (1) は上書きされる。
    rig.sendFdcCommand({0x04, 0x00});
    const x68k::u8 st3 = rig.machine.ioRead8(kFdcDataPort);
    CHECK((st3 & 0x03u) == 0x00u);
}

TEST_CASE("選択ポートと US が食い違ったら US が勝つ")
{
    Rig rig;
    FakeFloppy fd0;
    FakeFloppy fd1;
    rig.machine.setFloppyDisk(0, &fd0);
    rig.machine.setFloppyDisk(1, &fd1);

    constexpr x68k::u32 kDest = 0x028000;
    constexpr x68k::u32 kSectorSize = 1024;

    // ポートではドライブ 1 を選んでおきながら、コマンドの US は 0。
    // uPD72065 はコマンドの US で選ぶので、読まれるのはドライブ 0。
    // ポート側を優先する実装だと、Human68k が US だけを差し替えて
    // ドライブを切り替えるときに、必ず直前のドライブを読む。
    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x81);
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, kSectorSize, true);

    CHECK(rig.dmaSucceeded());
    CHECK(fd0.readCount == 1);
    CHECK(fd1.readCount == 0);

    // 逆向きも見る。ポートで 0 を選んで US に 1 を入れたらドライブ 1。
    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x46, 0x01, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, kSectorSize, true);

    CHECK(rig.dmaSucceeded());
    CHECK(fd1.readCount == 1);
    CHECK(fd0.readCount == 1);  // 増えていない
}

TEST_CASE("裏面 (ヘッド 1) への書き込みが表面を壊さない")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kSrc = 0x02A000;
    constexpr x68k::u32 kSectorSize = 1024;

    for (x68k::u32 i = 0; i < kSectorSize; ++i)
    {
        rig.mainRam[kSrc + i] = 0x77;
    }

    // HD|US = $04 (ヘッド 1、ドライブ 0)。C=0、R=1。
    rig.sendFdcCommand({0x45, 0x04, 0x00, 0x01, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kSrc, kSectorSize, false);

    CHECK(rig.dmaSucceeded());
    CHECK(floppy.writeCount == 1);

    // 裏面の (0,1,1) が書き換わった。
    const x68k::u8* back = floppy.sector(floppy.lbaOf(0, 1, 1));
    CHECK(back[0] == 0x77);
    CHECK(back[kSectorSize - 1] == 0x77);

    // 表面の (0,0,1) は無傷。ヘッドを落として書くと、裏面へ書いたつもりの
    // データが表面を潰す。ブートセクタが載っている面なので致命的。
    const x68k::u8* front = floppy.sector(floppy.lbaOf(0, 0, 1));
    CHECK(front[0] == 0x00);
    CHECK(front[2] == 0xA5);
    CHECK(front[kSectorSize - 1] == 0x5A);
}

TEST_CASE("裏面 (ヘッド 1) からの読み出しが表面と混ざらない")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    constexpr x68k::u32 kDest = 0x02C000;
    constexpr x68k::u32 kSectorSize = 1024;

    rig.sendFdcCommand({0x46, 0x04, 0x00, 0x01, 0x03, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, kSectorSize, true);

    CHECK(rig.dmaSucceeded());
    CHECK(std::memcmp(rig.mainRam.data() + kDest, floppy.sector(floppy.lbaOf(0, 1, 3)),
                      kSectorSize) == 0);
    // 表面の同じ R とは違う中身であること (テストが区別できている証拠)。
    CHECK(std::memcmp(rig.mainRam.data() + kDest, floppy.sector(floppy.lbaOf(0, 0, 3)),
                      kSectorSize) != 0);
}

TEST_CASE("ドライブごとのシリンダ位置は混ざらない")
{
    Rig rig;
    FakeFloppy fd0;
    FakeFloppy fd1;
    rig.machine.setFloppyDisk(0, &fd0);
    rig.machine.setFloppyDisk(1, &fd1);

    // 2 台を別々のシリンダへシークする。
    rig.sendFdcCommand({0x0F, 0x01, 0x14});  // ドライブ 1 → 20
    rig.sendFdcCommand({0x08});
    const x68k::u8 st0 = rig.machine.ioRead8(kFdcDataPort);
    CHECK((st0 & 0x03u) == 0x01u);
    CHECK(rig.machine.ioRead8(kFdcDataPort) == 20);

    rig.sendFdcCommand({0x0F, 0x00, 0x05});  // ドライブ 0 → 5
    rig.sendFdcCommand({0x08});
    (void)rig.machine.ioRead8(kFdcDataPort);
    CHECK(rig.machine.ioRead8(kFdcDataPort) == 5);

    // ドライブ 1 は 20 のまま。1 つの変数で持っていると、片方を
    // シークしただけでもう片方の READ DATA がシリンダ照合に落ちる。
    rig.sendFdcCommand({0x04, 0x01});
    CHECK((rig.machine.ioRead8(kFdcDataPort) & 0x10u) == 0);  // Track0 ではない

    // ドライブ 1 のシリンダ 20 は読める (照合が通る)。
    constexpr x68k::u32 kDest = 0x026000;
    rig.sendFdcCommand({0x46, 0x01, 0x14, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(kDest, 1024, true);
    CHECK(rig.dmaSucceeded());
    CHECK(std::memcmp(rig.mainRam.data() + kDest, fd1.sector(fd1.lbaOf(20, 0, 1)), 1024) == 0);
}

TEST_CASE("FDC の DMA は SASI のチャネル 1 を巻き込まない")
{
    Rig rig;
    FakeFloppy floppy;
    rig.machine.setFloppyDisk(0, &floppy);

    // チャネル 1 (SASI) に COC を立てておく。
    // 転送し切れば立つビットなので、FDC の転送で落ちてはいけない。
    constexpr x68k::u32 kDmac1Csr = 0xE84040;

    rig.machine.ioWrite8(kFdcDriveSelectPort, 0x80);
    rig.sendFdcCommand({0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF});
    rig.startDma(0x01A000, 1024, true);
    CHECK(rig.dmaSucceeded());

    // チャネル 1 は誰も起動していないので COC は立っていない。
    // ここが立っていたら、チャネル 0 の転送がチャネル 1 のレジスタを
    // 書いている (= レジスタバンクが分かれていない)。
    CHECK((rig.machine.ioRead8(kDmac1Csr) & x68k::Dmac::kCsrChannelOperationComplete) == 0);
}

TEST_CASE("結果をインラインで読まれないコマンドは CB を立てたまま終わらない")
{
    // IPL-ROM がコマンドの結果をその場で読むのは 2 つだけ:
    //   SENSE DRIVE STATUS      ($FF89DE が $D0 を待って 1 バイト読む)
    //   SENSE INTERRUPT STATUS  ($FF9036 の直後に読む)
    // それ以外はすべて $FF9014 → $FF9006 の「下位 5bit が 0」を完了条件に
    // しており、実際の結果は FDC 割り込みハンドラ ($FF1130) が $C90 へ
    // 積む。本エミュレータはその割り込み線を配線していないので、
    // 結果フェーズを積んだ時点で $FF9006 が永久に回る。
    //
    // 個別のコマンドごとに書くと、新しいコマンドを足したときに
    // 検査を書き忘れる。実際 READ ID がそれで抜けて起動が止まった。
    // ここで機械的に全部通す。
    struct Case
    {
        const char* name;
        std::vector<x68k::u8> bytes;
    };
    const Case cases[] = {
        {"SPECIFY", {0x03, 0xD0, 0x10}},
        {"RECALIBRATE", {0x07, 0x00}},
        {"SEEK", {0x0F, 0x00, 0x05}},
        {"READ ID", {0x4A, 0x00}},
        {"READ DATA", {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF}},
        {"WRITE DATA", {0x45, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF}},
        {"READ DELETED", {0x4C, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF}},
        {"WRITE DELETED", {0x49, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF}},
        {"READ TRACK", {0x42, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x35, 0xFF}},
        {"WRITE ID", {0x4D, 0x00, 0x03, 0x08, 0x35, 0xE5}},
    };

    for (const Case& c : cases)
    {
        CAPTURE(c.name);

        // ディスクあり・なしの両方で確かめる。イメージを繋いだ側だけを
        // 見ていると、繋いでいないときの経路が壊れていても気付かない。
        for (int withImage = 0; withImage < 2; ++withImage)
        {
            CAPTURE(withImage);
            x68k::Fdc fdc;
            FakeFloppy floppy;
            fdc.reset();
            if (withImage != 0)
            {
                fdc.setImage(0, &floppy);
            }

            for (const x68k::u8 byte : c.bytes)
            {
                fdc.writeData(byte);
            }

            // READ/WRITE DATA は実行フェーズに入りうる。そこは DMAC が
            // 畳むので、DMAC が動かなかった場合を模して打ち切りを通知する。
            if (fdc.isExecuting())
            {
                fdc.dmaComplete(false);
            }

            // ここが $10 (CB) や $D0 (結果) だと $FF9006 / $FF904C で止まる。
            CHECK((fdc.readStatus() & 0x1Fu) == 0);

            // 続けて次のコマンドを送れること。
            fdc.writeData(0x04);
            fdc.writeData(0x00);
            CHECK((fdc.readStatus() & 0xD0u) == 0xD0u);
        }
    }
}

TEST_CASE("reset してもイメージは外れない")
{
    x68k::Fdc fdc;
    FakeFloppy floppy;
    fdc.setImage(0, &floppy);
    fdc.reset();

    // 実機の電源を入れ直してもドライブに入れたディスクは入ったまま。
    // ここで外れると、リセット後の起動で必ず「FD なし」になる。
    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readData() & 0x20u) != 0);
}
