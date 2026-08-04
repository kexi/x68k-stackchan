// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "machine.h"

#include <cstring>

namespace x68k
{
namespace
{

// SASI のコマンド。IPL-ROM がブートセクタを読むのに使う範囲だけ実装する。
constexpr u8 kSasiTestUnitReady = 0x00;
constexpr u8 kSasiRezeroUnit = 0x01;
constexpr u8 kSasiRequestSense = 0x03;
constexpr u8 kSasiRead = 0x08;
constexpr u8 kSasiWrite = 0x0A;
constexpr u8 kSasiSeek = 0x0B;
// $C2 は X68000 固有。IPL-ROM が最初に発行し ($FF99AC のテンプレート)、
// ドライブのパラメータを設定する。コマンド 6 バイトの後に 10 バイトの
// パラメータが続く ($FF990E で D3=9 の DBRA)。中身は使わないが、
// 受け取り切らないと IPL-ROM が次へ進まない。
constexpr u8 kSasiSpecify = 0xC2;
constexpr u32 kSasiSpecifyParamBytes = 10;

// SASI のセクタ長。X68000 の SASI HDD は 256 バイト/セクタ。
constexpr u32 kSasiSectorSize = 256;

// SASI のフェーズ。
//
// $E96003 の下位 5bit が実機のフェーズを表す。IPL-ROM は AND.B #$1F の後に
// この値と比較して待つ ($FF97BA / $FF9842 / $FF991C)。
// IPL-ROM が待つ値は 2 つだけ。ビットの意味を推測して組み立てるより、
// 実際に比較されている値をそのまま名前にする方が間違えない。
//   $0B コマンド送出フェーズ ($FF9842 / $FF9890 / $FF98BC)
//       — CPU からターゲットへ送る側。コマンド 6 バイトとパラメータ。
//   $07 データインフェーズ ($FF97BE)
//       — ターゲットから CPU へ返す側。READ したセクタはここで渡す。
//   $03 $C2 のパラメータ送出待ち ($FF991C)
//   $0F ステータスフェーズ       ($FF970A で D6=$0F、$FF97E8 で一致を待つ)
//       — 終了ステータス 1 バイトを $E96001 から読む
//   $1F メッセージフェーズ       ($FF971E で D6=$1F)
//       — メッセージ 1 バイトを読んでバスを解放する
//
// Why not $07 を使うか: $FF97BA に $07 を待つ経路があるが、実際に呼ばれるのは
// $FF981E 側で、そちらは $0B を待つ。$07 を返すとコマンドを 1 バイトも
// 受け取れないまま止まる。
// セレクション待ちだけはビット単位で、$E96003 の bit1 が **0** になるのを
// 待つ ($FF96DA の BTST #1 → BEQ)。コマンドフェーズの $07 は bit1 が 1 なので、
// セレクションを受け付けた直後にいきなり $07 を返すと待ちを抜けられない。
// 「セレクション成立」を表す中間状態を挟む。
constexpr u8 kSasiStatusCommand = 0x0B;
constexpr u8 kSasiStatusDataOut = 0x0B;
constexpr u8 kSasiStatusDataIn = 0x07;
constexpr u8 kSasiStatusSpecifyParam = 0x03;
constexpr u8 kSasiStatusStatus = 0x0F;
constexpr u8 kSasiStatusMessage = 0x1F;
constexpr u8 kSasiStatusBusFree = 0x00;

constexpr u8 kPhaseBusFree = 0;
constexpr u8 kPhaseSelected = 6;  // セレクション成立。まだコマンドを受けない
// $C2 のパラメータ待ち。通常のデータアウト ($0B) と違い、IPL-ROM は
// ステータスフェーズと同じ $03 を待ってから送ってくる ($FF9910)。
constexpr u8 kPhaseSpecifyParam = 7;
constexpr u8 kPhaseCommand = 1;
constexpr u8 kPhaseDataIn = 2;
constexpr u8 kPhaseDataOut = 3;
constexpr u8 kPhaseStatus = 4;
constexpr u8 kPhaseMessage = 5;

// MFP の割り込みレベル。X68000 では MFP がレベル 6 に繋がっている。
constexpr u32 kMfpInterruptLevel = 6;

// SCC の割り込みレベル。X68000 では SCC (マウス / RS-232C) がレベル 5。
//
// MFP (6) より低いので、キーボードやタイマの処理中はマウスが待たされる。
// これは実機と同じ順序。ここを 6 以上にすると、マウスを動かし続けている間
// システムタイマが取りこぼされて時計が遅れる。
constexpr u32 kSccInterruptLevel = 5;

}  // namespace

Machine::Machine() : bus_(MemoryMap{}, sram_, *this), cpu_(bus_)
{
    // SASI と FDC のデータ転送はどちらも DMAC 経由で行われる。DMAC からは
    // データの出どころがデバイス、転送先がバスに見える。
    //
    // チャネルの割り当ては実機のとおり。FDC がチャネル 0 (IPL-ROM の
    // $FF8F3C が $E84005/$E8400A/$E8400C/$E84007 を叩く)、SASI が
    // チャネル 1 ($FF9944 が $E84045… を叩く)。
    dmac_.setDevice(Dmac::kSasiChannel, this);
    dmac_.setDevice(Dmac::kFdcChannel, &fdcDmaPort_);
    dmac_.setMemory(this);

    // G-VRAM の窓 ($C00000-$DFFFFF) はページ選択を兼ねており、CPU の書き込みを
    // 共有ワードのどのニブル/バイトへ折り込むかが色数モードで決まる。
    // バスがモードを引けるようにここで繋ぐ。
    bus_.setVideoController(&video_);

    // メインメモリへのアクセスを仮想関数抜きで通せるようにする。
    // 以後、実体・ROM 写像・ウォッチが変わるたびにバスが CPU へ教え直す。
    bus_.attachFastPathCpu(&cpu_);

    // X68000 では RESET 命令で $000000 の ROM 写像が解除される。
    // 68000 自身は RESET 信号を出すだけなので、機種固有のこの反応は
    // Machine が受け取って処理する。
    cpu_.setResetCallback(
        [](void* context)
        {
            auto* self = static_cast<Machine*>(context);
            self->bus_.setRomMappedAtZero(false);
        },
        this);
}

void Machine::setMemory(const MemoryMap& memory)
{
    bus_.setMemory(memory);
}

void Machine::reset()
{
    // SRAM はリセットで消さない。実機はバッテリバックアップなので、
    // リセットしても電源を切っても内容が残る。
    //
    // Why not 無条件に formatDefaults を呼ぶか: 以前はそうしていたが、
    // Human68k が起動デバイスや画面モードを書き換えた直後にリセットすると
    // 工場出荷値へ戻ってしまう。実機と挙動が違ううえ、SD へ保存しても
    // 次の起動で必ず上書きされるので保存する意味が無くなる。
    //
    // ただしマジックが壊れているときだけは初期化する。IPL-ROM は不正な
    // SRAM を見つけると自分で書き戻しにかかるが、その経路を通す前に
    // 途中の読み出しでゴミの設定を使ってしまう。ここで先回りしておく。
    if (!sram_.hasValidMagic())
    {
        sram_.formatDefaults();
    }
    crtc_.reset();
    video_.reset();
    mfp_.reset();
    rtc_.reset();
    fdc_.reset();
    scc_.reset();
    iosc_.reset();
    sprite_.reset();
    // 転送バッファは外から与えられた設定なので、リセットで消さない。
    // ここを丸ごと初期化すると nullptr に戻り、SASI が 1 バイトも
    // 受け取れなくなる。
    u8* const sasiBuffer = sasi_.buffer;
    sasi_ = SasiState{};
    sasi_.buffer = sasiBuffer;
    dmac_.reset();

    // リセット直後は IPL-ROM が $000000 に写像されている。
    // これがないとリセットベクタが読めない。
    bus_.setRomMappedAtZero(true);
    bus_.clearTextDirty();

    cpu_.reset();
}

u32 Machine::step()
{
    serviceInterrupts();

    const u32 cycles = cpu_.step();
    if (cycles == 0)
    {
        return 0;
    }

    mfp_.tick(cycles);
    rtc_.tick(cycles);
    if (crtc_.tick(cycles))
    {
        mfp_.setVerticalBlank(crtc_.inVerticalBlank());
    }

    return cycles;
}

u32 Machine::run(u32 cycles)
{
    u32 spent = 0;
    while (spent < cycles)
    {
        const u32 used = step();
        if (used == 0)
        {
            break;  // 停止した
        }
        spent += used;
    }
    return spent;
}

void Machine::serviceInterrupts()
{
    // MFP (レベル 6) を先に見る。SCC (レベル 5) より優先度が高いので、
    // 両方保留していたら MFP が勝つ。
    //
    // Why not SCC を先に見るか: 68000 は 1 回の割り込み受理で 1 つしか
    // 処理しない。低い方を先に渡すと、高い方が待たされるどころか
    // 「マウスを動かし続けている間キー入力が通らない」形で逆転する。
    // I/O コントローラ (レベル 1) は最下位なので最後に見る。MFP/SCC が
    // 保留している間は FDC の完了通知が待たされる。これは実機と同じ順序。
    if (!serviceMfpInterrupt() && !serviceSccInterrupt())
    {
        serviceIoScInterrupt();
    }
}

// FDC の割り込み線を I/O コントローラの bit0 へ反映する。
//
// Why not FDC から直接 IoSc を叩かないか: Fdc が IoSc を知ると、FDC 単体の
// テストに割り込みコントローラを連れてくる必要が出る。実機でも「線が
// 繋がっている」だけで FDC はコントローラの存在を知らないので、
// 配線を持つのは両者を組み立てる Machine の責務にしてある。
void Machine::updateFdcInterruptLine()
{
    iosc_.setFdcLine(fdc_.hasInterrupt());
}

bool Machine::serviceIoScInterrupt()
{
    // FDC の保留状態は DMA 完了などバスアクセス以外の契機でも変わるので、
    // 判定の直前に線を取り直す。
    updateFdcInterruptLine();

    if (!iosc_.hasPendingInterrupt())
    {
        return false;
    }

    // MFP / SCC と同じく、CPU が受け付けられるか先に確かめる。
    const u32 mask = cpu_.state().interruptMask();
    const bool isMasked = IoSc::kInterruptLevel <= mask;
    if (isMasked)
    {
        return false;
    }

    // I/O コントローラも自分のベクタ番号を返すデバイス。$E9C003 に書かれた
    // ベース ($60) にソース番号を足した値になる。オートベクタ (24+1=25) に
    // すると ROM が $180 へ張った FDC ハンドラ ($FF1130) へ届かない。
    const u8 vectorNumber = iosc_.acknowledgeInterrupt();
    if (vectorNumber == 0)
    {
        return false;
    }
    cpu_.requestInterrupt(IoSc::kInterruptLevel, vectorNumber);
    return true;
}

bool Machine::serviceMfpInterrupt()
{
    if (!mfp_.hasPendingInterrupt())
    {
        return false;
    }

    // CPU が今この割り込みを受け付けられるか先に確かめる。
    //
    // acknowledgeInterrupt() は MFP の IPR/ISR を書き換える破壊的な操作
    // なので、CPU がマスクしている間に呼ぶと割り込みが握りつぶされる。
    // 実機のバスは IACK サイクルが走って初めてこの遷移が起きるので、
    // 受理できないときは触らないのが正しい。
    //
    // ここを見落とすと、割り込みが上がり続けるのに一度も処理されず、
    // 原因の分かりにくい暴走になる。
    const u32 mask = cpu_.state().interruptMask();
    const bool isMasked = kMfpInterruptLevel <= mask;
    if (isMasked)
    {
        return false;
    }

    // MFP は自分のベクタ番号を返すデバイス (自動ベクタではない)。
    // VR レジスタの上位 4bit と割り込み番号を組み合わせた値になる。
    //
    // ここを自動ベクタ (24+6=30) にすると、IOCS が未初期化ベクタ用に
    // 埋めている「上位バイト = ベクタ番号」の値を PC に読み込んでしまい、
    // 不正ベクタのハンドラへ飛んで「エラーが発生しました」で止まる。
    const u32 vectorNumber = mfp_.acknowledgeInterrupt();
    if (vectorNumber == 0)
    {
        return false;
    }
    cpu_.requestInterrupt(kMfpInterruptLevel, vectorNumber);
    return true;
}

bool Machine::serviceSccInterrupt()
{
    if (!scc_.hasPendingInterrupt())
    {
        return false;
    }

    // MFP と同じ理由で、受理できるか先に確かめてから acknowledge する。
    //
    // acknowledgeInterrupt() は保留を落とす破壊的な操作なので、CPU が
    // マスクしている間に呼ぶとマウスのレポートが握りつぶされる。実機の
    // バスは IACK サイクルが走って初めてこの遷移が起きる。
    const u32 mask = cpu_.state().interruptMask();
    const bool isMasked = kSccInterruptLevel <= mask;
    if (isMasked)
    {
        return false;
    }

    // SCC も自分のベクタ番号を返すデバイス (WR2 に書かれた値が基になる)。
    // 自動ベクタにすると IOCS が張ったマウス用ハンドラへ届かない。
    const u32 vectorNumber = scc_.acknowledgeInterrupt();
    if (vectorNumber == 0)
    {
        return false;
    }
    cpu_.requestInterrupt(kSccInterruptLevel, vectorNumber);
    return true;
}

void Machine::pressKey(u8 scanCode)
{
    mfp_.receiveKeyboardByte(scanCode);
}

bool Machine::moveMouse(int dx, int dy, bool leftButton, bool rightButton)
{
    return scc_.moveMouse(dx, dy, leftButton, rightButton);
}

// --- 音声 --------------------------------------------------------------------
//
// FM (OPM) と ADPCM を足してモノラルで返す。実機は両者を独立した経路で
// アナログ的に混ぜるが、ここでは合成後に加算する。
void Machine::renderAudio(std::int16_t* out, std::size_t frames)
{
    if (out == nullptr)
    {
        return;
    }

    // 両方とも鳴っていなければ、合成そのものを省いてゼロで埋める。
    //
    // これが実機で音源を常時 ON にできるかどうかを分ける。X68000 を
    // 触っている時間の大半 (起動中、コマンド入力待ち) は音が鳴っておらず、
    // そこで 8ch x 4op を回すのは丸ごと無駄になる。Opm::renderSamples は
    // 同じ早期リターンを持つが、ここは 1 サンプルずつ混ぜる都合で
    // renderOneSample を呼ぶため、その恩恵を受けられない。
    //
    // Why not isSilent の判定を毎サンプル行わないか: エンベロープは
    // 1 ブロックの途中で切れうるが、鳴り終わった残りをゼロで埋めるか
    // 減衰しきった値で埋めるかの差しかない。ブロックの頭で 1 回だけ見る。
    const bool isQuiet = opm_.isSilent() && !adpcm_.isPlaying();
    if (isQuiet)
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            out[i] = 0;
        }
        return;
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        const std::int32_t fm = opm_.renderOneSample();
        const std::int32_t pcm = adpcm_.renderOneSample();

