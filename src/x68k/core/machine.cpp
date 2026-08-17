// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "machine.h"

#include <algorithm>
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
    // 音源も他のデバイスと同じくリセットする。
    //
    // 実機はシステムリセット線が両チップへ届く。落とさないと、鳴っている
    // 音がリセット後も鳴り続け、ADPCM はリセット前に FIFO へ残っていた
    // バイトを復号し続ける。
    opm_.reset();
    adpcm_.reset();
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

    // 期限の生成元 (MFP / CRTC / RTC) を 3 つとも 0 に戻したので、
    // 溜まっていた時間と期限の両方を捨てる。
    //
    // Why not 溜まった時間を流してから捨てないか: 流す先のデバイスが
    // 既にリセットされている。リセット前の時間をリセット後のデバイスへ
    // 渡すと、reset 直後だけ位相がずれる。
    sched_.reset();
    shadowArmed_ = false;
    stopSkippedCycles_ = 0;

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

    // step() はトレース用で、毎命令の速度が問題になる経路ではない。
    // 最適化スイッチはここでも尊重する (ホストの --no-fast-tick が
    // step() 経路でも同じ状態になることを確かめられるように)。
    if (perf_.allEnabled())
    {
        tickDevices<true, true, true>(cycles);
    }
    else
    {
        tickDevices<false, false, false>(cycles);
    }

    return cycles;
}

u32 Machine::run(u32 cycles)
{
    // 最適化スイッチはここで **1 回だけ** 見る。
    //
    // Why not tickDevices へ bool を渡さないか: それだと毎命令フラグを
    // 読んで分岐することになり、測ろうとしている当のホットループに
    // 計測器のコストが乗る。ESP32-S3 のオブジェクトを見ると、フラグ 3 つの
    // ロードと分岐がそのまま残っていた (codex の指摘)。
    //
    // テンプレート引数にすれば、有効側は **スイッチを入れる前と同じ
    // 生成コード** になる。定数畳み込みで分岐ごと消えるので、本番の姿に
    // 計測用の細工が残らない。切り替えの代償はスライスごとの分岐で、
    // 20000 サイクルに 1 度なので無視できる。
    //
    // 3 つを個別に見るのは、perf_switch.h が「個別に切れる」と約束して
    // いるため。まとめて allEnabled() で判定すると、RTC だけ切ったつもりが
    // MFP と CRTC まで切れて、どれが効いたのか分からなくなる。
    //
    // 組み合わせは 8 通りだが、実体化されるのは実際に使うものだけで、
    // 有効側 (true,true,true) が本番の姿になる。
    //
    // イベント駆動もここで 1 回だけ見る。同じ理由で実行時の bool を
    // ループへ持ち込まない。有効側の runEventDriven は、スイッチが
    // 無かったときと同じ生成コードになる。
    // shadow 検証はホスト専用の別ループへ分ける。
    //
    // Why not runWith の中で見ないか: それだと本番のホットループに毎命令の
    // ロードと分岐が乗る。実際に置いた版を objdump で見たら、有効側
    // (true,true,true) の生成コードが変わっていた。測る道具が測る対象を
    // 変えてはいけない (perf_switch.h)。
    if (shadowVerify_)
    {
        if (perf_.allEnabled())
        {
            return runShadowVerify<true, true, true>(cycles);
        }
        return runShadowVerify<false, false, false>(cycles);
    }
    if (eventDriven_)
    {
        if (perf_.inlineMfpTimer)
        {
            if (perf_.inlineRtcTick)
            {
                return perf_.inlineCrtcTick ? runEventDriven<true, true, true>(cycles)
                                            : runEventDriven<true, true, false>(cycles);
            }
            return perf_.inlineCrtcTick ? runEventDriven<true, false, true>(cycles)
                                        : runEventDriven<true, false, false>(cycles);
        }
        if (perf_.inlineRtcTick)
        {
            return perf_.inlineCrtcTick ? runEventDriven<false, true, true>(cycles)
                                        : runEventDriven<false, true, false>(cycles);
        }
        return perf_.inlineCrtcTick ? runEventDriven<false, false, true>(cycles)
                                    : runEventDriven<false, false, false>(cycles);
    }
    if (perf_.inlineMfpTimer)
    {
        if (perf_.inlineRtcTick)
        {
            return perf_.inlineCrtcTick ? runWith<true, true, true>(cycles)
                                        : runWith<true, true, false>(cycles);
        }
        return perf_.inlineCrtcTick ? runWith<true, false, true>(cycles)
                                    : runWith<true, false, false>(cycles);
    }
    if (perf_.inlineRtcTick)
    {
        return perf_.inlineCrtcTick ? runWith<false, true, true>(cycles)
                                    : runWith<false, true, false>(cycles);
    }
    return perf_.inlineCrtcTick ? runWith<false, false, true>(cycles)
                                : runWith<false, false, false>(cycles);
}

