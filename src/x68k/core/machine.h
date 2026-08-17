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

#include <cstddef>
#include <cstdint>

#include "bus.h"
#include "cpu/m68k.h"
#include "dev/adpcm.h"
#include "dev/dmac.h"
#include "dev/fdc.h"
#include "dev/iosc.h"
#include "dev/mfp.h"
#include "dev/opm.h"
#include "dev/rtc.h"
#include "dev/scc.h"
#include "dev/sprite.h"
#include "dev/sram.h"
#include "dev/video.h"
#include "perf_switch.h"

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

class Machine final : public IoHandler, public DmaDevice, public DmaMemory
{
public:
    Machine();

    // メモリ領域を設定する。実体の確保は呼び出し側が行う
    // (ESP32 では PSRAM の断片化を避けるため起動直後に一括確保したい)。
    void setMemory(const MemoryMap& memory);

    // SASI が 1 コマンドで扱えるセクタ数と、それに要るバッファの大きさ。
    //
    // 長さフィールドは 1 バイトだが、上限は 255 ではなく 256。
    // SASI の 6 バイトコマンドでは 0 が「256 ブロック」を意味する
    // (SASI Design Specifications Rev. F の 6.2.5)。1 と読むと、
    // 256 セクタの要求で最初の 1 つだけ処理して成功を返してしまう。
    // WRITE では静かなディスク破損になる。
    //
    // IPL-ROM は 0 を使わない (実測では 1 / 4 / 221) が、Human68k や
    // アプリケーションが使う可能性がある。
    static constexpr u32 kSasiMaxSectorsPerCommand = 256;
    static constexpr u32 kSasiBufferBytes = 256 * kSasiMaxSectorsPerCommand;

    // SASI の転送バッファを与える。kSasiBufferBytes 以上が要る。
    //
    // 呼ばないと SASI の READ が失敗する。ホストは new、実機は PSRAM から。
    void setSasiBuffer(u8* buffer)
    {
        sasi_.buffer = buffer;
    }

    // 起動デバイスを設定する。null なら「ディスクなし」として扱う。
    void setDisk(DiskImage* disk)
    {
        disk_ = disk;
    }

    // フロッピードライブ (FDD0/FDD1) にイメージを入れる。null で取り出す。
    //
    // Why not setDisk と同じ口にするか: FD は CHS でアクセスされ、
    // セクタ長も 1024 バイト (2HD) と SASI の 256 バイトで違う。同じ
    // DiskImage に押し込むと「どちらのセクタ長で数えた LBA か」を
    // 呼び出し側しか知らない状態になり、読み出し位置を静かに間違える。
    void setFloppyDisk(u32 drive, FloppyImage* image)
    {
        fdc_.setImage(drive, image);
    }

    // リセットして IPL-ROM の先頭から実行を始める。
    void reset();

    // 指定サイクル数ぶん実行する。実際に消費したサイクル数を返す。
    // デバイスの時間は命令ごとに進める (step() と同じ粒度)。
    u32 run(u32 cycles);

    // 1 命令だけ実行する。トレース用。
    u32 step();

    // 毎命令通る経路の最適化を個別に切る。実機で焼き直さずに効果を
    // 測るための口 (perf_switch.h に理由がある)。既定は全て有効。
    //
    // 呼ぶのはエミュレーションを走らせていないタイミング、あるいは
    // スライスの切れ目。切り替えても状態遷移は変わらないので、途中で
    // 切り替えてもゲストからは見えない。
    void setPerfSwitch(const PerfSwitch& value)
    {
        perf_ = value;
    }

    [[nodiscard]] const PerfSwitch& perfSwitch() const
    {
        return perf_;
    }

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
    // DMAC がデータを取りに来る口。SASI のデータインフェーズから 1 バイト渡す。
    bool dmaRead(u8* value) override;
    bool dmaWrite(u8 value) override;

    // データを受け取り切った後の後始末。DMA 経由と CPU 経由で共有する。
    void finishSasiWrite();