        // Why not それぞれ 1/2 にしてから足すか: 実機でも FM と ADPCM が
        // 同時に最大振幅になることはまずない。常時半分にすると、片方しか
        // 鳴っていない大半の時間で音量を 6dB 損する。飽和で受ける。
        std::int32_t mix = fm + pcm;
        constexpr std::int32_t kMin = -32768;
        constexpr std::int32_t kMax = 32767;
        if (mix < kMin)
        {
            mix = kMin;
        }
        if (mix > kMax)
        {
            mix = kMax;
        }
        out[i] = static_cast<std::int16_t>(mix);
    }
}

// --- I/O ディスパッチ --------------------------------------------------------

namespace
{

// スプライト VRAM ($EB8000-$EBFFFF) に当たるか。
//
// Why not ioRead8 の switch (base = addr & $FFE000) に混ぜないか:
// スプライト VRAM は 32KB あり、$FFE000 でマスクすると $EB8000 / $EBA000 /
// $EBC000 / $EBE000 の 4 つの case に散る。$EBC000 と $EBE000 は BG の
// ネームテーブルで、PCG と連続した 1 つの実体として扱う必要がある
// (dev/sprite.h の冒頭を参照)。範囲判定 1 つにまとめたほうが、
// 実体が連続しているという性質がコードにそのまま出る。
inline bool isSpriteVram(u32 addr)
{
    return addr >= kSpriteVramBase && addr < kSpriteVramEnd;
}

}  // namespace