template <bool FastMfp, bool FastRtc, bool FastCrtc>
u32 Machine::runWith(u32 cycles)
{
    u32 spent = 0;
    while (spent < cycles)
    {
        serviceInterrupts();

        const u32 used = cpu_.step();
        if (used == 0)
        {
            break;  // 停止した
        }
        spent += used;

        tickDevices<FastMfp, FastRtc, FastCrtc>(used);
    }

    return spent;
}

// 段 1 の shadow 検証。飛ばさずに、期限の予測だけを裏で突き合わせる。
//
// Why not イベント駆動側で検証しないか: 飛ばす側で検証すると、
// 「予測した期限に飛んだ先で状態が変わっていた」しか見えない。予測より
// **早く** 状態が変わっていた場合は飛び越した後なので検出できない。
// 毎命令 tick のまま裏で予測を回せば、1 サイクル単位で
// 「最初に変わった瞬間」が分かる。
//
// Why not runWith の中に if (shadowVerify_) を置かないか: それだと
// **本番のホットループに毎命令のロードと分岐が乗る**。実際に置いた版を
// objdump で見たところ、有効側 (true,true,true) の生成コードが変わって
// いた。計測器が測る対象を変えてはいけないという規律 (perf_switch.h) は
// 検証器にも同じくかかる。ホスト専用の別ループへ出す。
//
// このループは実機では一度も呼ばれない。ホストの --shadow-verify だけ。
template <bool FastMfp, bool FastRtc, bool FastCrtc>
u32 Machine::runShadowVerify(u32 cycles)
{
    u32 spent = 0;
    while (spent < cycles)
    {
        serviceInterrupts();

        const u32 used = cpu_.step();
        if (used == 0)
        {
            break;  // 停止した
        }
        spent += used;

        tickDevices<FastMfp, FastRtc, FastCrtc>(used);

        shadowElapsed_ += used;
        shadowObserve(used);
    }

    return spent;
}

// イベント駆動の run()。**毎命令の判定は debt_ とゼロの比較 1 本だけ**。
//
// 変数同士の比較 (spent < cycles) を毎命令に入れると、過去 2 回と同じ轍を
// 踏む (-18%, -6.5% の実測)。スライスの終端は sched_ が絶対サイクルで
// 別に持っていて、遅い側でしか見ない。
template <bool FastMfp, bool FastRtc, bool FastCrtc>
u32 Machine::runEventDriven(u32 cycles)
{
    sched_.beginSlice(cycles);
    rearmDeadline();

    // 入口で既に STOP なら、その場で飛ばす。
    //
    // Why これが要るか: 期限はスライス終端で切り詰められる。STOP のまま
    // スライスへ入ると、遅い側へ落ちるのはスライスが終わるときだけなので、
    // 飛び越しの出番が 1 度も来ない。20,000 サイクルのスライスなら
    // call8 M68k::step() を 5,000 回回してからスライスが終わる。
    // 実測でも、ここが無いと 400,000 サイクル中 31,494 しか飛ばせなかった。
    //
    // ホットパスには 1 命令も足していない。ループの外の 1 回だけ。
    if (cpu_.state().stopped && cpu_.pendingInterruptLevel() == 0)
    {
        skipStopped<FastMfp, FastRtc, FastCrtc>();
        if (sched_.reachedSliceEnd())
        {
            return sched_.sliceSpent();
        }
    }

    for (;;)
    {
        // 命令の実行だけを空回しにして、イベント駆動の状態のまま
        // 「命令実行が何 ns/cycle か」を測る。
        //
        // Why これが要るか: 過去の「命令実行 0.90 ns/cycle」は quantum を
        // 撤回する前、tickDevices が毎命令だった頃の測定で、デバイス処理が
        // 支配的すぎて命令実行が埋もれていた。イベント駆動で tickDevices が
        // 25,000 サイクルに 1 回まで減った今、その数字は使えない。
        //
        // **恒久的な機能ではない。** 状態は進まないのでゲストは動かない。
        const u32 used = nullExecInEvent_ ? 4u : cpu_.step();
        if (used == 0)
        {
            // halted。溜まった時間を捨てずに実体化してから抜ける。
            //
            // Why not そのまま break するか: 未実体化のサイクルが消えると、
            // halt した瞬間だけデバイスの時刻が巻き戻る。毎命令 tick 版と
            // 状態が食い違い、同値テストが落ちる。
            settle<FastMfp, FastRtc, FastCrtc>();
            break;
        }
        if (sched_.advance(used))
        {
            if (reachSlow<FastMfp, FastRtc, FastCrtc>())
            {
                break;
            }
        }
    }

    return sched_.sliceSpent();
}

