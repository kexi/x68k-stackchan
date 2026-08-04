// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// X68000 のアドレスデコード。CPU からのアクセスを RAM / ROM / VRAM / I/O へ振り分ける。
//
// 性能について:
//   ここは 68000 が命令をフェッチするたびに通る、エミュレータで最も実行回数の多い
//   経路になる。判定の順序は実行頻度順 (RAM → ROM → VRAM → I/O) にしてある。
//   ESP32 では bus_read16 の分岐が丸ごとホットパスなので、将来 IRAM 配置の
//   第一候補になる。
//
// メモリの所有:
//   RAM や VRAM の実体はここが持たず、外から与えられたポインタを指すだけにする。
//   ESP32 では PSRAM の断片化を避けるため起動直後に一括確保する必要があり、
//   確保の主導権を platform 層に渡しておきたいため。

#ifndef X68K_CORE_BUS_H
#define X68K_CORE_BUS_H

#include <cstdint>

#include "cpu/m68k_types.h"
#include "dev/sram.h"
#include "dev/video.h"
#include "memmap.h"

namespace x68k
{

// 仮想関数を通さない経路を持つ CPU。実体は cpu/m68k.h。
//
// Why not m68k.h を include しないか: m68k.h 側は bus.h を必要としない
// (Bus は m68k_types.h にある)。ここで含めると core/ のヘッダが相互に
// 巻き込み合う形になり、CPU だけを切り出してテストする構成が崩れる。
class M68k;

// I/O デバイスへのアクセスを受け取る口。
//
// Machine がこれを実装して各デバイスへ振り分ける。バスがデバイスの詳細を
// 知らずに済むので、デバイスを足してもバスのコードは変わらない。
class IoHandler
{
public:
    virtual ~IoHandler() = default;

    virtual u8 ioRead8(u32 addr) = 0;
    virtual void ioWrite8(u32 addr, u8 value) = 0;
    virtual u16 ioRead16(u32 addr) = 0;
    virtual void ioWrite16(u32 addr, u16 value) = 0;
};

// メモリ領域の実体。platform 層が確保して渡す。
struct MemoryMap
{
    u8* mainRam = nullptr;       // kMainRamSize バイト
    u8* textVram = nullptr;      // kTvramSize バイト (4 プレーン連続)
    u8* graphicVram = nullptr;   // 任意。無ければ読み出しは 0
    const u8* iplRom = nullptr;  // kIplromSize バイト。必須
    const u8* cgRom = nullptr;   // 任意。無ければ読み出しは 0
};

class SystemBus final : public Bus
{
public:
    SystemBus(MemoryMap memory, Sram& sram, IoHandler& io) : mem_(memory), sram_(sram), io_(io) {}

    // メモリ領域を差し替える。
    //
    // Why 再代入ではなくこれを使うか: SystemBus は Sram と IoHandler を参照で
    // 持つため代入演算子が消える。実体の確保は platform 層の責務にしてあるので
    // (ESP32 では PSRAM の断片化を避けるため起動直後に一括確保したい)、
    // 後からポインタだけ差し替えられる必要がある。
    void setMemory(const MemoryMap& memory)
    {
        mem_ = memory;
        publishFastRam();
    }

    // G-VRAM のページ折り込みに使うビデオコントローラを教える。
    //
    // $C00000-$DFFFFF は 512KB の窓 4 つで、窓ごとに「どのページを見るか」が
    // 変わる。どのビット幅を折り込むかは色数モード ($E82400 bit1-0) 次第なので、
    // バスがモードを知らないと 16 色の書き込みを他ページごと潰してしまう。
    //
    // Why not IoHandler に色数を問い合わせるメソッドを足さないか: IoHandler は
    // 「アドレスを渡すと読み書きが起きる」だけの口で、副作用のある経路。
    // ドット 1 つの書き込みごとにそこを通すと、I/O 側にアクセスログや
    // ウェイトを足したときに G-VRAM の書き込みが巻き込まれる。
    //
    // Why not MemoryMap にモードを持たせないか: MemoryMap は「実体の在りか」を
    // 表す値の集まりで、platform 層が確保時に一度作って渡す。時間とともに
    // 変わるモードを混ぜると、モード変更のたびに setMemory を呼ぶ設計になり
    // 所有の切り分けが崩れる。
    //
    // 未設定 (nullptr) なら 16 色モードとして扱う。VideoController::reset() が
    // $E82400 を 0 (=16 色) にするので、実機のリセット直後と同じ状態になる。
    void setVideoController(const VideoController* video)
    {
        video_ = video;
    }

    u8 read8(u32 addr) override;
    u16 read16(u32 addr) override;
    void write8(u32 addr, u8 value) override;
    void write16(u32 addr, u16 value) override;

    [[nodiscard]] bool lastAccessFaulted() const override
    {
        return faulted_;
    }

    // 指定アドレスへの書き込みを見張る。
    //
    // 「あるワークが 0 のままで先へ進まない」ときに、誰がそこを書くはず
    // だったのかを追うのに使う。命令列を静的に走査しても、間接
    // アドレッシングで書かれていると見つからない。
    //
    // 解除はコールバックに nullptr を渡す。
    //
    // Why not アドレス 0 を「解除」に使わないか: $000000 はリセットベクタが
    // 置かれる番地で、監視したい場面が実際にある。0 を特別扱いすると
    // そこだけ見られなくなる。
    void setWriteWatch(u32 addr, void (*callback)(u32 addr, u32 value, void* user), void* user)
    {
        watchAddr_ = addr;
        watchCallback_ = callback;
        watchUser_ = user;
        // ウォッチ中は CPU の直接経路を止める。素通しにすると監視対象への
        // 書き込みが notifyWatch を通らず、見張っているつもりで何も出ない。
        publishFastRam();
    }

