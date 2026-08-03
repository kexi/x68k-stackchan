// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// Z8530 SCC ($E98000)。チャネル B にマウスがぶら下がる。
//
// X68000 では 2 チャネルの用途が分かれている。
//   チャネル A ($E98004/$E98006) = RS-232C
//   チャネル B ($E98000/$E98002) = マウス
// 本エミュレータが要るのはマウス側だけだが、レジスタファイルは両チャネル分
// 持つ。IOCS は起動時に A も初期化するので (IPL-ROM $FF0DBC の 19 組)、
// 受け取り先が無いと書き込みが落ちる。
//
// 実装範囲: レジスタポインタ方式の読み書き、RR0 の状態ビット、チャネル B の
// 受信 (マウスレポート)、受信割り込み。ボーレート生成・DMA・SDLC・同期モードは
// 実装しない。マウスは 4800bps 固定で、エミュレータは実時間ではなくポーリング
// 契機でレポートを作るため、分周値を守っても得るものが無い。
//
// --- レジスタ配置の出典 -----------------------------------------------------
// rom/iplrom.dat (EXPERT 用 v1.0) を逆アセンブルして確かめた実測値。資料では
// A/B のアドレスが逆に書かれていることがあるので、ROM の実コードを根拠にする。
//
//   $FF0DBC: LEA $FF0E24,A0 / LEA $E98004,A1  → チャネル A の初期化表
//   $FF0DDC: LEA $FF0E4C,A0 / LEA $E98000,A1  → チャネル B の初期化表
//   $FF147E: MOVE.W #$0005,$E98000 / $FF1486: MOVE.W #$0062,$E98000
//            → マウス有効化。WR5 を選んでから $62 (RTS on) を書く
//   $FF144A: MOVE.W #$0005,$E98000 / $FF1452: MOVE.W #$0060,$E98000
//            → マウス無効化。同じ WR5 へ $60 (RTS off) を書く
//   $FF1512: MOVE.W $E98002,D0  → マウスの受信データはチャネル B のデータポート
//   $FF8042: MOVE.W #$0000,(A0) / MOVE.W (A0),D0 / AND.W #4,D0
//            → RR0 を読んで bit2 (送信バッファ空き) を見る
//
// つまり偶数オフセットが制御 (コマンド/ステータス)、奇数側が +2 でデータ。
//   $E98000 = ch B 制御 / $E98002 = ch B データ
//   $E98004 = ch A 制御 / $E98006 = ch A データ
//
// --- マウスのプロトコル -----------------------------------------------------
// 1 レポート = 3 バイト (ボタン, X 移動量, Y 移動量)。
//
// 出典は IPL-ROM $FF150A のチャネル B 受信ハンドラ。
//   $FF153A: MOVE.W #$0003,$092A   ← 残りバイト数を 3 で初期化
//   $FF1526: SUBQ.W #1,$092A / BNE ← 1 バイト受けるたびに減らす
//   $FF1554: LEA $0CB1,A0 / MOVE.B (A1)+,(A0)+ ×3
//            ← 0 になったら 3 バイトをワークへ写す
// 3 回の MOVE.B が展開されていることが「3 バイト固定」の直接の証拠になる。