template <bool FastMfp, bool FastRtc, bool FastCrtc>
void Machine::settle()
{
    // 毎命令の advance() は debt_ しか触らないので、ここで unsettled_ を
    // 引き直す。遅い側へ入るのはここだけなので、この 1 行で不変条件
    // (debt_ == unsettled_ - deadlineAt_) が閉じる。
    sched_.syncUnsettled();

    u32 remaining = sched_.pending();
    while (remaining != 0)
    {
        // CRTC へ 1 フレーム以上を一度に渡すと、その間の垂直帰線の開始と
        // 終了を丸ごと落とす (video.h の tickFast のコメント)。
        //
        // **現状ここは実際には 1 周しかしない。** 垂直帰線のエッジ 2 点が
        // 常に期限に入っているので (nextEventCycle)、飛ぶ量は必ず
        // 162,342 サイクル以下になる。STOP の一括飛ばしで割り込み源が
        // 1 つも無い場合ですら、CRTC のエッジが先に来て刻んでいることを
        // 実測で確認した (jump は 18,000 と 162,340 の繰り返しになる)。
        //
        // Why not それなら消さないか: 冗長なのは「CRTC が期限に入って
        // いる」という **今の設計** に依存した結論で、変更 4 (CRTC を
        // イベント化しない) を見直したら即座に反転する。そのとき壊れ方は
        // 「垂直帰線が丸ごと消える」で、症状から原因へ辿るのが難しい。
        // ループ 1 つぶんの保険としては安い。
        const u32 chunk = std::min(remaining, Scheduler::kMaxSettleChunk);
        tickDevices<FastMfp, FastRtc, FastCrtc>(chunk);
        sched_.consume(chunk);
        remaining -= chunk;
    }
}

template <bool FastMfp, bool FastRtc, bool FastCrtc>
bool Machine::reachSlow()
{
    sched_.countReach();
    settle<FastMfp, FastRtc, FastCrtc>();

    // 割り込みを配送する。配送できない保留が残ったら、期限を張らずに
    // 毎命令リトライへ縮退する。
    //
    // 配送失敗は状態を変えない (serviceMfpInterrupt はマスク中に
    // acknowledge せず false を返す) し、IoSc はレベル保持で
    // acknowledgeInterrupt が const なので受理しても status_ が下がらない。
    // つまり **新しいエッジは二度と来ない**。ここで縮退させることが、
    // 割り込みの正しさを wake の網羅性から切り離している。
    serviceInterrupts();
    const bool blocked =
        mfp_.hasPendingInterrupt() || scc_.hasPendingInterrupt() || iosc_.hasPendingInterrupt();

    if (sched_.reachedSliceEnd())
    {
        return true;
    }

    if (blocked)
    {
        sched_.holdForPendingInterrupt();
        return false;
    }

    // STOP 中は命令が 4 サイクルずつしか進まない。ここで飛ばさないと、
    // 利得が最大のはずの区間で call8 M68k::step() を期限のサイクル数 / 4 回
    // 回すことになり、純粋な劣化になる。
    //
    // Why not ホットパスへ st_.stopped の判定を足さないか: 毎命令の判定は
    // ゼロ比較 1 本という規律を崩す。ここは期限ごとにしか通らないので、
    // ロードと分岐を足しても効かない。
    //
    // 受理待ちの割り込みが既にあるときは飛ばさない。st_.stopped が下りるのは
    // 次の step() なので、ここで飛ばすと「もう起きることが決まっている」CPU を
    // 期限まで眠らせることになり、受理が期限ぶん (最大 80,000 サイクル)
    // 遅れる。毎命令 tick 版と PC が食い違うので同値テストで落ちる。
    const bool sleepsUntilDeadline = cpu_.state().stopped && cpu_.pendingInterruptLevel() == 0;
    if (sleepsUntilDeadline)
    {
        skipStopped<FastMfp, FastRtc, FastCrtc>();
        return sched_.reachedSliceEnd();
    }

    rearmDeadline();
    return false;
}