u8 Machine::ioRead8(u32 addr)
{
    const u32 base = addr & 0xFFE000u;

    // スプライト VRAM は 4 つの base にまたがるので switch より先に見る。
    if (isSpriteVram(addr))
    {
        return sprite_.vramRead8(addr - kSpriteVramBase);
    }

    switch (base)
    {
        case kSpriteRegBase:
        {
            // レジスタはワード単位。バイトアクセスは上下を切り出す。
            // ワード境界へ丸めてから読み、奇数アドレスなら下位バイトを返す。
            const u16 value = sprite_.read((addr - kSpriteRegBase) & ~1u);
            return static_cast<u8>((addr & 1) != 0 ? (value & 0xFFu) : (value >> 8));
        }

        case kCrtcBase:
            // CRTC はワード単位。バイトアクセスは上下を切り出す。
            {
                const u32 reg = (addr - kCrtcBase) / 2;
                const u16 value = crtc_.read(reg);
                return static_cast<u8>((addr & 1) != 0 ? (value & 0xFFu) : (value >> 8));
            }

        case kVideoCtrlBase:
        {
            const u16 value = video_.read(addr - kVideoCtrlBase);
            return static_cast<u8>((addr & 1) != 0 ? (value & 0xFFu) : (value >> 8));
        }

        case kMfpBase:
        {
            // MFP のレジスタは奇数アドレスにのみ現れる。
            //
            // 偶数側は実体が無いので 0 を返す。以前は / 2 で偶数も同じ
            // レジスタへ割り当てていたが、UDR の読み出しに「受信バッファフルを
            // 落とす」副作用を足したため、UDR ではない偶数アドレスを読んだ
            // だけでフラグが落ちるようになってしまった。
            const bool isOddAddress = (addr & 1) != 0;
            if (!isOddAddress)
            {
                return 0u;
            }
            return mfp_.read((addr - kMfpBase) / 2);
        }

        case kSasiBase:
            return sasiRead(addr);

        case kAreaSetBase:
            // エリアセットは書き込み専用。読んでも意味のある値は返さない。
            //
            // 重要: IPL-ROM 1.3 以降は CLR.B でここへ書き込む。68000 の CLR は
            // read-modify-write なので必ず読み出しが先に起きる。ここで副作用を
            // 持たせると起動しない。
            return 0u;

        case kRtcBase:
            // RTC (RP5C15)。レジスタは 4bit 幅で 2 バイトおきに並ぶ。
            // Human68k は起動時に日付を読むので、妥当な値を返す必要がある。
            return rtc_.read((addr - kRtcBase) / 2);

        case kSysPortBase:
            // システムポート。コントラストや CPU 種別。
            //
            // $E8E00B の bit3-0 が CPU 種別で、$DC が 68000 を表す
            // (上位ニブルは常に $D)。ここを間違えると IOCS が 68030 向けの
            // 初期化をしようとして失敗する。
            if ((addr & 0x0Fu) == 0x0B)
            {
                return 0xDCu;
            }
            return 0u;

        case kOpmBase:
            // YM2151。ステータスは $E90003 に現れる (奇数側の $E90001 は
            // レジスタ番号の書き込み専用)。
            //
            // IPL-ROM の待ちループ ($FF9C9C: TST.B $E90003 / BMI.S) は
            // bit7 (BUSY) が落ちるまで回り、タイムアウトを持たない。
            // Opm::readStatus は常に bit7 = 0 を返す。
            if ((addr & 0x0Fu) == 0x03)
            {
                return opm_.readStatus();
            }
            return 0u;

        case kAdpcmBase:
            // MSM6258V。$E92001 がステータス、$E92003 がデータ。
            // データ側は書き込み専用なので読んでも 0。
            if ((addr & 0x0Fu) == 0x01)
            {
                return adpcm_.readStatus();
            }
            return 0u;

        case kFdcBase:
            // FDC (uPD72065)。イメージを繋げば実際に読み書きする。
            //   $E94001 メインステータス
            //   $E94003 データ (コマンド送出と結果の受け取り)
            // セクタの中身は DMAC のチャネル 0 経由で流れるので、
            // ここ ($E94003) を通るのはコマンドと結果バイトだけ。
            if ((addr & 0x0Fu) == 0x01)
            {
                return fdc_.readStatus();
            }
            if ((addr & 0x0Fu) == 0x03)
            {
                const u8 data = fdc_.readData();
                // 結果バイトを読み切ると FDC 側の保留が畳まれることがある。
                // 読んだ直後に線を取り直さないと、要因が消えているのに
                // 線が上がったままになりハンドラが再入する。
                updateFdcInterruptLine();
                return data;
            }
            return 0u;

        case kDmacBase:
            return dmac_.read(addr - kDmacBase);

        case kSccBase:
            return sccRead(addr);

        case kIoScBase:
            return iosc_.read(addr - kIoScBase);

        case kPpiBase:
        case kPrinterBase:
            // スタブ。読み出しは 0。
            return 0u;

        default:
            return 0u;
    }
}