    // テキスト VRAM への書き込みがあった矩形を追跡する。
    //
    // 画面全体を毎フレーム転送すると SPI 接続の LCD では間に合わないので、
    // 変化した部分だけを送る。書き込み時にタイル単位で印を付けておき、
    // 描画側がそれを見て転送範囲を決める。
    static constexpr u32 kDirtyTileHeight = 16;
    static constexpr u32 kDirtyTileRows = 1024 / kDirtyTileHeight;

    [[nodiscard]] bool isTextRowDirty(u32 tileRow) const
    {
        return tileRow < kDirtyTileRows && textDirty_[tileRow];
    }

    void clearTextDirty()
    {
        for (auto& d : textDirty_)
        {
            d = false;
        }
    }

    [[nodiscard]] bool anyTextDirty() const
    {
        for (const auto d : textDirty_)
        {
            if (d)
            {
                return true;
            }
        }
        return false;
    }

    // 起動直後に IPL-ROM を $000000 に見せるかどうか。
    //
    // X68000 はリセット直後、ベクタを読むために ROM が $000000 へ写像される。
    // IPL-ROM がエリアセット ($E86001) に書き込むと通常の RAM 配置へ切り替わる。
    // これを再現しないとリセットベクタが読めない。
    void setRomMappedAtZero(bool mapped)
    {
        romAtZero_ = mapped;
        publishFastRam();
    }

    [[nodiscard]] bool romMappedAtZero() const
    {
        return romAtZero_;
    }

    // 仮想関数を通さない経路を使う CPU を登録する。
    //
    // Why バスから CPU へ押し出すか: 窓を使ってよいかどうかを決めるのは
    // 「メインメモリの実体がどこか」と「$000000 に ROM が写像されているか」で、
    // どちらもバスの持ち物。CPU から問い合わせる形にすると、状態が変わる
    // たびに呼び出し側が両方を更新する契約になり、setRomMappedAtZero を
    // 直に叩いている箇所 (ホストのフロントエンド・テスト) が全部漏れの
    // 候補になる。バスが変化のたびに押し出せば、契約は登録の 1 回だけで済む。
    //
    // Why not 登録を必須にしないか: テストベクタは SystemBus を使わず
    // 独自の Bus 実装を渡す。登録が無ければ CPU は今までどおり
    // 全アクセスを仮想関数で通すので、既存の使い方は何も変わらない。
    void attachFastPathCpu(M68k* cpu)
    {
        fastPathCpu_ = cpu;
        publishFastRam();
    }

private:
    // 登録済みの CPU へ「直接触ってよいメインメモリ」を教え直す。
    //
    // 実体・ROM 写像・ウォッチのどれかが変わるたびに呼ぶ。実装は bus.cpp
    // (ここでは M68k が不完全型なのでメンバを呼べない)。
    void publishFastRam();

    void markTextDirty(u32 offsetInPlane);

    // G-VRAM のアクセス方法。窓のアドレスと色数モードから 1 回だけ決める。
    //
    // 実 VRAM は 1 ワードに全ページのドットが同居する構造なので、CPU 側の
    // 512KB 窓へのアクセスは「共有ワードの一部だけ」に効く。
    // shift はそのワード内でのビット位置、mask は幅。
    struct GvramLane
    {
        u32 byteOffset;  // 実 VRAM 内のワード先頭からのバイト位置 (偶数)
        u32 shift;       // ワード内のビット位置
        u16 mask;        // 折り込む幅 ($F / $FF / $FFFF)
    };
    [[nodiscard]] GvramLane gvramLaneOf(u32 addr) const;

    // 窓アドレスに対応するドットを、ページのぶんだけ残したワードとして返す。
    // 16 色なら $000X、256 色なら $00XX、65536 色ならワードそのもの。
    [[nodiscard]] u16 readGvramDot(u32 addr) const;

    // 窓アドレスへ 1 ドット書く。他ページのビットは読み出して保つ。
    void writeGvramDot(u32 addr, u16 value);

    MemoryMap mem_;
    Sram& sram_;
    IoHandler& io_;
    // 色数モードを引くためだけに持つ。所有しない (Machine が持つ)。
    const VideoController* video_ = nullptr;
    bool romAtZero_ = true;
    // 直前のアクセスが応答しない領域だったか。
    // IPL-ROM は SCSI ROM ($FC0000) の有無をバスエラーで調べるので、
    // 「何も無い」ことを 0 ではなくエラーで返す必要がある。
    bool faulted_ = false;

    // 書き込みウォッチ。設定されていなければ何もしない。
    u32 watchAddr_ = 0;
    void (*watchCallback_)(u32 addr, u32 value, void* user) = nullptr;
    void* watchUser_ = nullptr;
    // 直接経路を使う CPU。所有しない。未登録なら全アクセスが仮想関数を通る。
    M68k* fastPathCpu_ = nullptr;

    // ウォッチ対象なら通知する。write8 / write16 の両方から呼ぶ。
    void notifyWatch(u32 addr, u32 value, u32 size)
    {
        if (watchCallback_ == nullptr)
        {
            return;
        }
        const bool hits = addr <= watchAddr_ && watchAddr_ < addr + size;
        if (hits)
        {
            watchCallback_(addr, value, watchUser_);
        }
    }
    bool textDirty_[kDirtyTileRows] = {};
};

}  // namespace x68k

#endif  // X68K_CORE_BUS_H