// STOP 中の一括飛び越し。割り込みが来るかスライスが終わるまで、期限を
// 追いながら丸ごと進める。
//
// STOP から抜けるのは割り込みだけで、割り込みを起こせるのはデバイスの
// 期限だけ。だから期限まで飛ばしても 1 サイクルも観測点を跨がない。
//
// **ループにするのが要点。** 1 期限ぶんだけ飛ばして返すと、呼び出し元の
// ホットループが「期限を張り直す → 4 サイクルずつ期限まで空回りする →
// また飛ぶ」を繰り返し、**飛ばした意味が無くなる**。実測で 400,000
// サイクル中 31,494 しか飛ばせていなかった。ここで割り込みが立つまで
// 追い続ければ、STOP 区間の全部が 1 回の呼び出しで消化される。
//
// 抜ける条件は 3 つ。どれも「次の命令が意味を持つ」瞬間になっている:
//   - 割り込みが受理待ちになった (次の step() が例外を積む)
//   - スライスが終わった (呼び出し元が抜ける)
//   - 期限が今そのもので 1 サイクルも進めない (毎命令へ縮退する)
template <bool FastMfp, bool FastRtc, bool FastCrtc>
void Machine::skipStopped()
{
    for (;;)
    {
        const std::uint64_t target = std::min(nextEventCycle(), sched_.sliceEnd());
        if (target <= sched_.now())
        {
            // 期限が今そのものなら 1 サイクルも飛ばせない。毎命令へ戻す。
            sched_.holdForPendingInterrupt();
            return;
        }

        // 飛ばす量を STOP の刻み (4 サイクル) の倍数へ切り上げる。
        //
        // Why これが要るか: 毎命令 tick 版は STOP 中も M68k::step() を
        // 呼び、そのたびに 4 サイクルずつ進む (m68k.cpp が stopped で 4 を
        // 返す)。つまりデバイスが見る時刻は必ず 4 の倍数の格子に乗る。
        // 飛ばす側が期限ちょうど (4 の倍数とは限らない) に着地すると、
        // 同じ総サイクル数を回しても CRTC のフレーム内位置が 1 ラスタ
        // ずれる。実際に rasterNumber が 151 と 152 で食い違って落ちた。
        //
        // 切り上げるのは、毎命令 tick 版が「期限を跨いだ最初の 4 の倍数」
        // まで進んでから割り込みを配送するため。切り下げると配送が
        // 1 刻み早くなる。
        constexpr u32 kStopStep = 4;
        u32 jump = static_cast<u32>(target - sched_.now());
        const u32 remainder = jump % kStopStep;
        if (remainder != 0)
        {
            jump += kStopStep - remainder;
        }
        stopSkippedCycles_ += jump;
        sched_.injectPending(jump);
        settle<FastMfp, FastRtc, FastCrtc>();

        // 飛んだ先で割り込みを配送する。受理待ちになったら CPU が起きるので、
        // 期限を張り直して呼び出し元へ返す。
        serviceInterrupts();
        const bool wakes = cpu_.pendingInterruptLevel() != 0;
        if (wakes || sched_.reachedSliceEnd())
        {
            rearmDeadline();
            return;
        }

        // 配送できない保留が残っているなら、毎命令リトライへ縮退する。
        // 飛ばし続けると、マスクが下りる瞬間を跨いでしまう。
        const bool blocked =
            mfp_.hasPendingInterrupt() || scc_.hasPendingInterrupt() || iosc_.hasPendingInterrupt();
        if (blocked)
        {
            sched_.holdForPendingInterrupt();
            return;
        }
    }
}