void Machine::ioWrite8(u32 addr, u8 value)
{
    const u32 base = addr & 0xFFE000u;

    if (isSpriteVram(addr))
    {
        sprite_.vramWrite8(addr - kSpriteVramBase, value);
        return;
    }

    switch (base)
    {
        case kSpriteRegBase:
        {
            // ワード単位のレジスタへのバイト書き込み。読んで片側だけ差し替える。
            const u32 offset = (addr - kSpriteRegBase) & ~1u;
            const u16 old = sprite_.read(offset);
            const u16 next = (addr & 1) != 0 ? static_cast<u16>((old & 0xFF00u) | value)
                                             : static_cast<u16>((old & 0x00FFu) | (value << 8));
            sprite_.write(offset, next);
            return;
        }

        case kCrtcBase:
        {
            const u32 reg = (addr - kCrtcBase) / 2;
            const u16 old = crtc_.read(reg);
            const u16 next = (addr & 1) != 0 ? static_cast<u16>((old & 0xFF00u) | value)
                                             : static_cast<u16>((old & 0x00FFu) | (value << 8));
            crtc_.write(reg, next);
            return;
        }

        case kVideoCtrlBase:
        {
            const u32 offset = addr - kVideoCtrlBase;
            const u16 old = video_.read(offset);
            const u16 next = (addr & 1) != 0 ? static_cast<u16>((old & 0xFF00u) | value)
                                             : static_cast<u16>((old & 0x00FFu) | (value << 8));
            video_.write(offset, next);
            return;
        }

        case kMfpBase:
        {
            // 読み出しと同じく、レジスタは奇数アドレスにのみ現れる。
            // 偶数側への書き込みは捨てる。
            const bool isOddAddress = (addr & 1) != 0;
            if (isOddAddress)
            {
                mfp_.write((addr - kMfpBase) / 2, value);
            }
            return;
        }

        case kRtcBase:
            rtc_.write((addr - kRtcBase) / 2, value);
            return;

        case kSasiBase:
            sasiWrite(addr, value);
            return;

        case kDmacBase:
            dmac_.write(addr - kDmacBase, value);
            return;

        case kSccBase:
            sccWrite(addr, value);
            return;

        case kIoScBase:
            iosc_.write(addr - kIoScBase, value);
            return;

        case kFdcBase:
            if ((addr & 0x0Fu) == 0x03)
            {
                fdc_.writeData(value);
                // コマンドが揃った時点で割り込みが上がることがある
                // (SEEK / RECALIBRATE)。逆に SENSE INTERRUPT STATUS は
                // 落とす。どちらも writeData の中で起きるので、
                // 書いた直後に線を取り直す。
                updateFdcInterruptLine();
            }
            else if ((addr & 0x0Fu) == 0x05)
            {
                // ドライブ制御 (選択とモーター)。
                fdc_.writeDriveControl(value);
            }
            else if ((addr & 0x0Fu) == 0x07)
            {
                // ドライブ選択とモーター。IPL-ROM の $FF909E が
                // 「$80 | ドライブ番号」をここへ書いてからコマンドを送る。
                fdc_.writeDriveSelect(value);
            }
            return;

        case kOpmBase:
            // YM2151。$E90001 にレジスタ番号、$E90003 に値。
            // IPL-ROM の書き込み手順 ($FF9C8A -> $FF9C94) がこの順に叩く。
            if ((addr & 0x0Fu) == 0x01)
            {
                opm_.writeAddress(value);
            }
            else if ((addr & 0x0Fu) == 0x03)
            {
                opm_.writeData(value);
            }
            return;

        case kAdpcmBase:
            // MSM6258V。$E92001 がコマンド、$E92003 がデータ。
            // IPL-ROM は $E92001 に #$04 (停止) / #$02 (再生) を書く
            // ($FF9A68 / $FF9A8C)。
            if ((addr & 0x0Fu) == 0x01)
            {
                adpcm_.writeCommand(value);
            }
            else if ((addr & 0x0Fu) == 0x03)
            {
                adpcm_.writeData(value);
            }
            return;

        case kAreaSetBase:
            // エリアセットへの書き込みで ROM の $000000 写像が解除される。
            // これで通常のメモリ配置になり、以降 $000000 は RAM を指す。
            bus_.setRomMappedAtZero(false);
            return;

        default:
            // その他のデバイスへの書き込みは捨てる。
            // IPL-ROM と IOCS は存在しないデバイスも初期化しに来るので、
            // ここでエラーにすると起動が進まない。
            return;
    }
}

