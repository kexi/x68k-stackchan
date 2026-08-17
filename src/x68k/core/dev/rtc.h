// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// RTC RP5C15 ($E8A000)。
//
// Human68k は起動時に日付と時刻を読む。全部 0 を返すと「0 年 0 月 0 日」に
// なり、月や日として不正な値を受け取った側が想定外の動きをすることがある。
// 妥当な日付を返せるようにしておく。
//
// レジスタは 4bit 幅で、$E8A001 から 2 バイトおきに 13 個並ぶ。
// バンク切り替えがあり、バンク 0 が時計、バンク 1 がアラームと設定。

#ifndef X68K_CORE_DEV_RTC_H
#define X68K_CORE_DEV_RTC_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Rtc
{
public:
    // レジスタ番号 (ベースからのオフセット / 2)。
    enum Reg : u32
    {
        kSecond1 = 0x0,   // 秒の 1 の位
        kSecond10 = 0x1,  // 秒の 10 の位
        kMinute1 = 0x2,
        kMinute10 = 0x3,
        kHour1 = 0x4,
        kHour10 = 0x5,
        kWeekday = 0x6,
        kDay1 = 0x7,
        kDay10 = 0x8,
        kMonth1 = 0x9,
        kMonth10 = 0xA,
        kYear1 = 0xB,
        kYear10 = 0xC,
        kModeRegister = 0xD,  // バンク選択
        kTestRegister = 0xE,
        kResetRegister = 0xF,
        kRegCount = 0x10,
    };

    void reset();

    [[nodiscard]] u8 read(u32 regIndex) const;
    void write(u32 regIndex, u8 value);

    // CPU サイクルぶん時間を進める。秒が繰り上がる。
    //
    // ここは **毎命令通る**のに、実際に秒が繰り上がるのは 1 秒 =
    // 10,000,000 サイクルに 1 度だけ。つまり実行のほぼ全ては
    // 「累算して閾値に届かない」で終わる。その判定だけをヘッダに置き、
    // 繰り上がる回だけ .cpp 側の tickCarry() を呼ぶ。
    //
    // Why not quantum でまとめないか: 一度「RTC は 1 秒単位でしか変わらない
    // から 10000 サイクルまとめてよい」として実装したが、ゲストは秒レジスタを
    // 命令単位でポーリングでき、秒の境界が最大 1998 サイクル遅れるずれが
    // 観測できた (docs/knowledge/cores3-emulator-runtime.md)。ここは
    // まとめず、渡す量はそのままで**呼び出しの間接性だけを消す**。
    // 状態遷移はサイクル単位で完全に元のままになる。
    //
    // FastPath=false にすると、この最適化を入れる前と同じ「常に実呼び出し」
    // へ戻る。実機で焼き直さずに効果を測るための口 (perf_switch.h)。
    // テンプレートにしてあるのは、有効側の生成コードをスイッチ導入前と
    // 同一に保つため。bool の引数だと毎命令フラグを読んで分岐する。
    // Why not 既定引数を付けて tick(cycles) でも呼べるようにしないか:
    // Machine の配線が FastPath を渡し忘れても既定の true が入り、
    // 切った側と入れた側が同じ経路になる。同値テストはその取り違えを
    // 見逃す (codex の指摘)。名前を分けて必ず明示させれば、渡し忘れは
    // コンパイルエラーになる。
    template <bool FastPath>
    void tickFast(u32 cycles)
    {
        cycleAccumulator_ += cycles;
        const bool canReturnEarly = FastPath && cycleAccumulator_ < kCyclesPerSecond;
        if (canReturnEarly)
        {
            return;  // ここが最頻。1 秒に 1 度しか下へ落ちない。
        }
        tickCarry();
    }

    // 起点となる日時を設定する。エミュレータの起動時にホストの時刻を渡す。
    // year は西暦の下 2 桁 (RP5C15 は 2 桁しか持たない)。
    void setDateTime(u32 year, u32 month, u32 day, u32 hour, u32 minute, u32 second);

private:
    // X68000 の CPU は 10MHz。1 秒ぶんのサイクル数。
    // tick() の速い側がヘッダで比較するので、ここに置く。
    static constexpr u32 kCyclesPerSecond = 10000000;

    // tick() の遅い側。累算が閾値に届いたときだけ呼ばれる。
    void tickCarry();

    void advanceOneSecond();

    // 時計の値。BCD ではなく 10 進の各桁として持つ (レジスタが 4bit 幅のため)。
    u32 second_ = 0;
    u32 minute_ = 0;
    u32 hour_ = 0;
    u32 day_ = 1;
    u32 month_ = 1;
    u32 year_ = 26;  // 西暦の下 2 桁
    u32 weekday_ = 0;

    // 1 秒ぶんの CPU サイクルを数える。
    u32 cycleAccumulator_ = 0;

    // バンク 1 のレジスタ。アラームなど。読み書きできれば足りる。
    std::array<u8, kRegCount> bank1_{};
    u8 mode_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_RTC_H