Settled Machine::materialize()
{
    // 未実体化ぶんが 0 なら何もしない。イベント駆動を切っているときは
    // 常にここで返る (毎命令 tick なので溜まらない)。
    //
    // 判定の前に unsettled_ を引き直すこと。毎命令の advance() は debt_
    // しか触らないので、引き直す前の pending() は前回の遅い側からの
    // 経過を含んでいない。
    sched_.syncUnsettled();
    if (sched_.pending() == 0)
    {
        return Scheduler::certify();
    }

    // Why not テンプレートで分けないか: ここは I/O アクセスの経路で、
    // 毎命令のホットループではない。ゲストが $E88000 台を読むのは
    // 割り込みハンドラの中や初期化のときだけなので、分岐が乗っても
    // 測れない。有効側の生成コードを変えてはいけないという規律は
    // ホットループにかかるもので、ここには及ばない。
    //
    // 3 つを個別に見るのは run() と同じ理由。まとめて allEnabled() で
    // 判定すると、RTC だけ切ったつもりが MFP と CRTC まで切れて、
    // イベント駆動の同値テストが「スイッチの取り違え」を見逃す。
    if (perf_.inlineMfpTimer)
    {
        if (perf_.inlineRtcTick)
        {
            perf_.inlineCrtcTick ? settle<true, true, true>() : settle<true, true, false>();
        }
        else
        {
            perf_.inlineCrtcTick ? settle<true, false, true>() : settle<true, false, false>();
        }
    }
    else if (perf_.inlineRtcTick)
    {
        perf_.inlineCrtcTick ? settle<false, true, true>() : settle<false, true, false>();
    }
    else
    {
        perf_.inlineCrtcTick ? settle<false, false, true>() : settle<false, false, false>();
    }
    return Scheduler::certify();
}

std::uint64_t Machine::nextEventCycle() const
{
    // 3 つの期限生成元の最小値。値は「今から何サイクル後か」で持ち、
    // 最後に now_ を足して絶対サイクルへ直す。
    //
    // MFP: IER で許可されているタイマのタイムアウト。IER が落ちている
    //      タイマ (X68000 の既定ではタイマ B) は IPR を立てないので入らない。
    //      これが段 4 の核心で、8 サイクル期限を外せる理由。
    // CRTC: 垂直帰線の開始と終了の 2 点。GPIP4 が変わり、AER 次第で
    //      割り込みも上がる。
    // RTC: 秒の繰り上がり。
    u32 best = mfp_.cyclesUntilNextRaise();
    best = std::min(best, crtc_.cyclesUntilVBlankEdge());
    best = std::min(best, rtc_.cyclesUntilCarry());

    return sched_.now() + best;
}

void Machine::rearmDeadline()
{
    sched_.armDeadline(nextEventCycle());
}

Machine::DeviceFingerprint Machine::fingerprint()
{
    DeviceFingerprint f;
    // peek を使うのは、read が UDR で副作用を持つため。タイマの現在値だけは
    // peek がリロード値を返すので、read 側の値を別に取る必要がある —
    // ここでは Mfp の内部を覗かず、read が返す値をそのまま指紋にする。
    f.ipra = mfp_.peek(Mfp::kIpra);
    f.iprb = mfp_.peek(Mfp::kIprb);
    f.gpip = mfp_.peek(Mfp::kGpip);
    f.timerValue[0] = mfp_.read(Mfp::kTadr);
    f.timerValue[1] = mfp_.read(Mfp::kTbdr);
    f.timerValue[2] = mfp_.read(Mfp::kTcdr);
    f.timerValue[3] = mfp_.read(Mfp::kTddr);
    f.second = (rtc_.read(Rtc::kSecond10) * 10u) + rtc_.read(Rtc::kSecond1);
    f.inVBlank = crtc_.inVerticalBlank();
    return f;
}

// 段 1 の shadow 検証: 期限を予測して、これから測り始める。
void Machine::shadowBeginPrediction()
{
    // 予測する期限は「割り込みが上がる時点」ではなく「外から見える
    // 状態が 1 つでも変わる時点」。指紋にはタイマの現在値も入れてある。
    //
    // Why not nextEventCycle() をそのまま使うか: あれは IER で許可された
    // タイマしか見ない。タイマ B は IPR を立てないので期限には入らないが、
    // timerValue_ は 8 サイクルごとに変わる。予測を nextEventCycle と
    // 同じにすると、この検証は「自分が正しいと思っていること」を
    // 確かめるだけになり、実体化リストの穴を一つも捕まえられない。
    u32 best = mfp_.cyclesUntilAnyTimerChange();
    best = std::min(best, crtc_.cyclesUntilVBlankEdge());
    best = std::min(best, rtc_.cyclesUntilCarry());

    shadowPredicted_ = best;
    shadowElapsed_ = 0;
    shadowBaseline_ = fingerprint();
    shadowArmed_ = true;
}