u16 Machine::ioRead16(u32 addr)
{
    const u32 base = addr & 0xFFE000u;

    if (base == kCrtcBase)
    {
        return crtc_.read((addr - kCrtcBase) / 2);
    }
    if (base == kVideoCtrlBase)
    {
        return video_.read(addr - kVideoCtrlBase);
    }
    // スプライトレジスタはワードが最小単位。read8 を 2 回に分けると、
    // 丸めたオフセットから同じワードを 2 度切り出すことになる。
    if (base == kSpriteRegBase)
    {
        return sprite_.read(addr - kSpriteRegBase);
    }

    return static_cast<u16>((ioRead8(addr) << 8) | ioRead8(addr + 1));
}

void Machine::ioWrite16(u32 addr, u16 value)
{
    const u32 base = addr & 0xFFE000u;

    if (base == kCrtcBase)
    {
        crtc_.write((addr - kCrtcBase) / 2, value);
        return;
    }
    if (base == kVideoCtrlBase)
    {
        video_.write(addr - kVideoCtrlBase, value);
        return;
    }
    // ワードでまとめて書く。バイト 2 回に分けると、上位バイトだけ書いた
    // 途中の値でプライオリティの数え直しが走る。
    if (base == kSpriteRegBase)
    {
        sprite_.write(addr - kSpriteRegBase, value);
        return;
    }

    ioWrite8(addr, static_cast<u8>(value >> 8));
    ioWrite8(addr + 1, static_cast<u8>(value & 0xFFu));
}

