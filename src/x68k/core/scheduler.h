// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// イベント駆動の時間管理。
//
// 命令ごとに全デバイスへサイクルを配るのをやめ、「次にどれかのデバイスの
// 状態が変わる時点」まで時間を溜めておく。溜まった時間はその期限に達した
// 瞬間か、ゲストがその値を読みに来た瞬間に、まとめてデバイスへ流す
// (実体化)。quantum と違い、状態が変わる瞬間は 1 サイクルもずれない。
//
// 設計と、それを壊しにいった結果は
// docs/knowledge/event-driven-implementation.md にある。ここに書くのは
// 「どう実現しているか」だけ。
//
// ## 毎命令のホットパス
//
//   debt_ += used;
//   if (debt_ >= 0) { reachSlow(); }
//
// debt_ は「期限まであと何サイクルか」を **負数** で持つ。ゼロとの比較 1 本に
// 縮めるためで、変数同士の比較 (spent < cycles) を入れると過去 2 回と同じ
// 轍を踏む (-18%, -6.5% の実測)。
//
// ## 3 つの量を分けて持つ理由
//
// now_      : 実体化済みの絶対サイクル。デバイスが実際に見ている時刻
// span_     : 今の期限までの幅。debt_ = -span_ で張る
// sliceEnd_ : スライスの終端 (絶対)
//
// Why not sliceEnd_ を debt_ へ min で畳まないか: 畳むと「期限に達した」のか
// 「スライスが終わった」のかを debt_ と span_ から復元できない。タイのとき
// 区別がつかず、スライス超過を検出できないとフレーム同期が壊れる。
// 毎命令のホットパスは debt_ の 1 分岐のままで、比較が増えるのは遅い側だけ。

#ifndef X68K_CORE_SCHEDULER_H
#define X68K_CORE_SCHEDULER_H

#include <algorithm>
#include <cstdint>

#include "cpu/m68k_types.h"

namespace x68k
{

// 「機械全体の時間が実体化されている」ことの証明。
//
// デバイスの read/write は、これを受け取らないと呼べない。呼び出し側は
// Scheduler::settle() を通らないとこの値を作れないので、実体化の忘れが
// 型として起こらなくなる。
//
// Why not デバイスごとに型を分けるか: Mfp::read(kGpip) が返す GPIP4 の
// 実体は CRTC にある。「MFP は実体化した」しか意味しない型だと、その 1 本を
// 守れない。トークンは Scheduler が発行する 1 種類だけにして、
// 「機械全体が今の時刻に追いついている」を意味させる。
//
// Why not = default にするか: private な defaulted default ctor は
// m.read({}, reg) のような値初期化を通してしまう (GCC 14.2 で実測)。
// user-provided な本体 {} を書くと 'Settled::Settled()' is private で落ちる。
// この 1 点が型安全性の全体を支えるので、test_scheduler.cpp で
// std::is_default_constructible_v が false であることを固定してある。
class Settled
{
private:
    friend class Scheduler;
    Settled() {}
};

// 期限の再計算をスコープで強制する。
//
// 書き込みは必ず「settle (旧設定で消化) → 適用 → 再計算」の 3 段でなければ
// ならない。3 段目を忘れると、旧設定で張った期限のまま走り続ける。
// デストラクタに置けば、途中で return しても抜ける道が無い。
class Scheduler;
class Rearm
{
public:
    ~Rearm();
    Rearm(const Rearm&) = delete;
    Rearm& operator=(const Rearm&) = delete;

private:
    friend class Scheduler;
    explicit Rearm(Scheduler& sched) : sched_(&sched) {}
    Scheduler* sched_;
};

class Scheduler
{
public:
    // 期限がこれより近いなら、イベント駆動をやめて毎命令 settle へ縮退する。
    //
    // reachSlow の呼び出しと期限の再計算のコストが、飛ばして得られる利得を
    // 上回る領域があるため。IERA bit0 (タイマ B の割り込み) が立つと期限が
    // 8 サイクルまで縮み、そのまま張ると現行の 2-3 倍遅くなる。
    //
    // Why not 0 を張らないか: 0 だと reachSlow が毎命令 call8 になり、
    // 現行のインライン早期リターン (mfp.h の tickFast) より遅くなる。
    // 縮退側では debt_ = 0 を張って毎命令 reachSlow へ入るが、そこで
    // 期限の再計算を省くので call8 1 回ぶんで収まる。
    static constexpr u32 kMinDeadline = 256;

    // 期限が 1 つも無いときに張る幅。
    //
    // Why not 無限にしないか: 無限だとスライス終端まで一度も実体化されず、
    // 溜まった時間が u32 を溢れる経路が生まれる。CRTC は 1 フレームを超える
    // サイクルを渡されるとその間の垂直帰線エッジを丸ごと落とす
    // (video.h の tickFast のコメント) ので、フレーム未満で必ず刻む。
    static constexpr u32 kFallbackSpan = 65536;

