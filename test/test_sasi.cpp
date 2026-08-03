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

// SASI の転送バッファ。Machine は自前で持たないので、テストでも与える。
//
// 実機では PSRAM から渡す。65KB を Machine に埋め込むと ESP32 の
// 内部 SRAM を圧迫し、IPL-ROM を内部 SRAM へ置けなくなる。
std::vector<x68k::u8>& sasiBuffer()
{
    static std::vector<x68k::u8> buffer(x68k::Machine::kSasiBufferBytes, 0);
    return buffer;
}

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
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 5, 1);
    CHECK(m.ioRead8(kSasiData) == 0x05);
    CHECK(m.ioRead8(kSasiData) == 0xA5);
}

TEST_CASE("ディスクが無ければエラーステータスを返す")
{
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    // setDisk を呼ばない = ディスクなし。

    sendCommand(m, 0x08, 0, 1);

    // ディスクが無ければセレクションが成立しないので、バスフリーのまま。
    // IPL-ROM 側はセレクション待ちがタイムアウトして「装置なし」と判断する。
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueBusFree);
}

TEST_CASE("TEST UNIT READY でディスクの有無が分かる")
{
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
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
    m.setSasiBuffer(sasiBuffer().data());
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

// --- reset() が外から与えた設定を保つか -------------------------------------
//
// 実際に起きた退行: 転送バッファを外部注入に変えたとき、reset() が
// sasi_ を丸ごと初期化してバッファポインタを nullptr に戻してしまい、
// ディスクが 1 セクタも読めなくなった。当時のテストは reset() を
// 呼んでいなかったので、テストは通ったまま実機だけが壊れた。

TEST_CASE("reset() の後も SASI の転送バッファが保たれる")
{
    // 保証すること: reset() を挟んでも READ が成功すること。
    //
    // 壊れると: バッファポインタが nullptr に戻り、READ が必ずエラー
    // ステータスを返す。IPL-ROM はブートセクタを読めず起動しない。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    m.reset();

    sendCommand(m, 0x08, 2, 1);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);
    CHECK(m.ioRead8(kSasiData) == 0x02);
    CHECK(m.ioRead8(kSasiData) == 0xA5);
}

TEST_CASE("reset() を繰り返してもディスクとバッファの設定が残る")
{
    // 保証すること: setSasiBuffer / setDisk は「外から与えた設定」であり、
    // 何度リセットしても消えないこと。
    //
    // 壊れると: 2 回目以降のリセット (ウォームブート) で起動できなくなる。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    m.reset();
    m.reset();

    sendCommand(m, 0x08, 4, 1);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);
    CHECK(m.ioRead8(kSasiData) == 0x04);
}

TEST_CASE("reset() で SASI のフェーズはバスフリーへ戻る")
{
    // 保証すること: 転送の途中でリセットしても状態機械が中途半端な
    // フェーズに残らないこと。
    //
    // 壊れると: リセット後の最初のセレクションが受け付けられず、
    // IPL-ROM のセレクション待ちがタイムアウトする。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    // データインフェーズの途中で止める。
    sendCommand(m, 0x08, 0, 1);
    m.ioRead8(kSasiData);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);

    m.reset();
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueBusFree);

    // リセット後も新しいコマンドを受け付けられる。
    sendCommand(m, 0x08, 1, 1);
    CHECK(m.ioRead8(kSasiData) == 0x01);
}

TEST_CASE("reset() を挟んでもメインメモリの内容は保たれる")
{
    // 保証すること: setMemory() で与えたメモリ実体は reset() で
    // クリアされないこと (実体の所有は呼び出し側)。
    //
    // 壊れると: ESP32 では PSRAM を再確保することになり、
    // 起動直後に一括確保するという前提が崩れる。
    x68k::Machine m;
    std::vector<x68k::u8> ram(x68k::kMainRamSize, 0);
    std::vector<x68k::u8> rom(x68k::kIplromSize, 0);
    x68k::MemoryMap memory;
    memory.mainRam = ram.data();
    memory.iplRom = rom.data();
    m.setMemory(memory);

    ram[0x1234] = 0x99;
    m.reset();

    // リセット直後は ROM が $000000 に写像されているので、
    // 写像を外してから見る。
    m.bus().setRomMappedAtZero(false);
    CHECK(m.bus().read8(0x1234) == 0x99);
}