    // DMAC がメモリを触る口。バスへそのまま流す。
    u8 dmaMemRead(u32 addr) override;
    void dmaMemWrite(u32 addr, u8 value) override;

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
    [[nodiscard]] Fdc& fdc()
    {
        return fdc_;
    }
    [[nodiscard]] Scc& scc()
    {
        return scc_;
    }
    [[nodiscard]] IoSc& iosc()
    {
        return iosc_;
    }
    [[nodiscard]] Sprite& sprite()
    {
        return sprite_;
    }
    [[nodiscard]] const Sprite& sprite() const
    {
        return sprite_;
    }
    [[nodiscard]] Opm& opm()
    {
        return opm_;
    }
    [[nodiscard]] Adpcm& adpcm()
    {
        return adpcm_;
    }

    // 音声を frames サンプルぶんモノラルで合成して out へ書く (pull 型)。
    //
    // platform 層が「必要になったときに必要な分だけ」取りに来る形にしてある。
    // Why not エミュレータ側から push するか: 出力側 (M5Unified のスピーカー)
    // のバッファが空くタイミングは platform 層しか知らない。push にすると
    // core/ が出力レートとバッファ長を知る必要が出て、ESP32 非依存でなくなる。
    //
    // 呼ぶのは Machine を所有するコア (実機では Core1) だけ。ここは
    // OPM のレジスタと ADPCM の FIFO を読むので、別コアから呼ぶと
    // ゲストが $E90003 を書いている最中の状態が混ざる。できたサンプルを
    // 別コアへ渡す仕事は platform/audio.h が持つ。
    //
    // 鳴っている音が 1 つも無ければ合成を省いてゼロで埋める。実機の
    // リアルタイムループから毎スライス呼べるのはこの早期リターンのため
    // (実測は docs/knowledge/cores3-emulator-runtime.md の音声の節)。
    void renderAudio(std::int16_t* out, std::size_t frames);

    // キーボードから 1 バイト届いた。
    void pressKey(u8 scanCode);

    // マウスが動いた / ボタンの状態が変わった。
    //
    // pressKey と同じく、ホストや platform 層が外から入力を注入する口。
    // dx/dy は前回からの相対移動量 (X68000 のマウスは絶対座標を持たない)。
    //
    // IOCS がマウスを有効化していない間の呼び出しは捨てられる。SCC の受信
    // FIFO が埋まっている間の呼び出しも同じく捨てられる (レポートは 3 バイト
    // 揃って初めて意味を持つので、途中まで積むことはしない)。
    //
    // 積めたら true、捨てたら false。呼び出し側が送り直しを判断できるように
    // する (Scc::moveMouse のコメントに理由を書いた)。
    bool moveMouse(int dx, int dy, bool leftButton, bool rightButton);

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
    // DMAC のチャネル 0 (FDC) を Fdc へ繋ぐ中継。
    //
    // Why not Machine 自身が両方のチャネルを引き受けるか: Machine は既に
    // チャネル 1 (SASI) の DmaDevice である。1 つのオブジェクトで 2 つの
    // チャネルを兼ねると dmaRead/dmaWrite の中で「今どちらのチャネルから
    // 呼ばれたか」を状態で判断することになり、転送の途中でその状態がずれた
    // ときに黙って相手のバッファを読む。チャネルごとに別のオブジェクトを
    // 繋げば、取り違えは型として起こらない。
    class FdcDmaPort final : public DmaDevice
    {
    public:
        explicit FdcDmaPort(Fdc& fdc) : fdc_(fdc) {}

        bool dmaRead(u8* value) override
        {
            return fdc_.dmaRead(value);
        }
        bool dmaWrite(u8 value) override
        {
            return fdc_.dmaWrite(value);
        }
        void dmaComplete(bool isComplete) override
        {
            fdc_.dmaComplete(isComplete);
        }

    private:
        Fdc& fdc_;
    };

    // 割り込みを 1 つだけ受理する。毎命令通る。
    //
    // 3 つのデバイスすべてに保留が無い状態が圧倒的に多いので、その判定を
    // ここでインラインに済ませ、何か上がっているときだけ .cpp 側の
    // 優先度付き処理 (serviceInterruptsSlow) を呼ぶ。
    //
    // Why not それぞれの service*Interrupt をそのまま呼ぶか: あちらは
    // 「保留があるか」「CPU が受け付けられるか」「ベクタ番号は何か」を
    // 順に見る本体で、.cpp 側にあるので ESP32-S3 では実呼び出しになる。
    // プロファイルで serviceIoScInterrupt / serviceMfpInterrupt だけで
    // 400 サンプル近くを占めていた。
    //
    // FDC の線はバスアクセス以外の契機 (DMA 完了など) でも変わるので、
    // 判定の前に必ず取り直す。ここは inline な setFdcLine 1 回で済む。
    void serviceInterrupts()
    {
        updateFdcInterruptLine();
        const bool anyPending =
            mfp_.hasPendingInterrupt() || scc_.hasPendingInterrupt() || iosc_.hasPendingInterrupt();
        if (!anyPending)
        {
            return;
        }
        serviceInterruptsSlow();
    }