// --- DMA ---------------------------------------------------------------------
//
// DMAC は「デバイスから 1 バイト取ってメモリへ書く」を繰り返すだけなので、
// SASI 側はデータインフェーズのバッファを 1 バイトずつ差し出せばよい。

bool Machine::dmaRead(u8* value)
{
    if (sasi_.phase != kPhaseDataIn || sasi_.bufferPos >= sasi_.bufferLength)
    {
        return false;
    }
    *value = sasi_.buffer[sasi_.bufferPos++];
    if (sasi_.bufferPos >= sasi_.bufferLength)
    {
        sasi_.phase = kPhaseStatus;
    }
    return true;
}

// データを受け取り切った後の後始末。
//
// WRITE ならディスクへ書き、ステータスフェーズへ移る。$C2 のパラメータは捨てる。
//
// Why not 呼び出し元それぞれに書くか: DMA 経由 (dmaWrite) と CPU が 1 バイトずつ
// 書く経路 (ioWrite8) の 2 つがあり、同じ処理を二重に持っていた。片方だけ直した
// せいで、DMA 経由の複数セクタ書き込みが 1 セクタしか書かないまま残っていた。
void Machine::finishSasiWrite()
{
    if (sasi_.command[0] == kSasiWrite)
    {
        const u32 lba = (static_cast<u32>(sasi_.command[1] & 0x1Fu) << 16) |
                        (static_cast<u32>(sasi_.command[2]) << 8) |
                        static_cast<u32>(sasi_.command[3]);
        // 受け取った分だけ書く。bufferLength はコマンドのセクタ数から決めてある。
        const u32 sectors = sasi_.bufferLength / kSasiSectorSize;

        // 書けなかったらエラーを返す。戻り値を捨てると、読み取り専用の
        // イメージや I/O エラーでも成功に見え、Human68k は書けたつもりで
        // 先へ進んでしまう。
        const bool ok = disk_ != nullptr && disk_->isPresent() &&
                        disk_->writeSector(lba, sasi_.buffer, sectors);
        if (!ok)
        {
            sasi_.status = 0x02;
        }
    }
    sasi_.phase = kPhaseStatus;
}

bool Machine::dmaWrite(u8 value)
{
    if (sasi_.phase != kPhaseDataOut || sasi_.bufferPos >= sasi_.bufferLength)
    {
        return false;
    }
    sasi_.buffer[sasi_.bufferPos++] = value;
    if (sasi_.bufferPos >= sasi_.bufferLength)
    {
        finishSasiWrite();
    }
    return true;
}

