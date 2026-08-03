// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "scc.h"

namespace x68k
{
namespace
{

// レジスタポインタとして意味を持つのは下位 3bit + WR0 の bit5-3 のコマンド。
//
// Z8530 は WR0 の下位 3bit でレジスタ 0-7 を選び、bit3 が立った $08-$0F の
// 書き込みで 8-15 を選ぶ (Point High)。X68000 の初期化表は WR9/WR11/WR12/WR13/
// WR14/WR15 を使う ($FF0E4C) ので、8 以上を選べないと初期化が成立しない。
constexpr u8 kPointerMask = 0x07;
constexpr u8 kPointHigh = 0x08;

// WR0 のコマンド部 (bit5-3)。
constexpr u8 kCommandMask = 0x38;

// WR1 の受信割り込みモード。
constexpr u8 kRxIntFirstOnly = 0x08;
constexpr u8 kRxIntAllChars = 0x10;
constexpr u8 kRxIntSpecialOnly = 0x18;

// WR2 は割り込みベクタ。A/B で共通の実体を持つ (チップに 1 つしかない)。
constexpr u32 kWr2Vector = 2;

// マウスの移動量は 1 バイト符号付き。
constexpr int kMouseDeltaMin = -128;
constexpr int kMouseDeltaMax = 127;

// マウスレポート 1 バイト目 (ボタン状態) のビット。
//
// 出典: IPL-ROM $FF150A の受信ハンドラが写した 3 バイトを、IOCS の
// マウス状態取得 (IOCS _MS_GETDT) がそのまま返す。bit0 が右、bit1 が左。
//
// Why not bit0 を左にしないか: X68000 のマウスは右ボタンが下位ビット。
// 逆にすると SX-Window でクリックとコンテキストメニューが入れ替わる。
constexpr u8 kMouseButtonRight = 0x01;
constexpr u8 kMouseButtonLeft = 0x02;

// 移動量を 1 バイト符号付きへ飽和させる。
//
// Why not 切り捨てる (static_cast<i8>) か: 大きく動かしたときに
// 符号が反転し、カーソルが逆方向へ飛ぶ。飽和なら「速度が頭打ちになる」
// だけで済み、操作感として破綻しない。
u8 saturateDelta(int delta)
{
    if (delta < kMouseDeltaMin)
    {
        return static_cast<u8>(static_cast<s8>(kMouseDeltaMin));
    }
    if (delta > kMouseDeltaMax)
    {
        return static_cast<u8>(kMouseDeltaMax);
    }
    return static_cast<u8>(static_cast<s8>(delta));
}

}  // namespace

void Scc::reset()
{
    for (auto& channel : ch_)
    {
        channel.wr.fill(0);
        channel.pointer = 0;
        channel.rxFifo.fill(0);
        channel.rxCount = 0;
        channel.rxInterruptPending = false;
    }
}

u8 Scc::peek(u32 channel, u32 reg) const
{
    if (!isValidChannel(channel) || reg >= kRegCount)
    {
        return 0u;
    }
    return ch_[channel].wr[reg];
}

u32 Scc::pendingBytes(u32 channel) const
{
    if (!isValidChannel(channel))
    {
        return 0u;
    }
    return ch_[channel].rxCount;
}

u8 Scc::buildRr0(u32 channel) const
{
    const ChannelState& state = ch_[channel];

    // 送信は即座に完了したものとして扱うので、送信バッファは常に空。
    //
    // ここを 0 にすると IPL-ROM $FF8042 の「RR0 を読んで bit2 を待つ」
    // ループから出られない。本エミュレータは送信先を持たないので、
    // 待たせる理由が無い。
    u8 value = kRr0TxEmpty;

    // DCD と CTS は常にアクティブにしておく。
    //
    // マウスもホスト側の RS-232C も相手が居ない状態だが、これらを落とすと
    // IOCS が「回線が切れている」と判断して受信を止めることがある。
    value = static_cast<u8>(value | kRr0Dcd | kRr0Cts);

    if (state.rxCount > 0)
    {
        value = static_cast<u8>(value | kRr0RxAvailable);
    }
    return value;
}

u8 Scc::readControl(u32 channel)
{
    if (!isValidChannel(channel))
    {
        return 0u;
    }
    ChannelState& state = ch_[channel];

    const u32 reg = state.pointer;
    // ポインタは 1 回のアクセスで 0 へ戻る (Z8530 の仕様)。
    //
    // 戻さないと、IOCS が「WR5 を選んで書く」の直後に RR0 を読んだつもりで
    // RR5 相当を読んでしまう。$FF8042 は毎回 #0 を書いてから読むので
    // 実害が出にくいが、$FF15A4 のように選択を省く経路がある。
    state.pointer = 0;

    switch (reg)
    {
        case 0:
            return buildRr0(channel);

        case 1:
            // RR1 は特殊受信条件。エラーを起こさないので All Sent だけ立てる。
            return 0x01;

        case 2:
        {
            // RR2 は割り込みベクタ。
            //
            // チャネル B から読むと、実機は割り込みの原因に応じて status を
            // 埋め込んだ値を返す (WR9 の VIS が立っている場合)。X68000 の
            // 初期化表は WR9 に $40 を書くだけで VIS を立てないので、
            // WR2 の値をそのまま返す。
            return ch_[kChannelA].wr[kWr2Vector];
        }

        case 3:
            // RR3 は割り込み保留状態。チャネル A からしか読めない。
            //
            // bit2 = ch B 受信、bit5 = ch A 受信。マウスは ch B なので
            // bit2 に出す。
            {
                u8 value = 0;
                if (ch_[kChannelB].rxInterruptPending)
                {
                    value = static_cast<u8>(value | 0x04);
                }
                if (ch_[kChannelA].rxInterruptPending)
                {
                    value = static_cast<u8>(value | 0x20);
                }
                return channel == kChannelA ? value : 0u;
            }

        default:
            // RR4 以降は WR の別名が多い (RR4=WR4, RR5=WR5 等)。
            // 書かれた値をそのまま返しておけば、読み戻しで確認する
            // コードが破綻しない。
            return reg < kRegCount ? state.wr[reg] : 0u;
    }
}

void Scc::writeControl(u32 channel, u8 value)
{
    if (!isValidChannel(channel))
    {
        return;
    }
    ChannelState& state = ch_[channel];

    const u32 reg = state.pointer;
    state.pointer = 0;

    // WR0 への書き込みはレジスタ選択とコマンドを兼ねる。
    if (reg == 0)
    {
        // 下位 3bit が次のレジスタ番号。bit3 (Point High) が立っていれば +8。
        const u8 selected = static_cast<u8>(value & kPointerMask);
        const bool isPointHigh = (value & kPointHigh) != 0;
        state.pointer = static_cast<u32>(selected) + (isPointHigh ? 8u : 0u);

        const u8 command = static_cast<u8>(value & kCommandMask);
        if (command == kWr0CmdResetHighestIus)
        {
            // Reset Highest IUS。受信ハンドラの最後に書かれる ($FF1566)。
            //
            // 実機は「サービス中で最上位のもの」を落とすが、本エミュレータは
            // 受信割り込みしか上げないので、それを落とせば足りる。
            //
            // Why not 無視するか: 無視しても acknowledgeInterrupt() が
            // 保留を落とすので一見動く。しかし IOCS がハンドラ内で
            // これを書いた後に再度割り込みを許すため、落とし忘れると
            // 「サービス中」が残る実装へ拡張したときに固まる。
            state.rxInterruptPending = false;
        }
        if (command == kWr0CmdErrorReset)
        {
            // Error Reset。受信エラーを起こさないので落とすものが無い。
            return;
        }
        return;
    }

    // WR0 以外は素直に格納する。
    if (reg < kRegCount)
    {
        state.wr[reg] = value;
    }

    // 受信を無効にされたら、溜まっているデータを捨てる。
    //
    // WR3 bit0 (受信有効) が落ちた状態でデータを持ち越すと、次に有効化した
    // 瞬間に古いレポートが流れ込む。マウスなら「触っていないのにカーソルが
    // 飛ぶ」形で出る。
    const bool isRxControl = reg == 3;
    if (isRxControl && (value & kWr3RxEnable) == 0)
    {
        state.rxCount = 0;
        state.rxInterruptPending = false;
    }

    // 割り込みモードを禁止にされたら保留も落とす。
    //
    // MFP の IER と同じ考え方。禁止した後に保留が残っていると、
    // ハンドラを外した後で受理されて不正なベクタへ飛ぶ。
    const bool isIntControl = reg == 1;
    if (isIntControl && (value & kWr1RxIntMask) == kWr1RxIntDisabled)
    {
        state.rxInterruptPending = false;
    }
}

u8 Scc::readData(u32 channel)
{
    if (!isValidChannel(channel))
    {
        return 0u;
    }
    ChannelState& state = ch_[channel];

    if (state.rxCount == 0)
    {
        return 0u;
    }

    const u8 value = state.rxFifo[0];
    // 先頭を取り出して詰める。
    //
    // Why not リングバッファにするか: 段数が 8 しかなく、取り出しは
    // 1 レポート 3 バイトの単位でしか起きない。読み書きの添字を 2 つ
    // 持つより、ずらす方が状態が 1 つ減って追いやすい。
    for (u32 i = 1; i < state.rxCount; ++i)
    {
        state.rxFifo[i - 1] = state.rxFifo[i];
    }
    --state.rxCount;

    // FIFO が空になったら受信割り込みの保留も落とす。
    //
    // 実機の Z8530 は文字を取り出すと RR0 の bit0 が下り、割り込みの要因が
    // 消える。残したままにすると、IOCS が全部読み切った後も割り込みが
    // 上がり続けて先へ進まない。
    if (state.rxCount == 0)
    {
        state.rxInterruptPending = false;
    }

    return value;
}

void Scc::writeData(u32 channel, u8 value)
{
    // 送信データ。X68000 ではマウスへのコマンドと RS-232C の送出に使う。
    //
    // 送信先を持たないので捨てるが、RR0 の送信バッファ空きは常に立てたまま
    // にする (buildRr0 を参照)。ここで空きを落とすと IPL-ROM $FF8042 の
    // 待ちループから出られない。
    (void)channel;
    (void)value;
}

bool Scc::isMouseEnabled() const
{
    const ChannelState& state = ch_[kChannelB];
    const bool isRxEnabled = (state.wr[3] & kWr3RxEnable) != 0;
    const bool isRtsAsserted = (state.wr[5] & kWr5Rts) != 0;
    return isRxEnabled && isRtsAsserted;
}

void Scc::pushRxByte(u32 channel, u8 value)
{
    ChannelState& state = ch_[channel];
    if (state.rxCount >= kRxFifoSize)
    {
        // 溢れたら捨てる。実機はオーバーランエラーを立てるが、
        // 捨てた側を IOCS に知らせる手段が無いので、静かに落とす方が
        // 「途中の 1 バイトだけ欠けたレポート」を作らずに済む。
        return;
    }
    state.rxFifo[state.rxCount++] = value;

    // 受信割り込みが許可されていれば保留を立てる。
    //
    // WR1 の bit4-3 が受信割り込みモード。X68000 の初期化表は $10
    // (全文字で割り込む) を書く。00 (禁止) のときは保留にもしない。
    const u8 mode = static_cast<u8>(state.wr[1] & kWr1RxIntMask);
    const bool wantsInterrupt =
        mode == kRxIntAllChars || mode == kRxIntFirstOnly || mode == kRxIntSpecialOnly;
    if (!wantsInterrupt)
    {
        return;
    }
    // マスタ割り込み許可 (WR9 bit3) も要る。WR9 はチップ共通なので
    // チャネル A 側の実体を見る。
    const bool isMasterEnabled = (ch_[kChannelA].wr[9] & kWr9Mie) != 0;
    if (!isMasterEnabled)
    {
        return;
    }
    state.rxInterruptPending = true;
}

void Scc::moveMouse(int dx, int dy, bool leftButton, bool rightButton)
{
    // 無効化中は積まない。有効化した瞬間に古い動きが流れ込むのを避ける。
    if (!isMouseEnabled())
    {
        return;
    }

    u8 buttons = 0;
    if (leftButton)
    {
        buttons = static_cast<u8>(buttons | kMouseButtonLeft);
    }
    if (rightButton)
    {
        buttons = static_cast<u8>(buttons | kMouseButtonRight);
    }

    // 3 バイトを一度に積む。IOCS 側は 1 バイトずつ割り込みで引き取り、
    // 3 バイト目でワークへ写す ($FF1554 の MOVE.B ×3)。
    pushRxByte(kChannelB, buttons);
    pushRxByte(kChannelB, saturateDelta(dx));
    pushRxByte(kChannelB, saturateDelta(dy));
}

bool Scc::hasPendingInterrupt() const
{
    return ch_[kChannelA].rxInterruptPending || ch_[kChannelB].rxInterruptPending;
}

u32 Scc::acknowledgeInterrupt()
{
    if (!hasPendingInterrupt())
    {
        return 0u;
    }

    // ベクタは WR2 に書かれた値をそのまま使う。
    //
    // X68000 の IOCS は WR2 に $70 を書き、$70/$71 (チャネル B 送受信) と
    // $74/$75 (チャネル A) を使う。VIS (WR9 bit4) は立てないので、
    // status によるベクタの変化は起きない。
    //
    // Why not 自動ベクタにするか: SCC は自分のベクタを返すデバイスで、
    // 自動ベクタにすると IOCS が張ったマウス用ハンドラへ届かない。
    // MFP を自動ベクタにしたときと同じ壊れ方 (不正ベクタへ飛ぶ) になる。
    const u8 base = ch_[kChannelA].wr[kWr2Vector];

    // チャネル B (マウス) を優先する。実機の SCC も B が上位。
    if (ch_[kChannelB].rxInterruptPending)
    {
        ch_[kChannelB].rxInterruptPending = false;
        // ch B 受信 = ベース +1。
        return static_cast<u32>(base) + 1u;
    }

    ch_[kChannelA].rxInterruptPending = false;
    // ch A 受信 = ベース +5。
    return static_cast<u32>(base) + 5u;
}

}  // namespace x68k