// 実際に最初に状態が変わったサイクルを、予測と突き合わせる。
void Machine::shadowObserve(u32 used)
{
    if (!shadowArmed_)
    {
        shadowBeginPrediction();
        return;
    }

    const DeviceFingerprint nowPrint = fingerprint();
    if (!(nowPrint != shadowBaseline_))
    {
        // まだ何も変わっていない。予測を過ぎているなら、期限が **遅すぎた**
        // ことにはならない (予測より遅く変わる分には飛び越さない) ので
        // 見逃してよい。ここで数えるのは「予測より早く変わった」だけ。
        return;
    }

    ++shadowChecks_;

    // 変化が起きたのは (前の命令の終わり, 今] の区間。実際の変化時刻を
    // T とすると intervalBegin < T <= shadowElapsed_。
    //
    // 予測 P がこの区間より **後ろ** にあれば (P > shadowElapsed_)、
    // 飛ばす実装は P まで飛んで変化を **飛び越して** いた。これが致命傷。
    //
    // P がこの区間より手前なのは許す。予測が保守的すぎるだけで、飛ぶ量が
    // 短くなり、その先で改めて期限を引き直す。正しさは失われない。
    const u32 intervalBegin = shadowElapsed_ - used;
    const bool jumpedOver = shadowPredicted_ > shadowElapsed_;
    if (jumpedOver)
    {
        ++shadowMismatches_;
    }
    // 予測が区間の中にぴたりと入った回数も見たい。ここが 0 のままなら、
    // 予測が常に保守的すぎて何も飛ばせていないことになる。
    const bool exact = shadowPredicted_ > intervalBegin && shadowPredicted_ <= shadowElapsed_;
    if (exact)
    {
        ++shadowExact_;
    }

    shadowArmed_ = false;
}

u32 Machine::runNullExec(u32 cycles)
{
    // JIT の上限と、デバイス処理の内訳を測るモード。命令の実行そのものを
    // 空回しにして、ループ運営・割り込み判定・tickDevices だけを残す。
    //
    // Stage をテンプレート引数にして入口で 1 回だけ分岐する。
    //
    // Why not 実行時の int を毎命令見ないか: それだと計測器自身が
    // 毎命令のロードと分岐になり、測ろうとしている対象にコストを乗せる。
    // 本番の '&' スイッチで同じ誤りを一度犯している (perf_switch.h)。
    // 計測モードでも同じ規律を守らないと、出てくる内訳が信用できない。
    // 段 0-3 は導入時の意味をそのまま残す。過去の実測 (tickDevices 59% /
    // serviceInterrupts 28% / 床 18.75 ns) と比べられなくなるため。
    // 段 4-7 を足して、tickDevices の中身を 1 つずつ落とす。
    //
    //   0: 全部含む                    ← 基準
    //   1: tickDevices を外す
    //   2: 割り込み判定を外す
    //   3: 両方外す                    ← 床 (ループ運営だけ)
    //   4: MFP だけ外す                ← 段 0 との差が MFP の寄与
    //   5: RTC だけ外す                ← 同上 RTC
    //   6: CRTC だけ外す               ← 同上 CRTC。変更 4 の判断はこれで決まる
    //   7: CRTC だけ残す               ← 4-6 の差分の和が合うかの裏取り
    //
    // Why not 4-6 の差だけで済ませないか: 3 つの tick は同じループの中で
    // キャッシュとレジスタを共有している。1 つ外した差が単独の寄与と
    // 一致する保証は無い。7 で「CRTC だけ」を直接測り、段 3 (床) との差が
    // 段 0 と段 6 の差と揃うかを見れば、内訳が加法的かどうかが分かる。
    // 揃わないなら差分ではなく段 7 の側を信じる。
    switch (nullExecStage_)
    {
        case 1:
            return runNullExecWith<false, false, false, true>(cycles);
        case 2:
            return runNullExecWith<true, true, true, false>(cycles);
        case 3:
            return runNullExecWith<false, false, false, false>(cycles);
        case 4:
            return runNullExecWith<false, true, true, true>(cycles);
        case 5:
            return runNullExecWith<true, false, true, true>(cycles);
        case 6:
            return runNullExecWith<true, true, false, true>(cycles);
        case 7:
            return runNullExecWith<false, false, true, true>(cycles);
        default:
            return runNullExecWith<true, true, true, true>(cycles);
    }
}