    // serviceInterrupts() の遅い側。どれか 1 つでも保留があるときだけ呼ばれ、
    // MFP (6) > SCC (5) > I/O コントローラ (1) の順に受理を試す。
    void serviceInterruptsSlow();
    // 保留していた割り込みを CPU へ渡せたら true。
    // 優先度の高い方から順に試し、1 つ通ったらそこで止めるために戻り値を使う。
    bool serviceMfpInterrupt();
    bool serviceSccInterrupt();
    bool serviceIoScInterrupt();
    // FDC の割り込み線を I/O コントローラへ反映する。
    // FDC 自身は自分がどのコントローラに繋がっているかを知らないので、
    // 線の橋渡しは Machine が持つ。
    // FDC の割り込み線を I/O コントローラへ反映する。毎命令通るので inline。
    //
    // Why not Fdc から直接 IoSc を叩かないか: Fdc が IoSc を知ると、FDC 単体の
    // テストに割り込みコントローラを連れてくる必要が出る。実機でも「線が
    // 繋がっている」だけで FDC はコントローラの存在を知らないので、配線を
    // 持つのは両者を組み立てる Machine の責務にしてある。
    void updateFdcInterruptLine()
    {
        iosc_.setFdcLine(fdc_.hasInterrupt());
    }
    u8 sasiRead(u32 addr);
    void sasiWrite(u32 addr, u8 value);
    u8 sccRead(u32 addr);
    void sccWrite(u32 addr, u8 value);

    // 時間で動くデバイスへ経過サイクルを渡す。step() と run() の共通路。
    //
    // FastTick はテンプレート引数。run() の入口で 1 回だけ決めるので、
    // 有効側はスイッチを入れる前と同じ生成コードになる (perf_switch.h)。
    // bool の引数にすると毎命令フラグを読んで分岐することになり、
    // 測ろうとしているホットループに計測器のコストが乗る。
    template <bool FastTick>
    void tickDevices(u32 cycles);

    // run() の本体。FastTick ごとに 1 つずつ実体化する。
    template <bool FastTick>
    u32 runWith(u32 cycles);

    // 毎命令通る経路の最適化スイッチ。既定は全て有効で、無効側は
    // 実機の計測でだけ使う (perf_switch.h)。
    PerfSwitch perf_{};

    Sram sram_;
    SystemBus bus_;
    M68k cpu_;
    Crtc crtc_;
    VideoController video_;
    Mfp mfp_;
    Rtc rtc_;
    Fdc fdc_;
    Scc scc_;
    IoSc iosc_;
    Opm opm_;
    Adpcm adpcm_;
    Sprite sprite_;
    Dmac dmac_;
    FdcDmaPort fdcDmaPort_{fdc_};
    DiskImage* disk_ = nullptr;

    // SASI の状態機械。IPL-ROM がブートセクタを読むのに使う。
    struct SasiState
    {
        u8 phase = 0;  // 0=バスフリー 1=コマンド 2=データ転送 3=ステータス
        u8 command[6] = {};
        u32 commandLength = 0;
        // 転送バッファは外から与える。
        //
        // 65KB を Machine に埋め込むと、ESP32 の内部 SRAM (512KB) の
        // 1/8 を静的に食う。実測では .bss が 88KB になり、IPL-ROM 128KB を
        // 内部 SRAM へ置く余地が無くなった。SASI の転送は DMA の完了待ちで
        // 一気に流すだけで遅延に敏感ではないので、実機では PSRAM に置く。
        u8* buffer = nullptr;
        u32 bufferPos = 0;
        u32 bufferLength = 0;
        u8 status = 0;
        bool interruptEnabled = false;
    };
    SasiState sasi_{};
};

}  // namespace x68k

#endif  // X68K_CORE_MACHINE_H