#ifndef X68K_CORE_DEV_SCC_H
#define X68K_CORE_DEV_SCC_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Scc
{
public:
    // 書き込みレジスタの数。Z8530 は WR0-WR15。
    static constexpr u32 kRegCount = 16;

    // チャネル番号。X68000 では B がマウス。
    enum Channel : u32
    {
        kChannelB = 0,  // マウス ($E98000/$E98002)
        kChannelA = 1,  // RS-232C ($E98004/$E98006)
        kChannelCount = 2,
    };

    // RR0 (送受信状態) のビット。
    //
    // bit0 Rx Character Available / bit2 Tx Buffer Empty が要るところ。
    // IPL-ROM $FF8042 が bit2 を、受信割り込みハンドラが bit0 相当の
    // 「データがある」ことを前提に動く。
    static constexpr u8 kRr0RxAvailable = 0x01;
    static constexpr u8 kRr0Dcd = 0x08;
    static constexpr u8 kRr0TxEmpty = 0x04;
    static constexpr u8 kRr0Cts = 0x20;

    // WR1 のビット。受信割り込みの許可はここ。
    //
    // bit4-3 が受信割り込みモード。00=禁止 / 01=最初の文字のみ /
    // 10=全文字 / 11=特殊条件のみ。X68000 の初期化表 ($FF0E4C の最後) は
    // WR1 に $10 を書くので「全文字で割り込む」になる。
    static constexpr u8 kWr1RxIntMask = 0x18;
    static constexpr u8 kWr1RxIntDisabled = 0x00;

    // WR3 bit0 = 受信有効。これが立つまでは文字を積んでも意味が無い。
    static constexpr u8 kWr3RxEnable = 0x01;

    // WR5 bit1 = RTS。X68000 はこれをマウスへの「データを寄越せ」信号に使う。
    //
    // $FF146C がマウス有効化で $62 (RTS=1)、$FF1444 が無効化で $60 (RTS=0)
    // を WR5 へ書く。実機のマウスは RTS がアクティブな間だけレポートを返す。
    static constexpr u8 kWr5Rts = 0x02;

    // WR9 (マスタ割り込み制御) のビット。
    static constexpr u8 kWr9Mie = 0x08;         // 割り込み全体の許可
    static constexpr u8 kWr9StatusHigh = 0x10;  // ベクタの status を上位に入れる

    // WR0 のコマンド (bit5-3)。
    //
    // $38 = Reset Highest IUS。受信ハンドラの最後に書かれる ($FF1566)。
    // これを無視すると、Software EOI 相当の状態が残って次の割り込みが
    // 二度と通らない。
    static constexpr u8 kWr0CmdResetHighestIus = 0x38;
    static constexpr u8 kWr0CmdResetExtStatus = 0x10;
    static constexpr u8 kWr0CmdErrorReset = 0x30;

    void reset();

    // 制御ポート (RR/WR) を読む。
    //
    // const にできないのは、Z8530 のレジスタポインタが「読んだら 0 へ戻る」
    // ためと、RR2 の読み出しが割り込みベクタの確定を伴うため。
    [[nodiscard]] u8 readControl(u32 channel);
    void writeControl(u32 channel, u8 value);

    // データポートを読む。受信 FIFO から 1 バイト取り出す。
    [[nodiscard]] u8 readData(u32 channel);
    void writeData(u32 channel, u8 value);

    // ホストからマウスの状態を入れる。
    //
    // 実機のマウスは RTS がアクティブな間、一定間隔でレポートを送り返す。
    // ここでは「ホストが動きを知らせた時点で 1 レポート積む」形にする。
    //
    // Why not 実時間の 4800bps を再現するか: レポートの間隔はマウスの
    // 内部タイマで決まり、SCC のボーレートとは独立している。エミュレータは
    // ホストのイベント (タッチやマウス移動) が来た時にだけ動きがあるので、
    // 時間で駆動しても中身の無いレポートを積むだけになる。
    //
    // dx/dy は -128..127 に飽和させる。X68000 のレポートは 1 バイト符号付きで、
    // 折り返すとカーソルが逆方向へ飛ぶ。
    void moveMouse(int dx, int dy, bool leftButton, bool rightButton);

    // マウスのレポートを受け付けられる状態か。
    //
    // WR3 の受信有効と WR5 の RTS が両方立っている必要がある。IOCS が
    // マウスを無効化している間にレポートを積むと、有効化した瞬間に
    // 古い動きが一気に流れ込む。
    [[nodiscard]] bool isMouseEnabled() const;

    // 保留中の受信割り込みがあるか。
    [[nodiscard]] bool hasPendingInterrupt() const;

    // 割り込みを受理する。ベクタ番号を返し、保留を落とす。
    // 保留が無ければ 0 を返す。
    u32 acknowledgeInterrupt();

    // レジスタに書かれた値を副作用なしで見る。テストと調査用。
    //
    // readControl() はレジスタポインタを 0 へ戻すので、状態を調べる用途には
    // 使えない。「マウスが動かない」ときに RTS が立っているのか受信が
    // 無効なのかで原因が全く違うため、覗く手段が要る。
    [[nodiscard]] u8 peek(u32 channel, u32 reg) const;

    // 受信 FIFO に溜まっているバイト数。テストと調査用。
    [[nodiscard]] u32 pendingBytes(u32 channel) const;

private:
    // 受信 FIFO の深さ。
    //
    // Z8530 の実機は 3 段。ここは 3 バイトのレポートを 1 つ丸ごと保持し、
    // かつ IOCS が取りに来るまで少し余裕を持たせたいので 8 段にする。
    //
    // Why not 実機どおり 3 段にするか: 実機は 4800bps で 1 バイトずつ
    // 割り込みを上げ、CPU が都度引き取る。本エミュレータは 1 回の
    // moveMouse() で 3 バイトを一度に積むので、3 段だと CPU が引き取る前に
    // 次のレポートが来た時点で溢れる。溢れたレポートは「途中の 1 バイトだけ
    // 落ちる」形になり、ボタンと移動量の対応がずれてカーソルが暴れる。
    static constexpr u32 kRxFifoSize = 8;

    struct ChannelState
    {
        // WR0-WR15。RR は多くが WR と別実体なので、必要なものだけ個別に持つ。
        std::array<u8, kRegCount> wr{};
        // 次のアクセスが向かうレジスタ番号。WR0 への書き込みで設定され、
        // 1 回アクセスすると 0 へ戻る。
        u32 pointer = 0;
        std::array<u8, kRxFifoSize> rxFifo{};
        u32 rxCount = 0;
        // 受信割り込みが保留中か。
        bool rxInterruptPending = false;
    };

    [[nodiscard]] bool isValidChannel(u32 channel) const
    {
        return channel < kChannelCount;
    }

    // FIFO へ 1 バイト積む。溢れたら捨てる。
    void pushRxByte(u32 channel, u8 value);

    // 受信状態から RR0 を組み立てる。
    [[nodiscard]] u8 buildRr0(u32 channel) const;

    std::array<ChannelState, kChannelCount> ch_{};
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_SCC_H