template <bool WithMfp, bool WithRtc, bool WithCrtc, bool WithInterrupts>
u32 Machine::runNullExecWith(u32 cycles)
{
    // 1 命令 4 サイクルとするのは、実測した平均が約 4 サイクルだったため
    // (400M サイクルで 99,998,982 命令 = 4.00)。
    //
    // **状態は進まない。** 速度の上限を見るためだけのモードで、
    // ゲストは動かない (jit_probe.h を見よ)。
    constexpr u32 kCyclesPerInstruction = 4;
    u32 spent = 0;
    while (spent < cycles)
    {
        if (WithInterrupts)
        {
            serviceInterrupts();
        }
        spent += kCyclesPerInstruction;

        // Why not tickDevices を呼ばずにここへ展開したか: tickDevices は
        // 本番の run() が通る関数で、テンプレート引数を 3 から 6 へ
        // 増やすと有効側の実体化が 8 通りから 64 通りへ増える。
        // 計測のためだけに本番経路の生成コードを触らない (perf_switch.h)。
        // 中身は tickDevices と同じ順序・同じ FastPath で並べてある。
        if (WithMfp)
        {
            mfp_.tickFast<true>(kCyclesPerInstruction);
        }
        if (WithRtc)
        {
            rtc_.tickFast<true>(kCyclesPerInstruction);
        }
        if (WithCrtc)
        {
            if (crtc_.tickFast<true>(kCyclesPerInstruction))
            {
                mfp_.setVerticalBlank(crtc_.inVerticalBlank());
            }
        }
    }
    return spent;
}

// runNullExec の switch が使う組み合わせだけを実体化する。
// 16 通り全ては要らない (使わない実体はコードサイズを食うだけ)。
template u32 Machine::runNullExecWith<true, true, true, true>(u32);      // 段 0
template u32 Machine::runNullExecWith<false, false, false, true>(u32);   // 段 1
template u32 Machine::runNullExecWith<true, true, true, false>(u32);     // 段 2
template u32 Machine::runNullExecWith<false, false, false, false>(u32);  // 段 3
template u32 Machine::runNullExecWith<false, true, true, true>(u32);     // 段 4
template u32 Machine::runNullExecWith<true, false, true, true>(u32);     // 段 5
template u32 Machine::runNullExecWith<true, true, false, true>(u32);     // 段 6
template u32 Machine::runNullExecWith<false, false, true, true>(u32);    // 段 7

template <bool FastMfp, bool FastRtc, bool FastCrtc>
void Machine::tickDevices(u32 cycles)
{
    mfp_.tickFast<FastMfp>(cycles);

    // RTC も CRTC もまとめない。
    //
    // 一度は「RTC は 1 秒単位でしか変わらないから粗くてよい」「CRTC の
    // ずれは 1 ラスタ未満だから見えない」として quantum を入れたが、
    // どちらも実測で観測可能なずれが出た:
    //   CRTC 64 サイクル -> 垂直帰線の開始が 24 サイクル遅れる
    //   RTC 10000 サイクル -> 秒の境界が最大 1998 サイクル遅れる
    //   MFP 8 サイクル -> 分周器の位相次第で割り込みが 4 サイクル遅れる
    //
    // いずれもゲストが命令単位でポーリングできる値なので、「分解能より
    // 細かいから見えない」は成り立たない。ポーリングループの反復回数
    // として観測できる。速度のために正しさを崩す取引だったので戻した。
    rtc_.tickFast<FastRtc>(cycles);

    if (crtc_.tickFast<FastCrtc>(cycles))
    {
        mfp_.setVerticalBlank(crtc_.inVerticalBlank());
    }
}

