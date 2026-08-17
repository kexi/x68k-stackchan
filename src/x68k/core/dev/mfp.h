// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MC68901 MFP ($E88000)。
//
// X68000 の割り込みとタイマの中枢。Human68k のシステムタイマ、垂直帰線割り込み、
// キーボード受信がすべてここを通るため、これが動かないと起動しても操作できない。
//
// 実装範囲: レジスタの読み書き、タイマ A-D、割り込みコントローラ (IERA/B,
// IPRA/B, ISRA/B, IMRA/B)、GPIP (垂直帰線の状態)、シリアル受信 (キーボード)。
// 実機の非同期な細部 (クロックの位相など、MFP サイクル未満の粒度) は追わない。
// 分周器の途中経過は保持する。停止と再開で端数の扱いが変わり、割り込みの
// 間隔に効くため。
//
// 外部入力 TAI/TBI は実装していない。これに依存するタイマのモード
// (イベントカウント / パルス幅測定) は動かない。

#ifndef X68K_CORE_DEV_MFP_H
#define X68K_CORE_DEV_MFP_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Mfp
{
public:
    // レジスタ番号 (ベースからのオフセット / 2)。
    // MFP は奇数アドレスにのみレジスタが並ぶ (8bit デバイスを 16bit バスに繋ぐため)。
    enum Reg : u32
    {
        kGpip = 0x00,  // 汎用入出力。bit4 に垂直帰線が入る
        kAer = 0x01,   // アクティブエッジ
        kDdr = 0x02,   // 入出力方向
        kIera = 0x03,  // 割り込み許可 A
        kIerb = 0x04,
        kIpra = 0x05,  // 割り込み保留 A
        kIprb = 0x06,
        kIsra = 0x07,  // 割り込みサービス中 A
        kIsrb = 0x08,
        kImra = 0x09,  // 割り込みマスク A
        kImrb = 0x0A,
        kVr = 0x0B,    // ベクタ番号 (上位 4bit)
        kTacr = 0x0C,  // タイマ A 制御
        kTbcr = 0x0D,
        kTcdcr = 0x0E,  // タイマ C/D 制御
        kTadr = 0x0F,   // タイマ A データ
        kTbdr = 0x10,
        kTcdr = 0x11,
        kTddr = 0x12,
        kScr = 0x13,  // 同期文字
        kUcr = 0x14,  // USART 制御
        kRsr = 0x15,  // 受信状態
        kTsr = 0x16,  // 送信状態
        kUdr = 0x17,  // USART データ
        kRegCount = 0x18,
    };

    // VR bit3 (S)。1 で Software EOI、0 で Automatic EOI。
    // X68000 の IOCS は $40 を書くので 0 側を使う。
    static constexpr u8 kVrSoftwareEoi = 0x08;

    // IERA/IPRA のビット。X68000 での割り当て。
    static constexpr u8 kIntGpip7 = 0x80;  // 未使用
    static constexpr u8 kIntGpip6 = 0x40;  // CRTC 同期
    static constexpr u8 kIntTimerA = 0x20;
    static constexpr u8 kIntRecvFull = 0x10;  // キーボード受信
    static constexpr u8 kIntRecvError = 0x08;
    static constexpr u8 kIntSendEmpty = 0x04;
    static constexpr u8 kIntSendError = 0x02;
    static constexpr u8 kIntTimerB = 0x01;  // 水平同期

    // IERB/IPRB のビット。
    static constexpr u8 kIntGpip5 = 0x80;
    static constexpr u8 kIntGpip4 = 0x40;  // 垂直帰線
    static constexpr u8 kIntTimerC = 0x20;
    static constexpr u8 kIntTimerD = 0x10;
    static constexpr u8 kIntGpip3 = 0x08;
    static constexpr u8 kIntGpip2 = 0x04;
    static constexpr u8 kIntGpip1 = 0x02;
    static constexpr u8 kIntGpip0 = 0x01;

    // タイマ制御レジスタの下位 3bit が表す分周比。
    // 0 は停止、1-7 が 4/10/16/50/64/100/200 分周 (MC68901 の仕様)。
    static constexpr u32 kPrescaleTable[8] = {0, 4, 10, 16, 50, 64, 100, 200};

    void reset();

    // レジスタを読む。
    //
    // const にできないのは、UDR (受信データ) を読むと RSR の受信バッファフルが
    // 落ちるため。実機の MC68901 がそう振る舞う。
    [[nodiscard]] u8 read(u32 regIndex);
    void write(u32 regIndex, u8 value);

    // CPU のサイクル数ぶん時間を進める。タイマのカウントダウンを行う。
    //
    // 停止中のタイマを弾く判定だけをここに置く。プロファイルでは
    // Mfp::tick と tickTimer の合計が 2171 サンプルで、CPU のディスパッチ
    // 全体より大きい単独最大の項目だった。tickTimer は .cpp 側にあり、
    // 4 本ぶん無条件に呼んでいた。
    //
    // X68000 が実際に動かすのはタイマ C (システムクロック) と D
    // (RS-232C ボーレート) だけで、A/B は IOCS が使うときしか動かない。
    // 止まっているタイマは制御レジスタの下位 3bit が 0 なので、そこを見れば
    // 呼ぶ前に分かる。
    //
    // Why not timerPrescale() をそのまま呼ばないか: あれは bit3 (外部入力を
    // 要するモード) の判定を含む .cpp 側の関数で、ここで呼ぶと結局 4 回の
    // 呼び出しが残る。下位 3bit が 0 なら bit3 の値に関わらず停止なので、
    // 速い側の判定としてはこれで十分 (bit3 が立つ場合は下の tickTimer が
    // 改めて timerPrescale() を引いて 0 を返す)。

    // MFP は 4MHz、CPU は 10MHz。4/10 ≒ 1/2.5 だが、割り算を避けて 2 で割る
    // 近似にしてある (タイマ精度は Human68k の起動に影響しない)。
    static constexpr u32 kCpuToMfpShift = 1;

    // inlineFastPath を false にすると、タイマの最頻経路を展開せず
    // 常に tickTimerCounted を呼ぶ (この最適化を入れる前と同じ形)。
    // 実機で焼き直さずに効果を測るための口で、状態遷移はどちらでも
    // 完全に同一 (perf_switch.h を見よ)。
    void tick(u32 cycles, bool inlineFastPath = true)
    {
        const u32 mfpCycles = cycles >> kCpuToMfpShift;
        if (mfpCycles == 0)
        {
            return;
        }
        // 動いているタイマだけを詰めた表を引く。本数と分周値は制御
        // レジスタが書かれたときにしか変わらない。
        //
        // Why キャッシュするか: この関数は **命令ごとに必ず通る**。毎回 3
        // 本のレジスタを読み、ビットを切り出し、止まって
        // いる本数ぶん空振りするのは、変わらない答えを繰り返し計算して
        // いることになる。実機と同じ run() 経路のプロファイルでは
        // Mfp::tick の前置きだけで約 330 サンプル (全体の 6%)。
        //
        // 一度この最適化を 0.0% と判定して捨てたことがある。当時の run() は
        // 8 サイクルの quantum を使っており、ホスト側は quantum を通らない
        // step() 経路でプロファイルを取っていたため、Mfp::tick の前置きが
        // 埋もれて見えていた (aed4797)。quantum はその後、観測可能なずれを
        // 作ると分かって撤廃した。
        for (u32 i = 0; i < runningCount_; ++i)
        {
            const RunningTimer& t = running_[i];
            u32& counter = prescaleCounter_[t.index];
            counter += mfpCycles;
            if (counter < t.prescale)
            {
                continue;  // まだ 1 回も減らない。ここが最頻。
            }

            // 閾値に届いた回のうち、圧倒的多数は「1 回だけ減って、まだ 0 に
            // ならない」で終わる。タイムアウトはデータレジスタの値ぶんに
            // 1 度しか来ないし (タイマ C の既定なら 200 回に 1 度)、
            // 2 回以上減るのは 1 命令のサイクル数が分周値を超えたときだけ。
            //
            // その最頻の経路だけをここへ出す。tickTimerCounted は別 TU に
            // あるので、ESP32-S3 では実呼び出しになる。RTC と CRTC で同じ形が
            // 効いたのと同じ理由 (TU を跨ぐ呼び出しだけが削れる)。
            //
            // Why not 全部を展開しないか: タイムアウト側はリロードと raise()
            // を含み、展開すると毎命令通るこのループが膨らむ。過去に即値と
            // 絶対ロングの展開で -3.1% を実測している。分ける位置が要点。
            const u32 remainder = counter - t.prescale;
            u8& value = timerValue_[t.index];
            const bool decrementsOnce = inlineFastPath && remainder < t.prescale && value > 1;
            if (decrementsOnce)
            {
                counter = remainder;
                --value;
                continue;
            }
            tickTimerCounted(static_cast<int>(t.index), t.prescale);
        }
    }

private:
    // 動作中のタイマ 1 本ぶんの決まりきった情報。
    struct RunningTimer
    {
        std::size_t index = 0;  // 0-3
        u32 prescale = 0;       // MFP サイクル単位
    };

    // 制御レジスタから running_ を組み直す。制御レジスタを書いたときに呼ぶ。
    void refreshRunningTimers()
    {
        const u8 tcdcr = reg_[kTcdcr];
        const u8 ctl[4] = {reg_[kTacr], reg_[kTbcr], static_cast<u8>((tcdcr >> 4) & 7u),
                           static_cast<u8>(tcdcr & 7u)};
        runningCount_ = 0;
        for (std::size_t i = 0; i < 4; ++i)
        {
            // bit3 が立つモードは外部入力 TAI/TBI を要する。未実装なので
            // 経過サイクルだけでは進められない (timerPrescale と同じ判定)。
            const bool needsExternalInput = (ctl[i] & 0x08u) != 0;
            if (needsExternalInput)
            {
                continue;
            }
            const u32 prescale = kPrescaleTable[ctl[i] & 7u];
            if (prescale == 0)
            {
                continue;
            }
            running_[runningCount_].index = i;
            running_[runningCount_].prescale = prescale;
            ++runningCount_;
        }
    }

public:
    // 垂直帰線の開始/終了を通知する。GPIP4 の状態が変わり、
    // 設定によっては割り込みが上がる。
    void setVerticalBlank(bool active);

    // キーボードから 1 バイト受信した。受信バッファに積んで割り込みを上げる。
    void receiveKeyboardByte(u8 value);

    // レジスタに書き込まれた値を副作用なしで見る。
    //
    // read() は読み出しで状態が変わるレジスタがあるため、状態を調べる用途には
    // 使えない。「キー入力が届かない」ような不具合は、割り込みがマスクされて
    // いるのか上がっていないのかで原因が全く違うので、覗く手段が要る。
    //
    // タイマデータレジスタでは read() と値が違う。read() は CPU から見える
    // メインカウンタの現在値を返すが、こちらは次のリロード値 (最後に書かれた
    // 値) を返す。動作中に書き換えられると両者は食い違う。カウンタの現在値が
    // 要るなら read() を使う。
    [[nodiscard]] u8 peek(u32 reg) const
    {
        return reg < reg_.size() ? reg_[reg] : 0u;
    }

    // 保留中で、マスクされていない割り込みがあるか。
    //
    // ここは **毎命令通る**。実行時間の大半は「保留が 1 つも無い」状態なので、
    // その判定だけをインラインに置き、実際に保留があるときだけ .cpp 側の
    // 完全な判定 (serviceBlockMask を含む) を呼ぶ。
    //
    // IPR & IMR が 0 なら、どんなマスクを掛けても 0 のまま。つまり
    // serviceBlockMask を見るまでもなく「保留無し」が確定する。
    //
    // Why not 全部インラインにしないか: serviceBlockMask は ISR を走査する
    // ループを含み、展開すると命令フェッチの熱い経路を押し出す。ESP32-S3 の
    // I-cache では、この経路を小さく保つこと自体が効く (デバイス tick の
    // まとめ込みが実機だけで +12.8% だったのと同じ理由)。
    [[nodiscard]] bool hasPendingInterrupt() const
    {
        const bool anyPending = ((reg_[kIpra] & reg_[kImra]) | (reg_[kIprb] & reg_[kImrb])) != 0;
        if (!anyPending)
        {
            return false;
        }
        return hasPendingInterruptBlocked();
    }

    // 最も優先度の高い保留割り込みのベクタ番号を返し、その割り込みを
    // サービス中へ移す。保留が無ければ 0 を返す。
    u32 acknowledgeInterrupt();

private:
    // hasPendingInterrupt() の遅い側。IPR & IMR に何か立っているときだけ
    // 呼ばれ、Software EOI の抑止を含めて判定する。
    [[nodiscard]] bool hasPendingInterruptBlocked() const;

    void raise(bool groupA, u8 bit);

    // Software EOI でサービス中のチャネルより下位を抑止するマスク。
    // 抑止しないビットが 1。S=0 なら常に 0xFF。
    [[nodiscard]] u8 serviceBlockMask(bool groupA) const;

    [[nodiscard]] u32 timerPrescale(u8 control) const;

    // タイマのデータレジスタへの書き込みを、停止中だけカウンタへ写す。
    // 動作中はリロードのときに reg_ から読まれる。
    void loadTimerIfStopped(int index, u8 control, u8 value);

    // タイマを停止させたら、プリスケーラの端数を捨てる。
    // 実機はメインカウンタを保つ一方、プリスケーラの残量は失う。
    void clearPrescalerIfStopped(int index, u8 control);
    // 分周の閾値に達したぶんだけカウンタを減らし、必要なら割り込みを上げる。
    // 累算は呼び出し側 (tickOne) が済ませてある。
    void tickTimerCounted(int index, u32 prescale);

    std::array<u8, kRegCount> reg_{};
    // 各タイマの分周カウンタ。実機の分周器に相当する。
    std::array<u32, 4> prescaleCounter_{};
    // タイマの現在値。データレジスタ書き込みでリロードされる。
    std::array<u8, 4> timerValue_{};
    // 動作中のタイマだけを詰めた表。refreshRunningTimers() が作る。
    std::array<RunningTimer, 4> running_{};
    u32 runningCount_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_MFP_H