u8 Machine::dmaMemRead(u32 addr)
{
    return bus_.read8(addr);
}

void Machine::dmaMemWrite(u32 addr, u8 value)
{
    bus_.write8(addr, value);
}

// --- SCC ---------------------------------------------------------------------
//
// Z8530 は 8bit デバイスで、16bit バスの下位バイト側に繋がっている。
// レジスタは奇数アドレスにのみ現れる (MFP と同じ理由)。
//
//   $E98001 ch B 制御 / $E98003 ch B データ   ← マウス
//   $E98005 ch A 制御 / $E98007 ch A データ   ← RS-232C
//
// IPL-ROM は MOVE.W で $E98000 のような偶数アドレスへ書くが、実際に
// デバイスへ届くのは下位バイト = 奇数アドレス側。
// 例: MOVE.W #$0062,$E98000 は $E98000 に $00、$E98001 に $62 を書く。
// この $62 が WR5 への値になる。
//
// Why not 偶数アドレスも同じレジスタへ割り当てるか: MFP で同じことをして
// 壊れた。Z8530 は制御ポートを「読むとレジスタポインタが 0 に戻る」ので、
// 偶数側の読みでもポインタが戻ると、MOVE.W で読んだときに上位バイトの
// アクセスがポインタを潰し、下位バイトが必ず RR0 を読むことになる。

u8 Machine::sccRead(u32 addr)
{
    // 偶数側は実体が無い。0 を返す。
    const bool isOddAddress = (addr & 1) != 0;
    if (!isOddAddress)
    {
        return 0u;
    }

    // $E98001/$E98003 が ch B、$E98005/$E98007 が ch A。
    // bit2 でチャネル、bit1 で制御/データを選ぶ。
    const u32 offset = addr & 0x07u;
    const u32 channel = (offset & 0x04u) != 0 ? Scc::kChannelA : Scc::kChannelB;
    const bool isDataPort = (offset & 0x02u) != 0;

    return isDataPort ? scc_.readData(channel) : scc_.readControl(channel);
}

void Machine::sccWrite(u32 addr, u8 value)
{
    const bool isOddAddress = (addr & 1) != 0;
    if (!isOddAddress)
    {
        return;
    }

    const u32 offset = addr & 0x07u;
    const u32 channel = (offset & 0x04u) != 0 ? Scc::kChannelA : Scc::kChannelB;
    const bool isDataPort = (offset & 0x02u) != 0;

    if (isDataPort)
    {
        scc_.writeData(channel, value);
        return;
    }
    scc_.writeControl(channel, value);
}

// --- SASI --------------------------------------------------------------------
//
// X68000 の SASI インタフェースは $E96000 から数バイトのレジスタを持つ。
//   $E96001: データレジスタ (コマンドの送出とデータの授受)
//   $E96003: ステータスレジスタ (ビジー/リクエスト等)
// IPL-ROM はここへ 6 バイトのコマンドを送り、ブートセクタを読み出す。

u8 Machine::sasiRead(u32 addr)
{
    const u32 reg = addr & 0x0Fu;

    if (reg == 0x01)
    {
        // データレジスタ。ターゲットから CPU へ渡す側。
        if (sasi_.phase == kPhaseDataIn && sasi_.bufferPos < sasi_.bufferLength)
        {
            const u8 value = sasi_.buffer[sasi_.bufferPos++];
            if (sasi_.bufferPos >= sasi_.bufferLength)
            {
                sasi_.phase = kPhaseStatus;
            }
            return value;
        }
        if (sasi_.phase == kPhaseStatus)
        {
            // 終了ステータスの次はメッセージ。IPL-ROM は 2 バイト読む。
            sasi_.phase = kPhaseMessage;
            return sasi_.status;
        }
        if (sasi_.phase == kPhaseMessage)
        {
            sasi_.phase = kPhaseBusFree;
            return 0u;
        }
        return 0u;
    }

    if (reg == 0x03)
    {
        // ステータスレジスタ。IPL-ROM は下位 5bit をフェーズとして読む。
        switch (sasi_.phase)
        {
            case kPhaseSelected:
                // セレクションが成立した直後。IPL-ROM は bit1 が 0 になるのを
                // 待っているので、ここでは 0 を返してから次の読みでコマンド
                // フェーズへ移る。
                sasi_.phase = kPhaseCommand;
                return kSasiStatusBusFree;

            case kPhaseCommand:
                return kSasiStatusCommand;
            case kPhaseDataIn:
                return kSasiStatusDataIn;

            case kPhaseDataOut:
                return kSasiStatusDataOut;
            case kPhaseStatus:
                return kSasiStatusStatus;

            case kPhaseMessage:
                return kSasiStatusMessage;

            case kPhaseSpecifyParam:
                return kSasiStatusSpecifyParam;
            default:
                return kSasiStatusBusFree;
        }
    }

    return 0u;
}