// machine.cpp の中でしか呼ばれないので、明示的な実体化で足りる。
// ヘッダへ定義を出すと、machine.h を読む全ての TU が SASI の状態機械まで
// 抱えることになる。
template u32 Machine::runWith<true, true, true>(u32);
template u32 Machine::runWith<true, true, false>(u32);
template u32 Machine::runWith<true, false, true>(u32);
template u32 Machine::runWith<true, false, false>(u32);
template u32 Machine::runWith<false, true, true>(u32);
template u32 Machine::runWith<false, true, false>(u32);
template u32 Machine::runWith<false, false, true>(u32);
template u32 Machine::runWith<false, false, false>(u32);

// イベント駆動側も同じ 8 通り。有効側 (true,true,true) が本番の姿になる。
template u32 Machine::runEventDriven<true, true, true>(u32);
template u32 Machine::runEventDriven<true, true, false>(u32);
template u32 Machine::runEventDriven<true, false, true>(u32);
template u32 Machine::runEventDriven<true, false, false>(u32);
template u32 Machine::runEventDriven<false, true, true>(u32);
template u32 Machine::runEventDriven<false, true, false>(u32);
template u32 Machine::runEventDriven<false, false, true>(u32);
template u32 Machine::runEventDriven<false, false, false>(u32);

// shadow 検証はホスト専用で、速さを測る道具ではない。組み合わせは
// 「全部有効」と「全部無効」の 2 つだけ実体化する。
template u32 Machine::runShadowVerify<true, true, true>(u32);
template u32 Machine::runShadowVerify<false, false, false>(u32);

void Machine::serviceInterruptsSlow()
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

bool Machine::serviceIoScInterrupt()
{
    // 線は serviceInterrupts() が呼ぶ前に取り直してある。
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
    // **run() の外から呼ばれる。** IPRA の受信フル (kIntRecvFull) が
    // 時間と無関係に立つので、期限を切って次命令で遅い側へ落とす。
    //
    // 漏らしても割り込みは失われない (保留中フォールバックが救う) が、
    // 次のデバイスイベントまでキー入力が遅れる。
    sched_.wake();
    shadowInvalidate();
}

bool Machine::moveMouse(int dx, int dy, bool leftButton, bool rightButton)
{
    const bool accepted = scc_.moveMouse(dx, dy, leftButton, rightButton);
    if (accepted)
    {
        // 積めたときだけ期限を切る。false なら SCC の状態は変わっていない
        // ので、切っても遅い側を 1 回無駄に通るだけになる。
        sched_.wake();
    }
    return accepted;
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
            // タイマデータレジスタ (TADR/TBDR/TCDR/TDDR) と IPRA/IPRB は
            // 時間で変わる。溜まっている時間を流してから読む。
            //
            // 特に TBDR。X68000 は IERA bit0 = 0 でタイマ B を走らせるので
            // 割り込みは上がらず期限にも入らないが、ゲストは 8 サイクルに
            // 1 減るカウンタとして読める。ここを落とすと quantum を入れた
            // ときとまったく同じ観測可能なずれが読み出し側から再発する。
            return mfpRead(materialize(), (addr - kMfpBase) / 2);
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
            //
            // 秒は 10,000,000 サイクルに 1 度しか繰り上がらないが、
            // ゲストは秒レジスタを命令単位でポーリングできる。実体化を
            // 落とすと秒の境界が期限ぶん遅れて見える。
            return rtcRead(materialize(), (addr - kRtcBase) / 2);

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
                // 「settle → 適用 → 再計算」の 3 段。
                //
                // 1 段目を落とすと、TCDCR で分周を 200 から 4 へ変えた
                // ときに、旧分周で溜まっていた時間が新分周で消化されて
                // 偽の割り込みが連発する。3 段目を落とすと旧設定の期限の
                // まま走り続ける。どちらも型で強制する。
                mfpWrite(materialize(), sched_.rearm(), (addr - kMfpBase) / 2, value);
            }
            return;
        }

        case kRtcBase:
            // RTC は書き込みで期限が変わらない (Rtc::write も setDateTime も
            // cycleAccumulator_ に触らない)。それでも rearm を通すのは、
            // 「期限を変えない」が実装依存の結論だから。将来 write が
            // 位相を触るようになったとき、ここを直し忘れても壊れない。
            //
            // Why not 期限を捨てないか: 捨てると秒の位相がリセットされ、
            // ゲストが時計を書くたびに秒の境界がずれて実機と違う。
            // rearm は「引き直す」であって「捨てる」ではない。
            rtcWrite(materialize(), sched_.rearm(), (addr - kRtcBase) / 2, value);
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