// --- SASI 状態機械のフェーズ遷移 ---------------------------------------------

TEST_CASE("セレクション直後の 1 回目のステータス読みはバスフリーを返す")
{
    // 保証すること: IPL-ROM は $E96007 へ ID を書いた後、$E96003 の bit1 が
    // 0 になるのを待つ ($FF96DA の BTST #1 → BEQ)。コマンドフェーズの
    // 値 $0B は bit1 が 1 なので、セレクション直後にいきなり $0B を返すと
    // この待ちを抜けられない。
    //
    // 壊れると: IPL-ROM がセレクション待ちのループから出られず、
    // コマンドを 1 バイトも送ってこない。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(4);
    m.setDisk(&disk);

    m.ioWrite8(kSasiSelect, 0x01);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueBusFree);
    // 2 回目でコマンドフェーズへ移る。
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataOut);
}

TEST_CASE("コマンド送出中はコマンドフェーズを保つ")
{
    // 保証すること: 6 バイト揃うまでフェーズが変わらないこと。
    //
    // 壊れると: 途中でフェーズが変わり、IPL-ROM が残りのバイトを
    // 送らなくなる。コマンドが完成しないので何も起きない。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(4);
    m.setDisk(&disk);

    m.ioWrite8(kSasiSelect, 0x01);
    m.ioRead8(kSasiStatus);

    for (int i = 0; i < 5; ++i)
    {
        m.ioWrite8(kSasiData, i == 0 ? 0x08 : 0x00);
        CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataOut);
    }
    // 6 バイト目で実行される。
    m.ioWrite8(kSasiData, 0x00);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);
}

TEST_CASE("メッセージを読み切るとバスフリーへ戻る")
{
    // 保証すること: ステータス → メッセージ → バスフリー という
    // 終わり方をすること。IPL-ROM は 2 バイト読んでバスを解放する。
    //
    // 壊れると: バスが解放されず、次のコマンドのセレクションが
    // 受け付けられない。1 コマンド目だけ動いて 2 コマンド目で固まる。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x00 /* TEST UNIT READY */, 0, 0);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);

    m.ioRead8(kSasiData);  // ステータス
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueMessage);

    m.ioRead8(kSasiData);  // メッセージ
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueBusFree);
}

TEST_CASE("$C2 SPECIFY は 10 バイトのパラメータを受け取ってから完了する")
{
    // 保証すること: IPL-ROM が最初に発行する $C2 が完走すること。
    // 通常のデータアウト ($0B) と違い、IPL-ROM はステータスと同じ $03 を
    // 待ってからパラメータを送ってくる ($FF9910)。
    //
    // 壊れると: IPL-ROM がここで止まり、その先の READ に到達しない。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0xC2, 0, 0);
    CHECK(m.ioRead8(kSasiStatus) == 0x03);

    // 10 バイト送る。最後の 1 バイトで完了する。
    for (int i = 0; i < 9; ++i)
    {
        m.ioWrite8(kSasiData, 0x11);
        CHECK(m.ioRead8(kSasiStatus) == 0x03);
    }
    m.ioWrite8(kSasiData, 0x11);

    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
    CHECK(m.ioRead8(kSasiData) == 0x00);  // 正常終了
}

TEST_CASE("WRITE コマンドがディスクへ書き込む")
{
    // 保証すること: データアウトフェーズで 256 バイト受け取ると、
    // 指定 LBA へ書き込まれること。
    //
    // 壊れると: Human68k からのファイル書き込みが黙って捨てられる。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x0A /* WRITE */, 7, 1);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataOut);

    for (int i = 0; i < 256; ++i)
    {
        m.ioWrite8(kSasiData, static_cast<x68k::u8>(i ^ 0x5A));
    }

    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
    CHECK(disk.data_[7 * 256 + 0] == 0x5A);
    CHECK(disk.data_[7 * 256 + 1] == (1 ^ 0x5A));
    CHECK(disk.data_[7 * 256 + 255] == (255 ^ 0x5A));
    // 他のセクタは触られていない。
    CHECK(disk.data_[6 * 256] == 0x06);
}

