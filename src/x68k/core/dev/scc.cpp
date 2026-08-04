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

// ベクタに埋め込む status (V3-V1)。Z8530 は要因ごとの 3bit 符号を bit3-1 に
// 入れるので、ベースへの加算値は「符号 × 2」になる。
//
//   000 ch B 送信空き / 001 ch B 外部状態 / 010 ch B 受信 / 011 ch B 特殊受信
//   100 ch A 送信空き / 101 ch A 外部状態 / 110 ch A 受信 / 111 ch A 特殊受信
//
// 使うのは受信の 2 つだけ。010 → +4、110 → +12。
constexpr u32 kVectorStatusRxB = 4;
constexpr u32 kVectorStatusRxA = 12;

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
    sharedWr_.fill(0);
}

u8 Scc::peek(u32 channel, u32 reg) const
{
    if (!isValidChannel(channel) || reg >= kRegCount)
    {
        return 0u;
    }
    // 共通レジスタはどちらのチャネルから覗いても同じ値を返す。
    if (isSharedReg(reg))
    {
        return sharedWr_[reg];
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
            // RR2 は割り込みベクタ。WR2 はチップ共通なので共通の実体から返す。
            //
            // チャネル A から読むと WR2 の素の値、チャネル B から読むと
            // 割り込みの原因に応じた status 込みの値を返すのが実機。
            // VIS (WR9 bit0) が立っていない場合は status を埋めない。
            //
            // Why not 常に素の値を返すか: IPL-ROM の初期化表は最後に
            // WR9 = $09 を書く ($FF0E6E) ので VIS が立つ。status を
            // 埋めないと、RR2 を読んで分岐する経路が要因を区別できない。
            const u8 base = sharedWr_[kWr2Vector];
            const bool isVis = (sharedWr_[9] & kWr9Vis) != 0;
            if (channel != kChannelB || !isVis)
            {
                return base;
            }
            if (ch_[kChannelB].rxInterruptPending)
            {
                return static_cast<u8>(base + kVectorStatusRxB);
            }
            if (ch_[kChannelA].rxInterruptPending)
            {
                return static_cast<u8>(base + kVectorStatusRxA);
            }
            return base;
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
            // Reset Highest IUS。受信ハンドラの最後に書かれる ($FF1564 の
            // MOVE.W #$0038,$E98000)。「サービス中」の印を落とすだけで、
            // 割り込みの要因そのものを消すコマンドではない。
            //
            // Why not 無条件に保留を落とさないか: ハンドラは 1 バイトしか
            // 読まずにこれを書く。FIFO に 2・3 バイト目が残っている状態で
            // 保留まで落とすと、次の受信割り込みが上がらず、ROM 側の
            // $092A のカウンタが 3 のまま止まってマウスが 1 バイトしか
            // 届かない。残りがあるかどうかで張り直す。
            refreshRxInterrupt(channel);
        }
        if (command == kWr0CmdErrorReset)
        {
            // Error Reset。受信エラーを起こさないので落とすものが無い。
            return;
        }
        return;
    }

    // WR0 以外は素直に格納する。WR2/WR9 だけはチップ共通の実体へ。
    if (reg < kRegCount)
    {
        if (isSharedReg(reg))
        {
            sharedWr_[reg] = value;
        }
        else
        {
            state.wr[reg] = value;
        }
    }

    // MIE を落とされたら両チャネルの保留を落とす。
    //
    // WR9 はチップ共通なので、片方のチャネルから禁止しても効くのが実機。
    // 落とし忘れると、IOCS が割り込みを止めたつもりの区間で受理が起きる。
    const bool isMasterControl = reg == 9;
    if (isMasterControl && (value & kWr9Mie) == 0)
    {
        for (auto& target : ch_)
        {
            target.rxInterruptPending = false;
        }
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

    // 割り込みモードが変わったら保留を張り直す。
    //
    // 禁止にされたら落とす。MFP の IER と同じ考え方で、禁止した後に
    // 保留が残っていると、ハンドラを外した後で受理されて不正なベクタへ飛ぶ。
    // 逆に許可されたときは、既に FIFO にあるデータが要因として立つ。
    const bool isIntControl = reg == 1;
    if (isIntControl)
    {
        refreshRxInterrupt(channel);
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

    // 残りに応じて保留を張り直す。空になれば落ち、まだ残っていれば
    // もう一度上がる。
    //
    // 実機の Z8530 は文字を取り出すと RR0 の bit0 が下り、空になった時点で
    // 割り込みの要因が消える。逆に残っている間は要因が消えないので、
    // ハンドラを抜けた直後に次の受信割り込みが上がる。これが
    // 「1 バイト = 1 割り込み」を成り立たせている。
    refreshRxInterrupt(channel);

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

bool Scc::wantsRxInterrupt(u32 channel) const
{
    // WR1 の bit4-3 が受信割り込みモード。X68000 の初期化表は $10
    // (全文字で割り込む) を書く ($FF0E6C)。00 (禁止) のときは上げない。
    const u8 mode = static_cast<u8>(ch_[channel].wr[1] & kWr1RxIntMask);
    const bool isModeEnabled =
        mode == kRxIntAllChars || mode == kRxIntFirstOnly || mode == kRxIntSpecialOnly;
    if (!isModeEnabled)
    {
        return false;
    }
    // マスタ割り込み許可 (WR9 bit3) も要る。WR9 はチップ共通の実体。
    return (sharedWr_[9] & kWr9Mie) != 0;
}

void Scc::refreshRxInterrupt(u32 channel)
{
    ChannelState& state = ch_[channel];

    // FIFO に 1 バイトでも残っていれば割り込みを上げ続ける。
    //
    // 実機の Z8530 は「受信文字がある」という状態(レベル)で割り込みを出す。
    // IPL-ROM $FF150A のハンドラは 1 回で 1 バイトしか読まず ($FF1512 の
    // MOVE.W $E98002,D0)、$092A のカウンタを 1 つ減らして抜ける
    // ($FF1526 の SUBQ.W #1)。3 バイト揃うのは 3 回目の割り込みのときで、
    // そこで初めて $0CB1 へ写す ($FF1554 の MOVE.B ×3)。
    //
    // Why not 1 レポートを 1 回の割り込みで渡さないか: ハンドラが 1 回で
    // 1 バイトしか引き取らない以上、割り込みを 1 回しか上げなければ
    // 2 バイト目以降が FIFO に残ったまま二度と読まれない。ROM 側の
    // カウンタは 3 のまま止まり、マウスはボタンの 1 バイトしか届かない。
    state.rxInterruptPending = state.rxCount > 0 && wantsRxInterrupt(channel);
}

void Scc::pushRxByte(u32 channel, u8 value)
{
    ChannelState& state = ch_[channel];
    if (state.rxCount >= kRxFifoSize)
    {
        // 呼び出し側が空きを確かめてから積む契約なので、ここへは来ない。
        return;
    }
    state.rxFifo[state.rxCount++] = value;
    refreshRxInterrupt(channel);
}

bool Scc::moveMouse(int dx, int dy, bool leftButton, bool rightButton)
{
    // 無効化中は積まない。有効化した瞬間に古い動きが流れ込むのを避ける。
    if (!isMouseEnabled())
    {
        return false;
    }

    // レポートは 3 バイト揃って初めて意味を持つので、全部入らないなら
    // 1 バイトも積まない。
    //
    // Why not 入る分だけ積まないか: IOCS のハンドラは $092A のカウンタで
    // 3 バイトを数えており ($FF153A で 3 を設定)、境界を知る手段が無い。
    // 途中で切れたレポートを渡すと以降のバイト境界が恒久的にずれ、
    // 次のレポートのボタン値が X 移動量として読まれてカーソルが暴れる。
    // 丸ごと捨てれば「その 1 回の動きが無かった」だけで同期は保たれる。
    const u32 freeSlots = kRxFifoSize - ch_[kChannelB].rxCount;
    if (freeSlots < kMouseReportBytes)
    {
        return false;
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

    // IOCS 側は 1 バイトずつ割り込みで引き取り、3 バイト目でワークへ写す
    // ($FF1554 の MOVE.B ×3)。
    pushRxByte(kChannelB, buttons);
    pushRxByte(kChannelB, saturateDelta(dx));
    pushRxByte(kChannelB, saturateDelta(dy));
    return true;
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

    // ベクタは WR2 のベースに、割り込み要因を表す status を埋め込んだ値。
    //
    // Z8530 は status を V3-V1 (bit3-1) に入れる。受信可能の符号は
    // チャネル B が 010、チャネル A が 110 なので、ベース +4 / +12 になる。
    // WR9 の Status High (bit4) が立っていれば V6-V4 (bit6-4) 側へ入るが、
    // X68000 は立てないので下位側で固定してよい。
    //
    // 根拠は IPL-ROM の実測値で二重に取れている。
    //   $FF0E2C: チャネル A の初期化表が WR2 = $50 を書く
    //            (WR2 もチップ共通なので、これがチップ全体のベース)
    //   $FF0DA2: LEA $00000140,A1 / $FF0DA8: LEA $FF0E04,A0 /
    //            MOVE.W #7,D1 のループが $FF0E04 の 8 エントリを
    //            1 つずつ 2 回書いて $140-$17F (ベクタ $50-$5F) を埋める
    //   その並びでマウスの受信ハンドラ $FF150A が入るのはベクタ $54/$55。
    // つまり $50 + 4 = $54 で、Z8530 の符号化と ROM のベクタ表が一致する。
    //
    // Why not 素朴に「ベース +1」にしないか: それでは $51 になり、
    // $FF0E04 の表では $FF15C0 (未使用要因の共通ハンドラ) へ飛ぶ。
    // マウスのハンドラが一度も呼ばれず、カーソルが動かない。
    //
    // Why not 自動ベクタにするか: SCC は自分のベクタを返すデバイスで、
    // 自動ベクタにすると IOCS が張ったマウス用ハンドラへ届かない。
    // MFP を自動ベクタにしたときと同じ壊れ方 (不正ベクタへ飛ぶ) になる。
    const u8 base = sharedWr_[kWr2Vector];

    // チャネル B (マウス) を優先する。実機の SCC も B が上位。
    if (ch_[kChannelB].rxInterruptPending)
    {
        ch_[kChannelB].rxInterruptPending = false;
        return static_cast<u32>(base) + kVectorStatusRxB;
    }

    ch_[kChannelA].rxInterruptPending = false;
    return static_cast<u32>(base) + kVectorStatusRxA;
}

}  // namespace x68k
