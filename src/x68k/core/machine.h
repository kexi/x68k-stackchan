// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// X68000 一式の組み立て。CPU・バス・各デバイスをまとめ、時間を進める。
//
// ここが core/ の最上位で、ホストのフロントエンドも ESP32 の platform 層も
// この API だけを使う。ROM とディスクの読み込みは呼び出し側の責務にしてある
// (ファイルシステムの都合を core/ に持ち込まないため)。

#ifndef X68K_CORE_MACHINE_H
#define X68K_CORE_MACHINE_H

#include <cstdint>

#include "bus.h"
#include "cpu/m68k.h"
#include "dev/mfp.h"
#include "dev/rtc.h"
#include "dev/sram.h"
#include "dev/video.h"

namespace x68k
{

// ディスクの読み書きを外から与えるための口。
// ホストでは通常のファイル、実機では microSD が実装する。
class DiskImage
{
public:
    virtual ~DiskImage() = default;

    // 論理セクタ単位で読む。1 セクタ 256 バイト (SASI)。
    // 成功したら true。
    virtual bool readSector(u32 lba, u8* buffer, u32 sectorCount) = 0;
    virtual bool writeSector(u32 lba, const u8* buffer, u32 sectorCount) = 0;
    [[nodiscard]] virtual bool isPresent() const = 0;
};

class Machine final : public IoHandler
{
public:
    Machine();

    // メモリ領域を設定する。実体の確保は呼び出し側が行う
    // (ESP32 では PSRAM の断片化を避けるため起動直後に一括確保したい)。
    void setMemory(const MemoryMap& memory);

    // 起動デバイスを設定する。null なら「ディスクなし」として扱う。
    void setDisk(DiskImage* disk)
    {
        disk_ = disk;
    }

    // リセットして IPL-ROM の先頭から実行を始める。
    void reset();

    // 指定サイクル数ぶん実行する。実際に消費したサイクル数を返す。
    // デバイスの時間もまとめて進める。
    u32 run(u32 cycles);

    // 1 命令だけ実行する。トレース用。
    u32 step();

    [[nodiscard]] M68k& cpu()
    {
        return cpu_;
    }
    [[nodiscard]] const M68k& cpu() const
    {
        return cpu_;
    }
    [[nodiscard]] SystemBus& bus()
    {
        return bus_;
    }
    [[nodiscard]] Sram& sram()
    {
        return sram_;
    }
    [[nodiscard]] Crtc& crtc()
    {
        return crtc_;
    }
    [[nodiscard]] VideoController& video()
    {
        return video_;
    }
    [[nodiscard]] Mfp& mfp()
    {
        return mfp_;
    }
    [[nodiscard]] Rtc& rtc()
    {
        return rtc_;
    }

    // キーボードから 1 バイト届いた。
    void pressKey(u8 scanCode);

    // CPU が停止しているか (未実装命令に当たった等)。
    [[nodiscard]] bool isHalted() const
    {
        return cpu_.state().halted;
    }

    // 未実装命令で止まったときの命令語。実装すべき命令を知るために使う。
    [[nodiscard]] u16 haltedOpcode() const
    {
        return cpu_.state().ir;
    }

    // --- IoHandler ---
    u8 ioRead8(u32 addr) override;
    void ioWrite8(u32 addr, u8 value) override;
    u16 ioRead16(u32 addr) override;
    void ioWrite16(u32 addr, u16 value) override;

private:
    void serviceInterrupts();
    u8 sasiRead(u32 addr);
    void sasiWrite(u32 addr, u8 value);

    Sram sram_;
    SystemBus bus_;
    M68k cpu_;
    Crtc crtc_;
    VideoController video_;
    Mfp mfp_;
    Rtc rtc_;
    DiskImage* disk_ = nullptr;

    // SASI の状態機械。IPL-ROM がブートセクタを読むのに使う。
    struct SasiState
    {
        u8 phase = 0;  // 0=バスフリー 1=コマンド 2=データ転送 3=ステータス
        u8 command[6] = {};
        u32 commandLength = 0;
        u8 buffer[256] = {};
        u32 bufferPos = 0;
        u32 bufferLength = 0;
        u8 status = 0;
        bool interruptEnabled = false;
    };
    SasiState sasi_{};

    // FDC が返す結果バイトの残り数。
    // コマンドを受けたら設定し、読まれるたびに減らす。0 になったら
    // コマンド待ちへ戻る。常に結果フェーズのままだと IPL-ROM が
    // 次のコマンドへ進めない。
    int fdcResultRemaining_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_MACHINE_H