void Machine::sasiWrite(u32 addr, u8 value)
{
    const u32 reg = addr & 0x0Fu;

    if (reg == 0x07)
    {
        // セレクション。IPL-ROM はここへターゲット ID を書き ($FF96CE)、
        // $E96003 の bit1 (BSY) が 0 になるのを待つ ($FF96DA)。
        //
        // Why not $E96001 でセレクションとするか: 実機の IPL-ROM は
        // $E96007 を使う。$E96001 はデータの授受専用で、セレクション前に
        // 書かれることはない。
        //
        // ディスクが無ければ BSY を立てたまま (バスフリーにしない) にして
        // タイムアウトさせる。
        if (disk_ != nullptr && disk_->isPresent())
        {
            sasi_.phase = kPhaseSelected;
            sasi_.commandLength = 0;
        }
        return;
    }

    if (reg == 0x01)
    {
        // データレジスタへの書き込み。
        if (sasi_.phase == kPhaseCommand)
        {
            if (sasi_.commandLength < sizeof(sasi_.command))
            {
                sasi_.command[sasi_.commandLength++] = value;
            }
            if (sasi_.commandLength < 6)
            {
                return;
            }

            // 6 バイト揃ったのでコマンドを実行する。
            const u8 opcode = sasi_.command[0];
            // LBA は command[1] の下位 5bit と command[2], command[3]。
            const u32 lba = (static_cast<u32>(sasi_.command[1] & 0x1Fu) << 16) |
                            (static_cast<u32>(sasi_.command[2]) << 8) |
                            static_cast<u32>(sasi_.command[3]);
            const u32 count = sasi_.command[4];

            sasi_.status = 0;
            sasi_.bufferPos = 0;
            sasi_.bufferLength = 0;

            switch (opcode)
            {
                case kSasiSpecify:
                    // パラメータ 10 バイトを受け取ってから終了ステータスを返す。
                    sasi_.bufferPos = 0;
                    sasi_.bufferLength = kSasiSpecifyParamBytes;
                    sasi_.phase = kPhaseSpecifyParam;
                    return;

                case kSasiTestUnitReady:
                case kSasiRezeroUnit:
                case kSasiSeek:
                    // ディスクが無ければエラーを返す。
                    sasi_.status = (disk_ != nullptr && disk_->isPresent()) ? 0x00 : 0x02;
                    sasi_.phase = kPhaseStatus;
                    return;

                case kSasiRequestSense:
                    // センスデータ 4 バイト。エラーなしを返す。
                    std::memset(sasi_.buffer, 0, 4);
                    sasi_.bufferLength = 4;
                    sasi_.phase = kPhaseDataIn;
                    return;

                case kSasiRead:
                {
                    // count = 0 は 256 セクタの意味 (SASI の 6 バイトコマンド)。
                    // 1 と読むと 256 セクタの要求で最初の 1 つしか読まない。
                    const u32 sectors = count == 0 ? 256u : count;
                    // 要求がバッファに収まらないなら、黙って切り詰めずに
                    // エラーを返す。切り詰めると転送量と bufferLength が
                    // ずれ、DMA が途中で止まったまま「成功」に見えてしまう。
                    const bool fits = sectors <= kSasiMaxSectorsPerCommand;
                    const bool ok = fits && sasi_.buffer != nullptr && disk_ != nullptr &&
                                    disk_->isPresent() &&
                                    disk_->readSector(lba, sasi_.buffer, sectors);
                    if (!ok)
                    {
                        sasi_.status = 0x02;
                        sasi_.phase = kPhaseStatus;
                        return;
                    }
                    sasi_.bufferLength = kSasiSectorSize * sectors;
                    sasi_.phase = kPhaseDataIn;
                    return;
                }

                case kSasiWrite:
                {
                    // READ と同じ数え方をする。ここを 1 セクタ固定にすると、
                    // 複数セクタの書き込みで最初の 1 つしか書かれないまま
                    // 成功を返し、ファイルシステムが静かに壊れる。
                    const u32 sectors = count == 0 ? 256u : count;
                    const bool fits = sectors <= kSasiMaxSectorsPerCommand;
                    if (!fits || sasi_.buffer == nullptr)
                    {
                        sasi_.status = 0x02;
                        sasi_.phase = kPhaseStatus;
                        return;
                    }
                    sasi_.bufferLength = kSasiSectorSize * sectors;
                    sasi_.phase = kPhaseDataOut;
                    return;
                }

                default:
                    // 未対応コマンド。エラーを返す。
                    sasi_.status = 0x02;
                    sasi_.phase = kPhaseStatus;
                    return;
            }
        }

        if (sasi_.phase == kPhaseDataOut || sasi_.phase == kPhaseSpecifyParam)
        {
            if (sasi_.buffer != nullptr && sasi_.bufferPos < kSasiBufferBytes)
            {
                sasi_.buffer[sasi_.bufferPos++] = value;
            }
            if (sasi_.bufferPos >= sasi_.bufferLength)
            {
                // WRITE ならディスクへ書く。$C2 のパラメータは捨てる。
                finishSasiWrite();
            }
            return;
        }
        return;
    }

    if (reg == 0x05)
    {
        // 割り込み許可。
        sasi_.interruptEnabled = (value & 1u) != 0;
        return;
    }
}

}  // namespace x68k