// --- READ のセクタ数クランプ -------------------------------------------------
//
// 実際に起きた退行: 要求がバッファに収まらないとき黙って切り詰めており、
// 転送量と bufferLength がずれて「DMA が途中で止まったまま成功に見える」
// 状態を作っていた。

TEST_CASE("READ の count=0 は 1 セクタを意味する")
{
    // 保証すること: SASI のコマンド長フィールドの 0 は 256 ではなく
    // 1 セクタとして扱われること。
    //
    // 壊れると: 0 セクタ = 転送量 0 となり、DMA が 1 バイトも動かないまま
    // 「完了」する。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 3, 0);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);
    CHECK(m.ioRead8(kSasiData) == 0x03);

    // ちょうど 1 セクタ (256 バイト) でステータスへ移る。
    for (int i = 1; i < 256; ++i)
    {
        m.ioRead8(kSasiData);
    }
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
}

TEST_CASE("READ はバッファに収まる最大セクタ数を扱える")
{
    // 保証すること: 上限ちょうど (255 セクタ) は切り詰めずに転送できること。
    //
    // 壊れると: 境界を 1 つ間違えて上限ちょうどを弾いてしまい、
    // 大きな読み出しが理由なく失敗する。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(512);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 0, static_cast<x68k::u8>(x68k::Machine::kSasiMaxSectorsPerCommand));
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);

    // 255 セクタぶん (65280 バイト) を読み切るまでデータインのまま。
    const int total = 256 * static_cast<int>(x68k::Machine::kSasiMaxSectorsPerCommand);
    for (int i = 0; i < total - 1; ++i)
    {
        m.ioRead8(kSasiData);
    }
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataIn);
    m.ioRead8(kSasiData);  // 最後の 1 バイト
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
}

TEST_CASE("ディスクの範囲外を読もうとするとエラーになる")
{
    // 保証すること: 読めなかったときは黙って成功にせず、
    // エラーステータスでステータスフェーズへ移ること。
    //
    // 壊れると: ゴミの入ったバッファを「読めた」として返し、
    // ブートセクタの検査が通らない理由が分からなくなる。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(4);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 3, 4);  // セクタ 3 から 4 つ = 範囲外
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
    CHECK(m.ioRead8(kSasiData) != 0x00);
    CHECK(disk.readCount == 0);
}

TEST_CASE("転送バッファを与えないと READ はエラーを返す")
{
    // 保証すること: バッファ未設定を「成功」と誤認しないこと。
    // これは reset() がバッファを消してしまった退行と同じ症状になる。
    //
    // 壊れると: nullptr へ書き込もうとするか、0 バイトを転送して
    // 「読めた」ことにしてしまう。
    x68k::Machine m;
    // setSasiBuffer を呼ばない。
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x08, 0, 1);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
    CHECK(m.ioRead8(kSasiData) != 0x00);
}

TEST_CASE("WRITE は要求されたぶんのセクタをまとめて書く")
{
    // 保証すること: 複数セクタの WRITE で、要求された全セクタが
    // ディスクへ届くこと。
    //
    // 壊れると: 最初の 1 セクタだけ書いて成功を返す。Human68k は
    // 書けたつもりで先へ進むので、ファイルシステムが静かに壊れる。
    // READ 側は count を見ているのに WRITE だけ 1 セクタ固定だった。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x0A, 4, 3);  // セクタ 4 から 3 つ書く
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataOut);

    // 3 セクタぶん (768 バイト) を送る。各セクタの先頭に印を置く。
    constexpr x68k::u32 kSectors = 3;
    for (x68k::u32 s = 0; s < kSectors; ++s)
    {
        for (x68k::u32 i = 0; i < FakeDisk::kSectorSize; ++i)
        {
            const auto value = static_cast<x68k::u8>(i == 0 ? (0x40u + s) : 0x11u);
            m.ioWrite8(kSasiData, value);
        }
    }

    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
    CHECK(m.ioRead8(kSasiData) == 0x00);  // 成功

    // 3 セクタとも書かれているか。2 つ目以降が元のままなら退行。
    for (x68k::u32 s = 0; s < kSectors; ++s)
    {
        const std::size_t offset = (4 + s) * FakeDisk::kSectorSize;
        CHECK(disk.data_[offset] == static_cast<x68k::u8>(0x40u + s));
        CHECK(disk.data_[offset + 1] == 0x11);
    }
}

