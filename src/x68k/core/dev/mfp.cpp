// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "mfp.h"

namespace x68k
{
namespace
{

// タイマ制御レジスタの下位 3bit が分周比を表す。
// 0 は停止、1-7 が それぞれ 4/10/16/50/64/100/200 分周。
constexpr u32 kPrescaleTable[8] = {0, 4, 10, 16, 50, 64, 100, 200};

// X68000 の MFP のクロックは 4MHz。CPU は 10MHz なので、
// CPU サイクルを MFP サイクルへ換算する必要がある。
// 分数のままだと毎回割り算が入るので、CPU サイクルを 2 で割って近似する
// (4/10 ≒ 1/2.5 だが、タイマ精度は Human68k の起動には影響しない)。
constexpr u32 kCpuToMfpShift = 1;

// GPIP4 (垂直帰線) のビット位置。
constexpr u8 kGpipVDisp = 0x10;

// TSR (送信状態レジスタ) の bit7 = 送信バッファ空き。
// IPL-ROM はキーボードへコマンドを送る前にこれを待つ。
constexpr u8 kTsrBufferEmpty = 0x80;

}  // namespace

void Mfp::reset()
{
    reg_.fill(0);
    prescaleCounter_.fill(0);
    timerValue_.fill(0);
    // GPIP の初期値。
    //
    // X68000 での割り当て:
    //   bit0 (未使用) / bit1 EXPON (拡張ボード) / bit2 POWER (電源スイッチ)
    //   bit3 OPMIRQ (FM音源) / bit4 V-DISP (垂直帰線) / bit5 (未使用)
    //   bit6 CIRQ (RTC アラーム) / bit7 HSYNC (水平同期)
    //
    // IPL-ROM は起動時に bit1/bit2 が 0 になるのを待つループを持つ
    // ($FF103C)。拡張ボードも電源スイッチの押下も無い状態を表すため、
    // これらは L (0) にしておく。全ビットを H にするとタイムアウトするまで
    // 無駄に回り続ける。
    constexpr u8 kGpipExpansion = 0x02;  // EXPON
    constexpr u8 kGpipPower = 0x04;      // POWER
    reg_[kGpip] = static_cast<u8>(0xFF & ~(kGpipExpansion | kGpipPower));

    // 送信バッファは空の状態で始める。
    //
    // TSR の bit7 (バッファ空き) を 0 のままにすると、IPL-ROM が
    // キーボードへコマンドを送るところで永久に待ち続ける
    // ($FF61C8 の「TSR を読んで bit7 を BTST し、0 なら戻る」ループ)。
    // 本エミュレータは送信を即座に完了したものとして扱うので、
    // 常に空いていることにする。
    reg_[kTsr] = kTsrBufferEmpty;
}

u8 Mfp::read(u32 regIndex)
{
    if (regIndex >= kRegCount)
    {
        return 0u;
    }

    // タイマのデータレジスタは、読むとメインカウンタの現在値が返る
    // (MC68901 の仕様)。reg_ が持つのは次のリロード値で、動作中に
    // 書き換えられると現在値と食い違う。
    //
    // Why not reg_ をそのまま返すか: 経過時間を測るためにカウンタを
    // ポーリングするコードが、いつ読んでも同じ値を見ることになる。
    static constexpr u32 kTimerDataRegs[4] = {kTadr, kTbdr, kTcdr, kTddr};
    for (std::size_t i = 0; i < 4; ++i)
    {
        if (regIndex == kTimerDataRegs[i])
        {
            return timerValue_[i];
        }
    }

    const u8 value = reg_[regIndex];

    // 受信データを読んだら「受信バッファフル」を落とす。
    //
    // 実機の MC68901 は UDR を読むと RSR の bit7 が下りる。落とさないと、
    // ゲストが「まだ読んでいないデータがある」と判断し続ける。
    //
    // Why not 読み出しを const のままにするか: フラグの更新は実機の
    // 副作用そのもので、隠すと「読んだのに状態が変わらない」という
    // 実機と違う振る舞いになる。
    const bool isReceiveData = regIndex == kUdr;
    if (isReceiveData)
    {
        reg_[kRsr] = static_cast<u8>(reg_[kRsr] & ~0x80u);
    }

    return value;
}

void Mfp::write(u32 regIndex, u8 value)
{
    if (regIndex >= kRegCount)
    {
        return;
    }

    switch (regIndex)
    {
        case kIpra:
        case kIprb:
        case kIsra:
        case kIsrb:
            // 保留/サービス中レジスタは「書いたビットを 0 にする」という
            // 特殊な動作をする (1 を書いても立たない)。割り込みの取り下げに使う。
            reg_[regIndex] &= value;
            return;

        // タイマのデータレジスタ。
        //
        // 動作中に書いた値は、カウンタが 0 まで下りてリロードされるときに
        // 初めて効く (MC68901 の仕様)。ここで timerValue_ を直接書くと、
        // 動いているタイマが即座に再スタートして割り込みの間隔が変わる。
        // 停止中なら実機もすぐ反映するので、そのときだけ写す。
        //
        // リロードは tickTimer が reg_ から読むので、書き込み側は reg_ を
        // 更新するだけでよい。
        case kTadr:
            reg_[kTadr] = value;
            loadTimerIfStopped(0, reg_[kTacr], value);
            return;
        case kTbdr:
            reg_[kTbdr] = value;
            loadTimerIfStopped(1, reg_[kTbcr], value);
            return;
        case kTcdr:
            reg_[kTcdr] = value;
            // タイマ C は TCDCR の上位 3bit。
            loadTimerIfStopped(2, static_cast<u8>((reg_[kTcdcr] >> 4) & 7u), value);
            return;
        case kTddr:
            reg_[kTddr] = value;
            // タイマ D は TCDCR の下位 3bit。
            loadTimerIfStopped(3, static_cast<u8>(reg_[kTcdcr] & 7u), value);
            return;

        case kGpip:
            // GPIP は入力なので書き込みは DDR で出力に設定されたビットのみ有効。
            // X68000 では実質入力専用なので無視する。
            return;

        case kUdr:
            // 送信データレジスタ。X68000 ではキーボードへのコマンド送信に使う
            // (LED の制御など)。本エミュレータは送信先を持たないので捨てるが、
            // 送信は即座に完了したものとして TSR の空きビットは立てたままにする。
            // ここを 0 にすると IPL-ROM が次の送信で待ち続ける。
            return;

        case kTsr:
            // 送信状態レジスタ。空きビットは常に立てておく (上記と同じ理由)。
            reg_[kTsr] = static_cast<u8>(value | kTsrBufferEmpty);
            return;

        // タイマ制御レジスタ。停止させたらプリスケーラの端数を捨てる。
        //
        // 実機は停止するとメインカウンタは保つがプリスケーラの残量は失う。
        // 残したままだと、停止して設定し直して再開したとき、最初の 1 周期
        // だけ最大 1 プリスケールぶん早く割り込みが出る。
        case kTacr:
            reg_[kTacr] = value;
            clearPrescalerIfStopped(0, value);
            return;
        case kTbcr:
            reg_[kTbcr] = value;
            clearPrescalerIfStopped(1, value);
            return;
        case kTcdcr:
            reg_[kTcdcr] = value;
            // C は上位 3bit、D は下位 3bit。別々に見る。
            clearPrescalerIfStopped(2, static_cast<u8>((value >> 4) & 7u));
            clearPrescalerIfStopped(3, static_cast<u8>(value & 7u));
            return;

        // ベクタレジスタ。S を落としたら ISR を捨てる。
        //
        // S=0 (Automatic EOI) の間、ISR は強制的に 0 に保たれる。
        // 残したままだと、Software EOI から切り替えた後もサービス中と
        // みなされ、それ以下の優先度が二度と受理されなくなる。
        case kVr:
        {
            reg_[kVr] = value;
            const bool isAutomaticEoi = (value & kVrSoftwareEoi) == 0;
            if (isAutomaticEoi)
            {
                reg_[kIsra] = 0;
                reg_[kIsrb] = 0;
            }
            return;
        }

        // 割り込み許可レジスタ。禁止したチャネルの保留を落とす。
        //
        // MC68901 は IER のビットを 0 にすると、対応する IPR のビットも
        // 同時に落とす。代入だけだと、割り込みを禁止した後も保留が残り、
        // ハンドラを外した後に受理されて不正なベクタへ飛ぶ。
        case kIera:
            reg_[kIera] = value;
            reg_[kIpra] = static_cast<u8>(reg_[kIpra] & value);
            return;
        case kIerb:
            reg_[kIerb] = value;
            reg_[kIprb] = static_cast<u8>(reg_[kIprb] & value);
            return;

        default:
            reg_[regIndex] = value;
            return;
    }
}

void Mfp::clearPrescalerIfStopped(int index, u8 control)
{
    const bool isStopped = (control & 0x0Fu) == 0;
    if (isStopped)
    {
        prescaleCounter_[static_cast<std::size_t>(index)] = 0;
    }
}

u32 Mfp::timerPrescale(u8 control) const
{
    // ディレイモード ($01-$07) 以外は分周しない。
    //
    // タイマ A/B の制御レジスタは 4bit あり、bit3 が立つと外部入力 TAI/TBI が
    // 関わるモードになる。$08 (イベントカウント) は入力の有効エッジ自体が
    // カウント源で、$09-$0F (パルス幅測定) は入力が有効な間だけ内部クロックで
    // 動く。前者はクロック源、後者はゲートという違いはあるが、どちらも入力を
    // 見ずには進められない。本エミュレータは TAI/TBI を実装していないので、
    // 一律に進めない。
    //
    // ここで 0 を返さないと、下位 3bit がそのまま分周比として拾われ、
    // 入力に関係なくカウンタが減り続ける。
    //
    // タイマ C/D は 3bit しか無く、呼び出し側が既に 7 でマスクしている
    // ため bit3 は立たない。この判定は素通りする。
    const bool needsExternalInput = (control & 0x08u) != 0;
    if (needsExternalInput)
    {
        return 0;
    }
    return kPrescaleTable[control & 7u];
}

void Mfp::loadTimerIfStopped(int index, u8 control, u8 value)
{
    // 停止中だけカウンタへ写す。動作中は次のリロードまで待つ。
    //
    // Why not timerPrescale(control) == 0 で判定しないか: あれは「内部クロックで
    // 進むか」を返す。$08 (イベントカウント) と $09-$0F (パルス幅測定) でも 0 に
    // なるが、これらは停止ではなく外部入力 TAI/TBI で動く状態。$00 だけが停止。
    //
    // 本エミュレータは外部入力の 2 モードを実装しておらず、実際にはカウンタが
    // 動かない。それでも「停止」と一緒くたにしないのは、実装したときにここが
    // 誤ったままになるのを避けるため。
    //
    // なおパルス幅測定は、制御値だけでは停止か動作中かを決められない
    // (TAI/TBI が非アクティブな間だけ止まる)。実装するときは入力の状態も要る。
    const bool isStopped = (control & 0x0Fu) == 0;
    if (isStopped)
    {
        timerValue_[static_cast<std::size_t>(index)] = value;
    }
}

void Mfp::raise(bool groupA, u8 bit)
{
    const u32 ierIndex = groupA ? kIera : kIerb;
    const u32 iprIndex = groupA ? kIpra : kIprb;

    // 許可されていない割り込みは保留にもならない。
    if ((reg_[ierIndex] & bit) == 0)
    {
        return;
    }
    reg_[iprIndex] |= bit;
}

void Mfp::tickTimer(int index, u8 control, u32 cycles)
{
    const u32 prescale = timerPrescale(control);
    if (prescale == 0)
    {
        // 停止中か、TAI/TBI を要するモード。後者は入力を実装していないので、
        // 経過サイクルだけでは進めようがない。
        return;
    }

    prescaleCounter_[static_cast<std::size_t>(index)] += cycles;
    while (prescaleCounter_[static_cast<std::size_t>(index)] >= prescale)
    {
        prescaleCounter_[static_cast<std::size_t>(index)] -= prescale;

        u8& value = timerValue_[static_cast<std::size_t>(index)];
        if (value == 0)
        {
            // 0 からのデクリメントは 256 として扱う (データレジスタの 0 は 256)。
            value = 0xFF;
        }
        else
        {
            --value;
        }

        if (value != 0)
        {
            continue;
        }

        // タイムアウト。データレジスタの値でリロードして割り込みを上げる。
        static constexpr u32 kDataReg[4] = {kTadr, kTbdr, kTcdr, kTddr};
        value = reg_[kDataReg[static_cast<std::size_t>(index)]];

        switch (index)
        {
            case 0:
                raise(true, kIntTimerA);
                break;
            case 1:
                raise(true, kIntTimerB);
                break;
            case 2:
                raise(false, kIntTimerC);
                break;
            default:
                raise(false, kIntTimerD);
                break;
        }
    }
}

void Mfp::tick(u32 cycles)
{
    const u32 mfpCycles = cycles >> kCpuToMfpShift;
    if (mfpCycles == 0)
    {
        return;
    }

    tickTimer(0, reg_[kTacr], mfpCycles);
    tickTimer(1, reg_[kTbcr], mfpCycles);
    // タイマ C は TCDCR の上位 3bit、タイマ D は下位 3bit。
    tickTimer(2, static_cast<u8>((reg_[kTcdcr] >> 4) & 7u), mfpCycles);
    tickTimer(3, static_cast<u8>(reg_[kTcdcr] & 7u), mfpCycles);
}

void Mfp::setVerticalBlank(bool active)
{
    const u8 before = reg_[kGpip];
    if (active)
    {
        // 垂直帰線中は GPIP4 が L になる。
        reg_[kGpip] = static_cast<u8>(before & ~kGpipVDisp);
    }
    else
    {
        reg_[kGpip] = static_cast<u8>(before | kGpipVDisp);
    }

    if (reg_[kGpip] == before)
    {
        return;
    }

    // AER で指定されたエッジのときだけ割り込みを上げる。
    const bool risingEdgeWanted = (reg_[kAer] & kGpipVDisp) != 0;
    const bool isRising = (reg_[kGpip] & kGpipVDisp) != 0;
    if (isRising == risingEdgeWanted)
    {
        raise(false, kIntGpip4);
    }
}

void Mfp::receiveKeyboardByte(u8 value)
{
    reg_[kUdr] = value;
    // 受信バッファフル。
    reg_[kRsr] |= 0x80;
    raise(true, kIntRecvFull);
}

u8 Mfp::serviceBlockMask(bool groupA) const
{
    // Software EOI (VR bit3 = 1) でサービス中のチャネルがあるとき、
    // それ以下の優先度を抑止するマスクを返す。抑止しないビットが 1。
    //
    // 優先度はグループ A の bit7 が最上位、グループ B の bit0 が最下位で、
    // 16 段階が一列に並ぶ。サービス中のチャネルより真に高い優先度だけが
    // 割り込める。同じチャネルの再入も許さない。
    //
    // Automatic EOI (S=0) では ISR が常に 0 なので、この関数は全許可を返す。
    // X68000 の IOCS は S=0 を使うため、実機ではこちらの経路しか通らない。
    const u8 isrA = reg_[kIsra];
    const u8 isrB = reg_[kIsrb];

    // サービス中が無ければ何も抑止しない。
    //
    // hasPendingInterrupt() は毎命令通るので、ここを早く抜けることが効く。
    // X68000 は Automatic EOI (S=0) を使い ISR が常に 0 なので、実機では
    // 必ずこの分岐で返る。下のループを毎回回すと実効クロックが 14% 落ちた。
    if ((isrA | isrB) == 0)
    {
        return 0xFF;
    }

    // サービス中のチャネルが居るグループ。A が居れば A が上位。
    const bool servicedInGroupA = isrA != 0;

    // 別のグループを問われたら、上下は丸ごと決まる。
    if (groupA != servicedInGroupA)
    {
        // A がサービス中で B を問われた → B は全て下位なので全抑止。
        // B がサービス中で A を問われた → A は全て上位なので全許可。
        return groupA ? 0xFF : 0x00;
    }

    // 同じグループ内では、最上位のサービス中ビットより上だけが通る。
    const u8 serviced = servicedInGroupA ? isrA : isrB;
    u8 allowed = 0;
    for (int bit = 7; bit >= 0; --bit)
    {
        const u8 mask = static_cast<u8>(1u << bit);
        if ((serviced & mask) != 0)
        {
            break;
        }
        allowed = static_cast<u8>(allowed | mask);
    }
    return allowed;
}

bool Mfp::hasPendingInterrupt() const
{
    const u8 pendingA = static_cast<u8>(reg_[kIpra] & reg_[kImra] & serviceBlockMask(true));
    const u8 pendingB = static_cast<u8>(reg_[kIprb] & reg_[kImrb] & serviceBlockMask(false));
    return (pendingA | pendingB) != 0;
}

u32 Mfp::acknowledgeInterrupt()
{
    // 優先度はグループ A の bit7 が最上位、グループ B の bit0 が最下位。
    // ベクタ番号は VR の上位 4bit + 割り込み番号 (0-15)。
    // Software EOI でサービス中のものがあれば、それ以下は受理しない。
    const u8 pendingA = static_cast<u8>(reg_[kIpra] & reg_[kImra] & serviceBlockMask(true));
    const u8 pendingB = static_cast<u8>(reg_[kIprb] & reg_[kImrb] & serviceBlockMask(false));

    // VR bit3 (S) が EOI の方式を選ぶ。
    //
    // S=0 (Automatic EOI) では受理と同時にサービスが終わったものとして扱い、
    // ISR は常に 0 のまま。S=1 (Software EOI) のときだけ ISR が立ち、
    // ハンドラが明示的に落とすまでサービス中として残る。
    //
    // X68000 の IOCS は VR に $40 を書く (S=0) ので、実機で使われるのは
    // Automatic EOI の側。無条件に ISR を立てると、実機では 0 のはずの
    // レジスタが埋まっていく。
    const bool isSoftwareEoi = (reg_[kVr] & kVrSoftwareEoi) != 0;

    for (int bit = 7; bit >= 0; --bit)
    {
        const u8 mask = static_cast<u8>(1u << bit);
        if ((pendingA & mask) != 0)
        {
            reg_[kIpra] = static_cast<u8>(reg_[kIpra] & ~mask);
            if (isSoftwareEoi)
            {
                reg_[kIsra] |= mask;
            }
            const u32 number = static_cast<u32>(bit) + 8u;
            return (static_cast<u32>(reg_[kVr] & 0xF0u)) | number;
        }
    }
    for (int bit = 7; bit >= 0; --bit)
    {
        const u8 mask = static_cast<u8>(1u << bit);
        if ((pendingB & mask) != 0)
        {
            reg_[kIprb] = static_cast<u8>(reg_[kIprb] & ~mask);
            if (isSoftwareEoi)
            {
                reg_[kIsrb] |= mask;
            }
            const u32 number = static_cast<u32>(bit);
            return (static_cast<u32>(reg_[kVr] & 0xF0u)) | number;
        }
    }

    return 0u;
}

}  // namespace x68k