    // 実体化を 1 回で渡す上限。CRTC が 1 フレーム以上をまとめて受け取ると
    // その間の垂直帰線の開始と終了を報告しなくなるため、必ず下で刻む。
    static constexpr u32 kMaxSettleChunk = 65536;

    void reset()
    {
        debt_ = 0;
        now_ = 0;
        unsettled_ = 0;
        deadlineAt_ = 0;
        sliceEnd_ = 0;
        sliceBegin_ = 0;
        degraded_ = true;
    }

    // 期限まであと何サイクルか。負のうちは飛ばしてよい。
    [[nodiscard]] std::int32_t debt() const
    {
        return debt_;
    }

    // まだデバイスへ渡していないサイクル数。
    //
    // unsettled_ は CPU が実行し終えた仮想時刻、now_ はデバイスが実際に
    // 見ている時刻。その差がそのまま「渡していない量」になる。
    [[nodiscard]] u32 pending() const
    {
        return static_cast<u32>(unsettled_ - now_);
    }

    [[nodiscard]] std::uint64_t now() const
    {
        return now_;
    }

    [[nodiscard]] bool degraded() const
    {
        return degraded_;
    }

    // スライスの開始。sliceEnd_ を絶対サイクルで固定する。
    void beginSlice(u32 cycles)
    {
        sliceBegin_ = now_;
        sliceEnd_ = now_ + cycles;
    }

    // スライスで実際に消費したサイクル数。実体化済みの分だけを返す。
    [[nodiscard]] u32 sliceSpent() const
    {
        return static_cast<u32>(now_ - sliceBegin_);
    }

    // スライス終端に達したか。実体化済みの時刻で判断する。
    [[nodiscard]] bool reachedSliceEnd() const
    {
        return now_ >= sliceEnd_;
    }

    [[nodiscard]] std::uint64_t sliceEnd() const
    {
        return sliceEnd_;
    }

    // 命令を実行せずに時間を進めたことにする (STOP の一括飛び越し)。
    //
    // 呼ぶのは settle 済み (pending() == 0) のときだけ。直後にもう一度
    // settle して、飛ばしたぶんをデバイスへ流すこと。
    void injectPending(u32 cycles)
    {
        unsettled_ += cycles;
        // debt_ の不変条件 (debt_ == unsettled_ - deadlineAt_) を保つ。
        // 次の settle が syncUnsettled() で unsettled_ を引き直すので、
        // ここを揃えないと飛ばしたぶんが消える。
        debt_ += static_cast<std::int32_t>(cycles);
    }

    // 命令が消費したサイクルを積む。戻り値が true なら遅い側へ入る。
    //
    // **毎命令通る唯一の経路**。加算 1 とゼロ比較 1 だけに保つこと。
    // unsettled_ の更新は遅い側でまとめて行う (下の syncUnsettled)。
    bool advance(u32 used)
    {
        debt_ += static_cast<std::int32_t>(used);
        return debt_ >= 0;
    }

    // 未実体化ぶんを消費したことにして now_ を進める。
    // 実際にデバイスへ渡す仕事は呼び出し側 (Machine) が持つ。
    //
    // 期限は deadlineAt_ に絶対サイクルで持っているので、now_ が進んでも
    // 動かない。
    //
    // Why not span_ からの相対で持たないか: 相対だと、ゲストがレジスタを
    // 読んで途中で実体化するたびに期限が同じだけ後ろへずれる。TBDR を
    // 毎命令ポーリングするループでは期限が永久に来なくなり、スライスの
    // 終端すら越える (実際に踏んだ)。
    void consume(u32 cycles)
    {
        now_ += cycles;
    }

    // debt_ から unsettled_ を引き直す。遅い側の入口で 1 回だけ呼ぶ。
    //
    // 毎命令の advance() は debt_ しか触らない (ホットパスを 4 命令に
    // 保つため)。debt_ = unsettled_ - deadlineAt_ という関係から、
    // unsettled_ をここで復元する。
    void syncUnsettled()
    {
        unsettled_ = deadlineAt_ + static_cast<std::int64_t>(debt_);
    }