TEST_CASE("WRITE が失敗したらエラーステータスを返す")
{
    // 保証すること: 書き込めなかったことが呼び出し元へ伝わること。
    //
    // 壊れると: 読み取り専用のイメージや I/O エラーでも成功に見え、
    // Human68k は書けたつもりで進む。あとから読み直すまで気付けない。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(4);
    m.setDisk(&disk);

    // セクタ 3 から 2 つ = 範囲外なので writeSector が false を返す。
    sendCommand(m, 0x0A, 3, 2);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataOut);

    for (x68k::u32 i = 0; i < FakeDisk::kSectorSize * 2; ++i)
    {
        m.ioWrite8(kSasiData, 0x99);
    }

    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
    CHECK(m.ioRead8(kSasiData) != 0x00);  // エラー
}

TEST_CASE("WRITE は扱える上限ちょうどの要求を受け付ける")
{
    // 保証すること: 255 セクタ (バッファの上限) の要求を弾かないこと。
    //
    // 壊れると: 上限の判定が「以上」と「より大きい」を取り違えていると、
    // 正当な最大サイズの要求がエラーになる。
    //
    // Why not 上限を超えた場合を試さないか: コマンドの count は 8bit で
    // 最大 255、kSasiMaxSectorsPerCommand も 255 なので、SASI のコマンドから
    // 上限を超える要求は作れない。上限の検査そのものは READ と揃えるために
    // 残してあるが、この経路からは到達しない。
    static_assert(x68k::Machine::kSasiMaxSectorsPerCommand == 255,
                  "count が 8bit である以上、上限は 255 を超えられない");

    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(512);
    m.setDisk(&disk);

    sendCommand(m, 0x0A, 0, 255);
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataOut);
}

TEST_CASE("DMA 経由の WRITE も要求されたぶんのセクタを書く")
{
    // 保証すること: DMA でデータを送る経路でも、複数セクタの WRITE が
    // 全セクタ届くこと。
    //
    // 壊れると: 最初の 1 セクタだけ書いて成功を返す。CPU が 1 バイトずつ
    // 書く経路と同じ欠陥だが、コードが二重にあったため片方だけ直っていた。
    // Human68k は DMA を使うので、実際に踏むのはこちら。
    x68k::Machine m;
    m.setSasiBuffer(sasiBuffer().data());
    FakeDisk disk(16);
    m.setDisk(&disk);

    sendCommand(m, 0x0A, 6, 2);  // セクタ 6 から 2 つ
    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueDataOut);

    // DMAC が dmaWrite で 1 バイトずつ流す。
    constexpr x68k::u32 kSectors = 2;
    for (x68k::u32 s = 0; s < kSectors; ++s)
    {
        for (x68k::u32 i = 0; i < FakeDisk::kSectorSize; ++i)
        {
            const auto value = static_cast<x68k::u8>(i == 0 ? (0x70u + s) : 0x22u);
            CHECK(m.dmaWrite(value));
        }
    }

    CHECK(m.ioRead8(kSasiStatus) == kPhaseValueStatus);
    CHECK(m.ioRead8(kSasiData) == 0x00);

    for (x68k::u32 s = 0; s < kSectors; ++s)
    {
        const std::size_t offset = (6 + s) * FakeDisk::kSectorSize;
        CHECK(disk.data_[offset] == static_cast<x68k::u8>(0x70u + s));
        CHECK(disk.data_[offset + 1] == 0x22);
    }
}