    // 期限を張り直す。next は「次にどれかのデバイスの状態が変わる絶対
    // サイクル」。
    //
    // 呼ぶ前に必ず未実体化ぶんを 0 にしておくこと (pending() == 0)。
    void armDeadline(std::uint64_t next)
    {
        // スライス終端より先の期限は張らない。飛び越すと sliceSpent が
        // 要求サイクルを超え、呼び出し側のフレーム同期がずれる。
        //
        // 期限が無くても刻む。CRTC が 1 フレーム以上を一度に受け取ると
        // 垂直帰線エッジを落とすため (video.h の tickFast のコメント)。
        const std::uint64_t limit = std::min({next, sliceEnd_, now_ + kFallbackSpan});

        const std::uint64_t delta = limit > now_ ? limit - now_ : 0;
        if (delta < kMinDeadline)
        {
            // 期限が近すぎる。飛ばす利得よりオーバーヘッドが勝つので、
            // 毎命令 reachSlow へ縮退する (現行と同じ形)。
            degraded_ = true;
            deadlineAt_ = now_;
            debt_ = 0;
            return;
        }
        degraded_ = false;
        deadlineAt_ = limit;
        debt_ = -static_cast<std::int32_t>(delta);
    }

    // 保留中の割り込みが配送できなかった。期限を張らず、次命令で必ず
    // 遅い側へ戻す。
    //
    // 配送失敗は状態を変えない (serviceMfpInterrupt はマスク中に
    // acknowledge せず false を返す) ので、新しいエッジは二度と来ない。
    // ここで毎命令リトライへ縮退させることが、割り込みの正しさを
    // wake の網羅性から切り離す唯一の仕組みになっている。
    void holdForPendingInterrupt()
    {
        degraded_ = true;
        deadlineAt_ = now_;
        debt_ = 0;
    }

    // 実体化の証明を発行する。Machine が settle を終えた直後にだけ呼ぶ。
    [[nodiscard]] static Settled certify()
    {
        return Settled{};
    }

    // 期限の再計算をスコープの終わりへ予約する。
    [[nodiscard]] Rearm rearm()
    {
        return Rearm{*this};
    }

    // Rearm のデストラクタから呼ばれる。期限を今すぐ切って、次命令で
    // 必ず遅い側 (reachSlow) へ落とす。そこで期限が引き直される。
    //
    // Why not ここで直接引き直さないか: 期限の計算はデバイスへ問い合わせる
    // 仕事で、Scheduler はデバイスを知らない。知らせると core/ の依存が
    // 逆流し、Scheduler の単体テストにデバイス一式が要る。
    //
    void requestRearm()
    {
        wake();
    }

    // 期限を今すぐ切る (wake)。次命令が必ず遅い側へ落ちる。
    //
    // 未実体化ぶんは保存する。溜まった時間を捨てると、その時間が
    // デバイスへ二度と渡らずに消える。期限を unsettled_ そのものへ
    // 置けば、debt_ = 0 でも pending() の値は変わらない。
    void wake()
    {
        // 先に unsettled_ を引き直す。
        //
        // Why これが要るか: unsettled_ を更新するのは syncUnsettled() だけで、
        // それを呼ぶのは settle() と materialize() に限られる (毎命令の
        // advance() は debt_ しか触らない。ホットパスを 4 命令に保つため)。
        // つまり「遅い側へ入っていない状態で wake() を呼ぶ」と、前回の
        // 遅い側からの経過サイクルが deadlineAt_ の代入で丸ごと消える。
        //
        // 現在の呼び出し元は materialize() と組で使われるため偶然助かって
        // いるが、それは **C++ の full-expression 内のデストラクタ順序** に
        // 依存している。Settled / Rearm でわざわざ型に固定した規律の外側の
        // 規則に正しさが乗るのは危うい。materialize() を伴わない wake() を
        // 1 箇所足した瞬間に、そこから溜まった時間が黙って消える
        // (症状はタイマが遅れる・秒がずれるで、原因へ辿るのが難しい)。
        //
        // wake() は遅い側 (I/O アクセスと外部注入) でしか呼ばれないので、
        // 1 行足してもホットパスには影響しない。
        syncUnsettled();
        deadlineAt_ = unsettled_;
        debt_ = 0;
        degraded_ = true;
    }

private:
    // 期限まであと何サイクルか。負数で持つ。**毎命令書き換わる唯一の値**。
    //
    // 不変条件: debt_ == unsettled_ - deadlineAt_。
    // 毎命令の advance() は debt_ だけを触り、unsettled_ は遅い側の入口で
    // syncUnsettled() が引き直す。ホットパスを 4 命令に保つための分担。
    std::int32_t debt_ = 0;
    // 実体化済みの絶対サイクル。デバイスが見ている時刻。
    std::uint64_t now_ = 0;
    // CPU が実行し終えた絶対サイクル。now_ との差が未実体化ぶん。
    std::uint64_t unsettled_ = 0;
    // 次に遅い側へ落ちる絶対サイクル。
    std::uint64_t deadlineAt_ = 0;
    std::uint64_t sliceEnd_ = 0;
    std::uint64_t sliceBegin_ = 0;
    // 縮退中 (期限が近すぎるか、保留中の割り込みがある)。
    bool degraded_ = true;
};

inline Rearm::~Rearm()
{
    sched_->requestRearm();
}

}  // namespace x68k

#endif  // X68K_CORE_SCHEDULER_H
