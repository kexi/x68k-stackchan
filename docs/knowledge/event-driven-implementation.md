---
title: イベント駆動デバイス — 実装確定版
description: 命令ごとの tickDevices / serviceInterrupts をやめ、次に状態が変わる時点まで飛ばす設計の確定版。複数の敵対的レビューで壊しにいった結果を反映してある。実装済みで、実機で +51.9% (4991 -> 7579 kHz、実機比 76%) を実測した。
type: reference
tags: [x68k, emulator, esp32s3, performance, event-driven, mfp, crtc, rtc, interrupt]
verified: measured
sources:
  - 実測（M5Stack CoreS3、ESP-IDF v5.5.2、Human68k 3.02 稼働中、2026-08-17）
  - 多エージェントによる設計と敵対的検証（調査 3 / 設計 3 / 攻撃 9 / 統合 1）
updated: 2026-08-17
---

# イベント駆動デバイス — 実装確定版

## 実装結果 (2026-08-17、CoreS3 実機、Human68k 稼働中)

**実装して実機で測った。同じ起動の中で `'$'` を切り替え、ON → OFF → ON で再現。**

| | 実効クロック | 実機比 |
|---|---|---|
| OFF | 4991 / 4989 kHz | 49.9% |
| **ON** | **7579 kHz** | **75.8%** |

**+51.9%。** 試算は 12655 kHz だったので、届いたのは 6 割ほど。残差の
原因は `serviceInterrupts` を毎命令のまま残していること (28% ぶん) と、
縮退 (`degraded_`) に落ちる区間があること。次段で詰める。

検証: ホストテスト 559 件 / 226,521 assertions、状態が完全に不変
(400M サイクルで 99998982 命令 / `$ED0000=82773638`)、Human68k が
`A>` まで起動して `dir` が動く。

## 0. 事前検証で確定した事実 (推測でなく実コードから)

攻撃レポートの主張のうち、設計を変える 5 件を実コードで確認した。

| 主張 | 確認 | 実体 |
|---|---|---|
| STOP は 0 でなく 4 を返す | **正しい** | `m68k.cpp:651-655` `if (st_.stopped) { return 4; }`。0 を返すのは `m68k.cpp:612` の halted のみ |
| `rasterNumber()` が同値テストの比較対象 | **正しい** | `test_device_timing.cpp:324`。`video.cpp:41-55` は `frameCycles_ * kRasterCount / kCyclesPerFrame` の比例計算 |
| `setVerticalBlank` は raise 前に GPIP を書く | **正しい** | `mfp.cpp:330-339` が無条件に `reg_[kGpip]` を更新し、`mfp.cpp:341-352` で初めて IER ゲートのかかる `raise` を呼ぶ |
| IoSc は受理しても保留が落ちない | **正しい** | `iosc.h:170` `[[nodiscard]] u8 acknowledgeInterrupt() const;` — const なので `status_` を触れない |
| DMA 完了経路にフックが無い | **正しい** | `fdc.cpp:825` の `interruptPending_ = true` は `machine.cpp:659`/`766` のどちらからも到達しない。`dmac_.write` (`machine.cpp:747`) の直後にフックが要る |

加えて **設計を変える誤りを 1 件見つけた**: `Crtc::tickFast` (`video.h:81`) は `cycles >= kCyclesPerFrame` で `tickSlow` へ落ちるが、**`tickSlow` も 1 フレーム超のエッジは報告しない** (`video.h:54-58` のコメントが明言)。「飛ばす量を必ずエッジで切る」は選択肢ではなく必須条件。

---

## 1. 採用する設計と理由

### 採用: `debt_` 単一カウンタ + **保留中フォールバック** + **STOP 専用の飛び越し**

3 案の骨格 (負数 `debt_`、絶対サイクルの期限表、`Settled` トークン) は採るが、**攻撃で fatal と判定された 3 点で構造を変える**。

#### 変更 1: 割り込みは「wake エッジ」でなく「保留中フォールバック」で守る

deadline-cache 案は「配送可能性が変わる契機で `debt_ = 0`」という**エッジモデル**だった。これは interrupt-loss レンズの指摘どおり原理的に不足する。理由は 2 つあり、どちらも実コードで確認した。

1. **配送失敗は状態を変えない。** `serviceMfpInterrupt` (`machine.cpp:406-411`) はマスク中に `acknowledgeInterrupt` を呼ばずに `return false` する。IPR は立ったまま、wake だけが消費される。現行はこれを「次命令で必ず再試行される」ことに全面依存している。
2. **IoSc はレベル保持。** `acknowledgeInterrupt` が const (`iosc.h:170`) なので受理しても `status_` が下がらない。新しいエッジは二度と来ない。

**採る形**: `reachSlow()` の末尾で「保留があるのに 1 つも配送できなかった」なら **`debt_` を再 arm せず 0 のまま返す**。次命令の `debt_ += used` が必ず正になり、現行と同じ毎命令リトライへ縮退する。

```
保留なし        → 期限まで飛ぶ            (通常。ここで 28% が効く)
保留あり・配送済 → 期限まで飛ぶ
保留あり・不能  → debt_ = 0 のまま        (ハンドラ内。現行と同じ速度へ縮退)
```

これで**正しさが `setSr` フックの網羅性に依存しなくなる**。`setSr` callback は後から足す純粋な速度最適化になり、将来 SR を触る命令が増えても静かに壊れない。攻撃が maintainability として挙げた指摘をそのまま採用する。

> ハンドラ内が現行速度に落ちる代償は認める。X68000 は垂直帰線ごとに数千命令がハンドラ内なので、28% の削減は実効で目減りする。段 5 で実測し、足りなければ段 7 で `setSr` callback を**上乗せ**する (正しさの前提ではないので、いつでも外せる)。

#### 変更 2: `debt_` は期限専用にし、スライス終端を畳まない

perf レンズの指摘どおり `min()` は情報を落とす。「`debt_` と `span_` から理由を復元できる」は算術的に不可能で、タイのとき区別できない。スライス超過を検出できないとフレーム同期が壊れる。

**採る形**: スライス終端は `debt_` に入れず、`armDeadline` が `min(次イベント, スライス残り)` を取ったうえで **`sliceEnd_` を絶対サイクルで別に保持**する。`reachSlow()` は `now_ >= sliceEnd_` を見て終了を判断する。毎命令のホットパスは `debt_` の 1 分岐のままで、比較が増えるのは slow 側だけ。

#### 変更 3: STOP は専用の飛び越し経路を持つ

perf レンズの指摘どおり、`STOP` 中に `call8 M68k::step()` を 20,000 回回すのは**利得が最大のはずの区間での純粋な劣化**になる。かといってホットパスに `st_.stopped` のロードと分岐を足すのは規律違反。

**採る形**: `M68k::step()` は `stopped` のとき 4 を返す既存の契約を変えない。代わりに **`reachSlow()` の中で** `cpu_.state().stopped` を見る。STOP 中なら、そこから次の期限まで **1 回の `settle` で丸ごと飛ばして** `debt_` を張り直す。ホットパスは 1 命令も増えない。

STOP に入った直後は `debt_` がまだ負なので最大 1 期限ぶん (80,000 サイクル = 20,000 回の `step`) は空回りするが、その後は期限ごとに 1 回で飛ぶ。空回りを完全に消すには `M68k::setSr` の STOP 経路 (`m68k_ops_group4.cpp:167`) で `debt_ = 0` を書けばよい — これは正しさに要らない純粋な最適化なので段 7 に置く。

#### 変更 4: CRTC はイベント化しない (段 3 から外す)

timing レンズの指摘が正しい。`rasterNumber()` (`video.cpp:41-55`) は 568 段階すべてが観測可能な状態変化点で、平均 317 サイクルごとに値が変わる。`crtcEdge` (垂直帰線 2 点) では表現できず、`test_device_timing.cpp:324` が現に比較対象にしている。

**CRTC は毎命令 tick のまま残す。** `Crtc::tickFast` は既にインラインの早期リターン (`video.h:86-97`) で、`frameCycles_ += cycles` と 1 比較しかしない。ここをイベント化しても利得は小さく、`$E80028` 実装時に破綻する。**やらない。**

これは `tickDevices` の 59% のうち CRTC 分を諦めることを意味する。内訳の再測 (段 0) で CRTC 単独の寄与を先に確かめ、大きければ判断し直す。

#### 変更 5: `Settled` はデバイス単位でなく機械全体の証明にする

timing レンズの指摘どおり、`Mfp::read(Settled, kGpip)` の `Settled` が「MFP は実体化した」しか意味しないと GPIP4 (実体は CRTC) を守れない。ただし変更 4 で CRTC を毎命令 tick に残したので **GPIP4 は常に最新**であり、この穴は消える。それでも将来の再発を防ぐため、トークンは `Settled` 1 種類だけを `Scheduler` が発行する**機械全体の実体化証明**として定義する (デバイスごとに型を分けない)。

---

## 2. 毎命令のホットパス (確定版)

```cpp
// machine.h の private セクション。
// runWith の EventDriven=true 側だけが使う。
template <bool FastMfp, bool FastRtc, bool FastCrtc>
u32 Machine::runEventDriven(u32 cycles)
{
    sched_.beginSlice(cycles);   // sliceEnd_ = now_ + cycles; armDeadline()
    for (;;)
    {
        const u32 used = cpu_.step();
        if (used == 0)
        {
            // halted。溜まった時間を捨てずに実体化してから抜ける。
            // (attack: halt 経路だけ now_ が更新されない非対称を潰す)
            sched_.settleAndStop<FastMfp, FastRtc, FastCrtc>();
            break;
        }
        debt_ += static_cast<std::int32_t>(used);
        if (debt_ >= 0)                        // ← 毎命令の判定はこれだけ
        {
            if (reachSlow<FastMfp, FastRtc, FastCrtc>())
            {
                break;                          // スライス終端
            }
        }
    }
    return sched_.sliceSpent();
}
```

生成される Xtensa 命令列 (`-O2 -mlongcalls`、offset は vtable ぶん 0 にはならないので 20 と仮定):

```
.L3:
    mov.n a10, a7
    call8 _ZN4M68k4stepEv
    beqz.n a10, .L8              # used == 0 (halted のみ)
    l32i.n a8, a7, 20            # debt_ をロード      ← ロード 1 (narrow)
    add.n  a10, a10, a8          # debt_ += used       ← 加算 1
    s32i   a10, a7, 20           # 書き戻し            ← ストア 1 (wide)
    bltz   a10, .L3              # 期限前なら即ループ  ← 分岐 1
    mov.n  a10, a7
    call8  _ZN7Machine9reachSlowILb1ELb1ELb1EEEbv
    beqz.n a10, .L3
```

**ロード 1 / 加算 1 / ストア 1 / 分岐 1。** 現行の `spent < cycles` の `add.n` + `bltu` と `serviceInterrupts` の `call8` が消える。

### 攻撃で指摘された性能上の罠への対処

| 罠 | 対処 |
|---|---|
| STOP で `used==0` にならず空回り | `reachSlow` 内で `cpu_.state().stopped` を見て期限まで一括 settle。ホットパスは不変 |
| スライス終端を `min` に畳むと復元不能 | `sliceEnd_` を絶対サイクルで別持ち。`debt_` は期限専用 |
| halt 時に未実体化時間が消える | `used == 0` 経路で `settleAndStop` を必ず通す |
| `debt_` を offset 0 に置けない | Machine は多相基底 3 つ (`machine.h:48`) で offset 0 は vtable。**`perf_` の直後**に置き、offset ≤ 60 を狙う。`l32i.n` は narrow になるが `s32i.n` は適用外 — 設計根拠を訂正のうえ採用 (4 命令構成自体は変わらない) |
| IERA bit0 が立つと期限 8 サイクルで劣化 | `armDeadline` が「最小期限 < `kMinDeadline` (256 サイクル)」を検出したら **`debt_` を張らず毎命令 tick へ縮退**する (下記) |

### 最悪ケースの保護 (現行より遅くならない保証)

```cpp
// scheduler.h
static constexpr u32 kMinDeadline = 256;

void Scheduler::armDeadline()
{
    const std::uint64_t next = nextEventCycle();     // 全期限の min
    if (next == kNever) { armFallback(); return; }
    const std::uint64_t delta = next - now_;
    if (delta < kMinDeadline)
    {
        // 期限が近すぎる。イベント駆動のオーバーヘッドが利得を上回るので、
        // 毎命令 settle へ縮退する (現行と同じ形)。
        // Why not 0 を張らないか: 0 だと reachSlow が毎命令 call8 になり、
        // 現行のインライン早期リターン (mfp.h:131-135) より遅くなる。
        degraded_ = true;
        debt_ = 0;
        return;
    }
    degraded_ = false;
    span_ = static_cast<u32>(delta);
    debt_ = -static_cast<std::int32_t>(span_);
}
```

`degraded_` のときは `reachSlow` が `tickDevices` を呼んで `debt_ = 0` を書き直す。呼び出しは残るが、`Mfp::tickFast` のインライン早期リターンは効いたままなので現行 + `call8` 1 回で収まる。**IERA bit0 が立っても破綻せず、劣化幅が有界になる。**

---

## 3. 読み出し時に実体化しなければならないレジスタ (完全なリスト)

実体化 = `Scheduler::settle()` を呼び、`now_` から実経過ぶんをデバイスへ流す。**トークンはデバイスの `read`/`write` 側に置く** — `ioRead8` / `ioRead16` (`machine.cpp:841` で `ioRead8` x2 に落ちる) / DMA (`machine.cpp:933-941` が `bus_.read8` を直接呼ぶ) の 3 経路を一度に覆うため。

| # | レジスタ | アドレス | 実体 | 変化粒度 | 到達経路 |
|---|---|---|---|---|---|
| 1 | TADR | `$E8801F` | `timerValue_[0]` (`mfp.cpp:66-73`) | 分周 x 2 CPU cyc | `machine.cpp:577-591` |
| 2 | **TBDR** | `$E88021` | `timerValue_[1]` | **8 CPU cyc** (分周 4) | 同上 |
| 3 | TCDR | `$E88023` | `timerValue_[2]` | 400 CPU cyc (分周 200) | 同上 |
| 4 | TDDR | `$E88025` | `timerValue_[3]` | 分周 x 2 CPU cyc | 同上 |
| 5 | IPRA | `$E8800B` | `reg_[kIpra]`。`raise` (`mfp.cpp:281`) が立てる | タイマ A/B タイムアウト | 同上 |
| 6 | IPRB | `$E8800D` | `reg_[kIprb]` | タイマ C/D + GPIP4 | 同上 |
| 7 | RTC 秒〜年 | `$E8A001`〜 | `second_` 他 (`rtc.cpp:188-231`) | 10,000,000 cyc | `machine.cpp:604-607` |

**2 番の TBDR が設計の核心。** IERA bit0 = 0 なので割り込みは上げず期限計算に入らないが、ゲストは読める。実体化を落とすと quantum 撤回とまったく同じ観測可能なずれが読み出し側から再発する。

### 実体化が **不要** と確定したもの

| レジスタ | 理由 |
|---|---|
| **GPIP ($E88001)** | **変更 4 で CRTC を毎命令 tick に残したので常に最新。**イベント化するなら必須になる |
| ISRA/ISRB | `acknowledgeInterrupt` (`mfp.cpp:445,461`) でしか立たず、X68000 は VR=$40 (S=0) で常に 0 |
| UDR / RSR | `receiveKeyboardByte` (`mfp.cpp:355-361`) の外部注入のみ。時間駆動でない |
| TSR | `mfp.cpp:47,154` の定数 |
| CRTC R00-R23 | `Crtc::read` (`video.cpp:28-31`) は `reg_` をそのまま返す。**`$E80028` を読んでもラスタ番号は返らない** |
| `rasterNumber()` | 現在バスから到達不能 (`src` 内の呼び出し元 0)。`$E80028` を実装するなら 317 サイクル粒度の実体化が要る — **CRTC をイベント化しない限り自動的に守られる** |
| RTC バンク 1 | `rtc.cpp:191-195` は `bank1_` を返すだけ |
| Scc / IoSc / Fdc / Dmac / Sprite / Sram / Opm / Adpcm | CPU サイクルを受け取る口を持たない (`tick`/`cycle` が 0 ヒット) |

### `peek()` の扱い

`Mfp::peek` (`mfp.h:238-241`) は `reg_` を返すのでトークン不要のまま残す。ただし **タイマデータレジスタでは `read()` と値が違う** (`mfp.h:234-237` が明言)。ホストの `'@'` ダンプ (`host/main.cpp:1102-1111`, `main/main.cpp:1036-1041`) は `read()` 側へ寄せる — でないと古い値を出す。

---

## 4. 期限を無効化しなければならない書き込み経路 (完全なリスト)

全て **settle (旧設定で消化) → 適用 → 再計算** の 3 段。順序を型で強制する。

```cpp
auto s = sched_.settle();      // 旧設定で遅延中の時間を消化
auto r = sched_.rearm();       // スコープを抜けると recompute()
mfp_.write(s, r, reg, value);  // 書き込みを適用
```

| # | 経路 | 場所 | 変わるもの | 無効化 |
|---|---|---|---|---|
| 1 | TACR | `mfp.cpp:162-166` | `running_` 全体 (`mfp.h:194-218`) | 全タイマ期限 |
| 2 | TBCR | `mfp.cpp:167-171` | 同上 | 全タイマ期限 |
| 3 | **TCDCR** | `mfp.cpp:172-178` | **C と D を 1 回で両方** | 全タイマ期限 |
| 4 | TADR/TBDR/TCDR/TDDR | `mfp.cpp:121-138` | 停止中なら `timerValue_` 即反映 (`mfp.cpp:264-268`) | 次周期以降の期限 |
| 5 | IERA/IERB | `mfp.cpp:202-209` | `raise` の早期リターン条件 (`mfp.cpp:277-280`) | **どのタイマがイベント源か**。IERA bit0 が立つとタイマ B が本物のイベント源になる |
| 6 | VR | `mfp.cpp:185-195` | S を落とすと ISR を 0 に (`mfp.cpp:190-193`) | 割り込み wake のみ |
| 7 | IMRA/IMRB | `mfp.cpp:211-213` (default) | `hasPendingInterrupt` の `IPR & IMR` (`mfp.h:258`) | 割り込み wake のみ |
| 8 | IPRA/IPRB | `mfp.cpp:103-110` | 落とす方向のみ | **不要** |
| 9 | ISRA/ISRB | `mfp.cpp:103-110` | `serviceBlockMask` (`mfp.cpp:363-411`) が緩む | 割り込み wake のみ |
| 10 | AER | `mfp.cpp:211-213` (default) | GPIP4 のどちらのエッジで raise するか (`mfp.cpp:347-352`) | 割り込み wake のみ |
| 11 | `Machine::reset` | `machine.cpp:122-168` | 3 つの期限生成元が全て 0 に | **期限 + `debt_`/`span_` の両方を捨てる** |

### 無効化が **不要** と確定したもの (過剰無効化の禁止)

| 経路 | 根拠 |
|---|---|
| `Rtc::write` / `Rtc::setDateTime` | `rtc.cpp:51-64,111-173` は `cycleAccumulator_` に一切触れない。期限は `kCyclesPerSecond - cycleAccumulator_` (`rtc.h:96`) で時計の値に依存しない。**捨てると秒の位相がリセットされ実機と違う** |
| `Crtc::write` | `video.cpp:33-39` は `reg_` へ格納するだけ。周期は**コンパイル時定数** `kCyclesPerFrame=180342` / `kVBlankCycles=18000` (`video.h:42-44`)。CRTC レジスタに依存しない |
| `VideoController::write` | `video.cpp:134-163` はパレットとモードのみ |
| GPIP 書き込み | `mfp.cpp:140-143` で無視される |

> ⚠️ CRTC の「定数だから安全」は**実装依存の結論**。将来 R04-R09 から実周期を計算する実装へ変えたら反転する。

---

## 5. wake を要求しなければならない経路 (完全なリスト)

`wake()` = `debt_ = 0` の代入 1 つ。分岐はホットパスに 1 本も足さない。

**ただし変更 1 により、これらは全て「速度最適化」であって「正しさの前提」ではない。** 保留中フォールバックが最後の砦として全経路を無条件に救う。漏らしても割り込みは失われず、最悪でも次のデバイスイベントまで遅れるだけ。

### [CPU]

| # | 経路 | 場所 |
|---|---|---|
| 1 | `M68k::setSr` で IPL が**実際に下がった**とき | `m68k.cpp:73-98`。呼び出し元は RTE (`m68k_ops_group4.cpp:114`) / STOP (`:167`) / MOVE to SR (`:510`) / ANDI-ORI-EORI to SR (`m68k_ops_misc.cpp:258`) の 4 箇所 (grep で全数確認済み) |
| 2 | `st_.stopped == true` の間 | `m68k.cpp:651-655`。`reachSlow` 内で見る (§2 変更 3) |
| 3 | `requestInterrupt` が `pendingIrq_` を上げたとき | `m68k.cpp:590-597`。`level > pendingIrq_` のときだけ |

> `st_.sr =` の直代入は全て CCR ビットのみで IPL に触れないことを確認した (`m68k_ops_group4.cpp:125` の RTR は `kCcrMask` のみ、他は全てフラグ更新)。**この不変条件はテストで固定する** (§8 の T5)。

### [MFP — 時間]

| # | 経路 | 場所 |
|---|---|---|
| 4 | タイマのタイムアウトで `raise` が実発火する本数のみ | `mfp.cpp:284-326` → `271-282`。IER で早期リターンする本数は期限から除外 |
| 5 | CRTC の垂直帰線エッジ 2 点 | `machine.cpp:328-331` → `mfp.cpp:328-353`。**AER の値で raise するエッジが変わるので両方を期限に入れる** |

### [MFP — レジスタ] `machine.cpp:726-736` の 1 箇所で捕まる

| # | 経路 | 場所 |
|---|---|---|
| 6 | IMRA/IMRB | `mfp.cpp:211-213`。**マスク中に立った IPR が即座に配送可能になる** |
| 7 | IERA/IERB | `mfp.cpp:202-209` |
| 8 | IPRA/IPRB/ISRA/ISRB | `mfp.cpp:103-110`。ISR を落とすと下位優先度が通る |
| 9 | VR | `mfp.cpp:185-195` |
| 10 | AER | `mfp.cpp:211-213` |
| 11 | TACR/TBCR/TCDCR | `mfp.cpp:162-178` — 期限の再計算 |
| 12 | TADR〜TDDR | `mfp.cpp:121-138` — 期限の再計算 |

### [MFP — 外部注入]

| # | 経路 | 場所 |
|---|---|---|
| 13 | `Machine::pressKey` | `machine.cpp:458-461` → `mfp.cpp:355-361`。**run() の外から呼ばれる** |

### [SCC] `machine.cpp:665-666` (`sccRead`) と `743-745` (`sccWrite`) の 2 箇所で 15-17 を捕まえる

| # | 経路 | 場所 |
|---|---|---|
| 14 | `Machine::moveMouse` が **true を返したとき** | `machine.cpp:463-466` → `scc.cpp:395-403`。false なら状態不変なので wake 不要 |
| 15 | WR0 Reset Highest IUS / WR1 / WR3 / WR9 | `scc.cpp:233-244` / `296-301` / `286-291` / `270-280` |
| 16 | **`Scc::readData` の `refreshRxInterrupt`** | `scc.cpp:336`。**「1 バイト = 1 割り込み」を成立させている機構** (`scc.cpp:375-392` のコメント)。漏らすとマウスレポート 3 バイトの 2 バイト目以降が届かない |
| 17 | `Scc::writeData` | `machine.cpp:993` |

### [IoSc / FDC / DMAC]

| # | 経路 | 場所 |
|---|---|---|
| 18 | `$E9C001` / `$E9C003` 書き込み | `machine.cpp:747-749`。`enable_` が上がると既存の `status_` が通る (`iosc.h:158-162`) |
| 19 | `$E94003` 書き込み | `machine.cpp:766` — **既存** |
| 20 | `$E94003` 読み出し | `machine.cpp:659` — **既存** |
| 21 | **`dmac_.write` の直後** | `machine.cpp:747`。**現在フックが無い唯一の穴** |
| 22 | `Machine::reset` | `machine.cpp:135-168` |

### 21 の詳細 — `updateFdcInterruptLine` を毎命令から外せる根拠

`fdc.cpp` で `interruptPending_ = true` を立てる全 5 箇所を実測で確認した:

| 場所 | コマンド | 到達経路 | 既存フック |
|---|---|---|---|
| `fdc.cpp:534` | RECALIBRATE | `writeData` → `executeCommand` | `machine.cpp:766` ✅ |
| `fdc.cpp:566` | SEEK | 同上 | ✅ |
| `fdc.cpp:614` | READ ID | 同上 | ✅ |
| `fdc.cpp:646` | READ TRACK / WRITE ID | 同上 | ✅ |
| **`fdc.cpp:825`** | `finishExecute` | `readData` (`fdc.cpp:272`) | `machine.cpp:659` ✅ |
| **`fdc.cpp:825`** | `finishExecute` | **`dmaRead`/`dmaWrite`/`dmaComplete` (`fdc.cpp:828`/`887`/`860`)** | ❌ **無い** |

DMA 経路は `Dmac::start` の転送ループ (`dmac.cpp:96-138`) が `Dmac::write` の中で**同期的にバースト実行**される。つまり **1 命令の途中で、エミュレート時間 0 で、FDC の割り込み線が上がる**。

**`machine.cpp:747` の `dmac_.write(...)` の直後に `updateFdcInterruptLine()` を 1 行足せば、毎命令の呼び出しは完全に不要になる。**

落とす方向 (`fdc.cpp:145` reset / `:478` SENSE INTERRUPT STATUS) は配送可能性を減らすので wake 不要。`readStatus` は const (`fdc.cpp:222`)、`writeDriveControl`/`writeDriveSelect` (`fdc.cpp:394`/`399`) は `interruptPending_` を触らない。

---

## 6. fatal 判定への対処

| 攻撃の指摘 | レンズ | 対処 |
|---|---|---|
| GPIP4 は raise しなくても変化し、ゲストが読める | timing | **設計変更。CRTC をイベント化しない** (変更 4)。GPIP4 は常に最新になり穴が消える |
| STOP は 4 を返すので `used==0` で抜けない | timing / perf | **設計変更。`reachSlow` 内で `stopped` を見て一括飛ばし** (変更 3)。ホットパスは不変 |
| halt 経路で未実体化時間が消える | timing | **`used == 0` で `settleAndStop` を通す** (§2 のホットパス) |
| `rasterNumber` が同値テストの比較対象 (317 サイクル粒度) | timing | **CRTC をイベント化しないので発生しない**。`$E80028` を実装しても安全 |
| `Settled` がデバイス単位だと GPIP4 を守れない | timing | **トークンを機械全体の実体化証明に統一** (変更 5) |
| 配送失敗で wake が消費され保留が残る | interrupt-loss | **保留中フォールバック** (変更 1)。`setSr` 依存を正しさから外す |
| SCC の FIFO 補充が wake リストに無い | interrupt-loss | **リスト #16 に明記**。`machine.cpp:665-666` の 1 箇所で捕まる |
| IoSc がレベル保持で新エッジが来ない | interrupt-loss | **保留中フォールバックが無条件に救う**。段 2 単独投入時も毎命令リトライを残す (下記) |
| 複数デバイス同時保留で敗者の wake が無い | interrupt-loss | **保留中フォールバック**。1 つでも配送不能なら `debt_ = 0` |
| `min()` は情報を落とすのでスライス終端を復元できない | perf | **`sliceEnd_` を絶対サイクルで別持ち** (変更 2) |
| IERA bit0 が立つと現行の 2-3 倍遅い | perf | **`kMinDeadline` 縮退** (§2)。劣化幅が有界になる |
| `debt_` を offset 0 に置けない (vtable) / `s32i.n` は無い | perf | **設計根拠を訂正。`perf_` 直後に配置**して offset ≤ 60 を狙う。4 命令構成は変わらない |
| `Settled() = default` は偽造可能 | perf | **`Settled() {}` を user-provided にする。§8 の T6 でコンパイル失敗を固定** |

### やらないと明示するもの

1. **CRTC のイベント化。** `rasterNumber` が 317 サイクル粒度で観測可能。利得より破綻リスクが大きい。毎命令 tick のまま残す。
2. **`setSr` callback を正しさの前提にすること。** 段 7 の速度最適化としてのみ足す。
3. **STOP 中の完全な空回り解消。** 段 7 で `m68k_ops_group4.cpp:167` にフックを足すまでは、STOP 突入後の最大 1 期限ぶんは空回りする。
4. **`step()` のイベント駆動化。** `step()` (`machine.cpp:170-193`) はトレース用で常に毎命令 settle (`degraded_` 相当) として扱う。`run()` との同値性を `test_device_timing.cpp:243` が守る。
5. **`runNullExec` のイベント駆動化。** 通さない。ただし**内訳を測り直す** (段 0) — 通さないままだと計測値が本番と乖離する。

---

## 7. 実機で段階ごとに測れる導入順序

各段は Human68k を起動した状態 (`A>` プロンプト) で測る。ディスク無しだと同じ変更が +3.17% と +6.40% で倍違った実測がある。`PerfSwitch` と同じく**同一起動内で切り替えて**測り、焼き直しの揺れ (3711 vs 3630 kHz) と効果を混ぜない。

### 段 0: 内訳の再測 (実機、変更なし)

**なぜ最初か**: CRTC をイベント化しない判断 (変更 4) が正しいかは、CRTC 単独の寄与を知らないと決まらない。`tickDevices` の 118.76 ns/cycle を MFP / RTC / CRTC に分解する。

**測り方**: `nullExecStage_` にステージを 3 つ足し、`tickDevices` から 1 つずつ外したテンプレート実体を作る。**実行時 `int` で見てはいけない** — 計測器自身が毎命令のロードと分岐になり、床が 32000 → 53323 kHz へ動いた前例がある。

**期待**: CRTC が 20 ns/cycle 未満なら変更 4 を確定。それ以上なら「垂直帰線 2 点だけの期限 + `rasterNumber` を bus 非到達のまま凍結」を再検討する。

### 段 1: shadow 検証 (ホストのみ、速度は変わらない)

期限を計算するが**飛ばさない**。毎命令 tick は現行のまま。予測期限と、実際に最初に状態が変わったサイクルが一致するかを `assert` する。

**測り方**: ホストで Human68k を起動 (`--hdd ... --keys $'dir\
' --stats`)。実機計測は不要。

**なぜここ**: 不一致が出るなら以降は全部無意味。実機に持っていく前にホストで潰す。

### 段 2: `updateFdcInterruptLine` を毎命令から外す

`machine.cpp:747` の `dmac_.write` 直後に 1 行足し (§5 の #21)、`machine.h:313` の毎命令呼び出しを消す。**イベント駆動の本体と独立**に入れられる。

> ⚠️ **この段だけでは `serviceInterrupts` の毎命令リトライを外さない。** IoSc はレベル保持なので (`iosc.h:170`)、リトライを同時に外すとフロッピー起動が retry ループから抜けなくなる (MEMORY の `$FF9058` と同じ場所を踏む)。外すのは `updateFdcInterruptLine` の呼び出しだけ。

**期待効果**: 28% のうち IoSc/FDC 分。単独で実機計測でき、以降の投資判断に使える。

**検証**: フロッピー起動が通ること。

### 段 3: `debt_` の配線を RTC だけで入れる

RTC は期限が 10,000,000 サイクルと最長で、**無効化の契約が最も軽い** — `Rtc::write` も `setDateTime` も期限を変えないと確定済み (`rtc.cpp:51-64`)。`Scheduler` / `Settled` / `sliceEnd_` / `reachSlow` の合流点をここで実機確認する。

**期待効果**: 小さい (RTC の tick は `rtc.h:77-87` で既にインライン早期リターン)。**目的は機構の検証。**

**壊れたときの症状**: 時計が止まる。分かりやすい。

### 段 4: MFP を「次のタイムアウト」でイベント化 + lazy materialization

TBDR/TCDR の読み出しで実体化する (§3 の 1-4)。**IERA bit0 = 0 という動的な事実に依存する**ので、IER 書き込みで期限を張り直す配線 (§4 の #5) とセットでしか入れられない。

**期待効果**: `tickDevices` の 59% のうち MFP 分。CRTC は残るので全部は消えない。

**検証**: `kMinDeadline` 縮退が効くことを、IERA bit0 を立てるテストで確かめる (§8 の T7)。

### 段 5: `serviceInterrupts` を wake 型 + 保留中フォールバックへ

最大の単一項目 (28%)。wake 契機 22 項目を配線する。**この段だけは効果が出ても正しさが確認できるまでマージしない。**

**実機で通す 3 つ**: キー入力 / マウス (連続ドラッグ) / フロッピー起動。

**期待効果**: 28% のうちハンドラ外の分。ハンドラ内はフォールバックで現行速度に縮退するので、**実効は 28% より小さい**。ここで実測して段 7 の要否を決める。

### 段 6: 内訳の再々測

イベント駆動を入れた後は内訳が変わる。「古い実測値は、それが測られた条件が変わったら無効になる」を一度踏んでいる (quantum ありの上限 10000 kHz が撤回後 5161 kHz だった)。**測り直さないと次の判断を誤る。**

### 段 7 (任意): `setSr` callback と STOP フックを上乗せ

段 5 の実測でハンドラ内の縮退が支配的だった場合のみ。`m68k.cpp:73-98` に「IPL が実際に下がったときだけ」の callback を足し、`m68k_ops_group4.cpp:167` (STOP) に `debt_ = 0` を足す。**正しさの前提ではないので、効果が出なければ捨てる。**

---

## 8. 必要なテスト (変異テストで検出できることを条件に)

各テストに「これを消す/壊すと落ちる変異」を明記する。落ちない変異が書けるテストは価値が無い。

### T1: 遅延中に TBDR を読むと実体化される

```cpp
TEST_CASE("イベント駆動でも TBDR の読み出しはサイクル単位で正しい")
{
    // タイマ B (分周 4, データ 13) を IERA bit0 = 0 のまま動かす。
    // 実測設定と同じにして「割り込みは上げないがカウンタは読める」を再現する。
    // 8 サイクルごとに 1 減るはずの値を、期限を跨がない位置で読む。
    // 毎命令 tick 版と 1 サイクルも違わないことを比べる。
}
```

**検出できる変異**: `Mfp::read` の `Settled` 要求を消す / `settle()` の中身を空にする / `kTimerDataRegs` のループ (`mfp.cpp:66-73`) から `kTbdr` を外す。

**なぜ必要**: `test_device_timing.cpp` の既存 3 本は `tickFast` を直接叩くのでイベント駆動の `read` 経路を通らない。

### T2: TCDCR の書き換えで溜まった時間が旧分周で消化される

```cpp
TEST_CASE("タイマ制御の書き換えは遅延中の時間を旧設定へ適用してから効く")
{
    // TCDR=$C8 で 199 回ぶん進んだ状態から TCDCR を分周 200 → 4 へ書き替える。
    // 79,600 サイクルが新分周で消化されると偽の割り込みが連発する。
    // 毎命令 tick 版と IPRB / TCDR が一致することを確かめる。
}
```

**検出できる変異**: `Mfp::write` から `Settled` 要求を消す / `settle` → `write` の順序を入れ替える / `Rearm` のデストラクタを空にする。

### T3: マスク中に立った保留が SR 低下後に配送される

```cpp
TEST_CASE("マスク中に立った割り込みがマスク解除後に配送される")
{
    // SR=$2700 でマスクした状態でタイマ B をタイムアウトさせる。
    // serviceMfpInterrupt は acknowledge せず false を返す (machine.cpp:406-411)。
    // その後 RTE 相当で SR を下げ、80,000 サイクル以内に受理されることを確かめる。
    // 受理は SSP の減少と interruptMask() == 6 で外から見える
    // (test_device_timing.cpp:232-234 と同じ形)。
}
```

**検出できる変異**: **`reachSlow` の保留中フォールバックを消して無条件に再 arm する** ← これが変更 1 の中核。`setSr` callback だけの実装ではこの変異で落ちない (フォールバックが無いと `setSr` 漏れが即座に露見する形にできない) ため、**フォールバックの存在そのものを固定する唯一のテスト**。

### T4: SCC の FIFO に残りがあれば読み出し直後に再度割り込む

```cpp
TEST_CASE("マウスレポート 3 バイトが 3 回の割り込みで届く")
{
    // moveMouse で 3 バイト積み、1 バイト読むごとに rxInterruptPending が
    // 再度立ち、次の命令境界で配送されることを確かめる。
    // 割り込み回数が 3 であることを数える。
}
```

**検出できる変異**: `machine.cpp:665-666` の `sccRead` から wake を外す / `scc.cpp:336` の `refreshRxInterrupt` を消す。

**なぜ必要**: `scc.cpp:389-394` のコメントが警告する壊れ方 (カーソルが最初のレポート後に凍る) を自動で捕まえる手段が現在無い。

### T5: IPL を変えるのは `setSr` だけ (不変条件の固定)

```cpp
TEST_CASE("SR の割り込みマスクを変える経路は setSr に集約されている")
{
    // RTE / STOP / MOVE to SR / ANDI-ORI-EORI to SR の 4 命令を実行し、
    // setSr を通った回数を数える。RTR は CCR のみなので通らないことも確かめる。
    // (M68k にテスト専用のカウンタを 1 つ足す)
}
```

**検出できる変異**: `m68k_ops_group4.cpp:114` の RTE を `st_.sr = sr` の直代入へ変える ← **静かに割り込みを殺す変異**を、段 7 を入れる前から捕まえられる。

**なぜ必要**: 段 7 の `setSr` callback はこの不変条件に乗る。フォールバックがあるので正しさは守られるが、**性能が静かに退行する**のを検出する。

### T6: `Settled` は偽造できない (コンパイル失敗の固定)

```cpp
// static_assert ではなく、ビルドシステムで「コンパイルが失敗すること」を確かめる。
// GCC 14.2 は private な defaulted default ctor に対して m.read({}, reg) を
// 通してしまう (実測で確認済み)。Settled() {} と user-provided にすると
// 'Settled::Settled()' is private within this context で落ちる。
static_assert(!std::is_default_constructible_v<x68k::Settled>,
              "Settled() は user-provided でなければ {} で偽造できる");
```

**検出できる変異**: `Settled() {}` を `Settled() = default;` へ変える ← **この設計で最も静かな失敗モード**。型で守ったつもりが守れていない状態がコンパイル成功のせいで気づかれない。`std::is_default_constructible_v` は `= default` + private で `false` にならない可能性があるので、**実際にコンパイル失敗を確かめるテスト** (CMake の `try_compile` か、意図的に失敗するファイルを `SHOULD_FAIL` で登録) を併用する。

### T7: IERA bit0 が立っても現行より遅くならない

```cpp
TEST_CASE("タイマ B の割り込みを有効化しても期限機構が破綻しない")
{
    // IERA bit0 を立てて期限を 8 サイクルへ縮め、kMinDeadline 縮退が
    // 効くことを確かめる。状態が毎命令 tick 版と一致することが主眼で、
    // 速度は実機で測る。
}
```

**検出できる変異**: `kMinDeadline` の判定 (`delta < kMinDeadline`) を消す。

### T8: halt しても未実体化時間が失われない

```cpp
TEST_CASE("CPU が halt してもデバイスの時間が失われない")
{
    // 不正命令で halt させ、RTC の cycleAccumulator_ が
    // 毎命令 tick 版と一致することを確かめる。
}
```

**検出できる変異**: ホットパスの `used == 0` 経路から `settleAndStop` を消す。

### T9: STOP 中もデバイスが進み、割り込みで抜ける

```cpp
TEST_CASE("STOP 中もタイマが進み、割り込みで抜ける")
{
    // STOP #$2000 を実行し、タイマ C のタイムアウトで抜けることを確かめる。
    // 抜けるまでのサイクル数が毎命令 tick 版と一致すること。
}
```

**検出できる変異**: `reachSlow` の STOP 一括飛ばしで `Crtc::tickFast` にフレーム超のサイクルを渡す (エッジが消える) / STOP 中に時間を進めない実装にする (デッドロック)。

### 既存テストへの追加

`test_device_timing.cpp:243` の「最適化スイッチの両側で状態が一致する」に **イベント駆動の ON/OFF を第 4 の軸として足す**。比較対象 (`:319-328`) はそのまま使える — `rasterNumber()` (`:324`) が含まれているので、**CRTC をイベント化したら即座に落ちる**。これは変更 4 の判断を守る安全網として機能する。

---

## 9. 実装を始める前に確かめること

1. **CRTC 単独の寄与 (段 0)。** 変更 4 (CRTC をイベント化しない) はこの数字で決まる。20 ns/cycle 未満なら確定、それ以上なら設計を見直す。**これを測らずに実装を始めない。**

2. **`Settled() = default` と `Settled() {}` の違いを Xtensa GCC 14.2.0 で実際に確かめる。** 攻撃レポートが実測で確認したと主張しているが、この 1 点が設計の型安全性の全体を支える。`xtensa-esp-elf-g++ 14.2.0 -std=c++17` で両方コンパイルし、`= default` 側が通ることを自分の目で見る。通らないなら T6 の形を変える必要がある。

3. **`debt_` を `perf_` 直後に置いたときの実オフセットと生成コード。** `machine.h:48` の多相基底 3 つで vtable がどこに入るか、`l32i.n` が効くか (offset ≤ 60) を `objdump` で確認する。効かないなら配置の設計判断を捨てる (4 命令構成は変わらないので致命的ではない)。

4. **`Mfp::hasPendingInterrupt` が `reachSlow` の中で呼ばれても十分速いか。** 現在は毎命令インライン (`mfp.h:256-264`) で、`IPR & IMR` が 0 なら即 return する。`reachSlow` へ移すと `call8` の中に入る。保留中フォールバックが働く区間 (ハンドラ内) ではこれが毎命令になるので、**現行のインライン版より遅くならないこと**を確かめる。遅いなら `hasPendingInterrupt` の早期リターンだけホットパスに残す設計変更が要る — ただしそれは分岐 1 本の追加になるので、規律との兼ね合いを段 5 で判断する。

5. **`reachSlow` の中で `Crtc::tickFast` へ渡すサイクル数が `kCyclesPerFrame` を超えないこと。** `video.h:54-58` のコメントどおり、超えると垂直帰線の開始/終了が**丸ごと消える**。RTC の期限 (10,000,000) だけを見て飛ばすと即座に踏む。CRTC を毎命令 tick に残す設計 (変更 4) ではそもそも `tickFast` に大きな値が渡らないが、**STOP の一括飛ばし (変更 3) だけは例外**。ここで必ず `kCyclesPerFrame` 未満に刻む。

6. **ホストの `'@'` ダンプが `peek()` を使っている箇所。** `host/main.cpp:1102-1111` と `main/main.cpp:1036-1041` を確認し、タイマデータレジスタは `read()` 側へ寄せる。でないと実体化が入った後に古い値を出し、**実機のゲスト設定を読み違える** — 段 4 の前提 (IERA bit0 = 0) を確認する道具そのものが壊れる。

7. **`kMinDeadline` の値 (256 サイクル) の妥当性。** `reachSlow` の `call8` + 期限再計算のコストを実測し、それを下回る期限では縮退させる。推測で決めず、段 3 の機構が動いてから測って決める。

---

## 補足: 主要ファイルの絶対パス

- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/src/x68k/core/machine.cpp`
- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/src/x68k/core/machine.h`
- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/src/x68k/core/dev/mfp.h` / `mfp.cpp`
- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/src/x68k/core/dev/video.h` / `video.cpp`
- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/src/x68k/core/dev/rtc.h` / `rtc.cpp`
- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/src/x68k/core/dev/scc.cpp` / `iosc.h` / `fdc.cpp` / `dmac.cpp`
- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/src/x68k/core/cpu/m68k.cpp` / `m68k_ops_group4.cpp` / `m68k_ops_misc.cpp`
- `/Users/kei/ghq/github.com/kexi/x68k-stackchan/test/test_device_timing.cpp`
- 新規: `src/x68k/core/scheduler.h` (`Scheduler` / `Settled` / `Rearm`)

**設計の 1 行要約**: 負数 `debt_` で毎命令をゼロ比較 1 本に保ちつつ、**割り込みの正しさは wake の網羅性でなく「保留中は毎命令リトライへ縮退する」フォールバックで守り**、CRTC は `rasterNumber` の 317 サイクル粒度ゆえイベント化しない。",
    "fatalCount": 0,
    "attacks": 3
  },
  "workflowProgress": [
    {
      "type": "workflow_phase",
      "index": 1,
      "title": "Survey"
    },
    {
      "type": "workflow_phase",
      "index": 2,
      "title": "Design"
    },
    {
      "type": "workflow_phase",
      "index": 3,
      "title": "Adversarial"
    },
    {
      "type": "workflow_phase",
      "index": 4,
      "title": "Synthesize"
    },
    {
      "type": "workflow_agent",
      "index": 1,
      "label": "survey:observability",
      "phaseIndex": 1,
      "phaseTitle": "Survey",
      "agentId": "ad66af0a1061b3a0c",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786962521821,
      "queuedAt": 1786962521811,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786962715203,
      "tokens": 114198,
      "toolCalls": 38,
      "durationMs": 193382,
      "resultPreview": "{"findings":[{"area":"MFP タイマデータレジスタ TADR/TBDR/TCDR/TDDR ($E8801F/$E88021/$E88023/$E88025)","detail":"Mfp::read (src/x68k/core/dev/mfp.cpp:66-73) はこの 4 本だけ reg_ ではなく timerValue_[i] を返す。timerValue_ を減らすのは Mfp::tickFast (src/x68k/core/dev/mfp.h:150-182) と Mfp::tickTimerCounted (mfp.cpp:284-326) だけ。変化の粒度は `プリスケール値 x 2 CPU サイクル` (kCpuToMfpShift=1, mfp.h:120)。実測設定では タイマ B が分周 4 => **8 CPU サイクルごとに 1 減る*…"
    },
    {
      "type": "workflow_agent",
      "index": 2,
      "label": "survey:invalidation",
      "phaseIndex": 1,
      "phaseTitle": "Survey",
      "agentId": "a36ace4e2e44ffde3",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786962521821,
      "queuedAt": 1786962521812,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786962755142,
      "tokens": 102654,
      "toolCalls": 25,
      "durationMs": 233321,
      "resultPreview": "{"findings":[{"area":"構造的前提: 書き込みは「過去の時点」で起きる (machine.cpp:235-253)","detail":"runWith は `serviceInterrupts(); used = cpu_.step(); tickDevices(used);` の順で回る (machine.cpp:240-249)。つまり **命令が実行された時点ではデバイスの時間はまだ進んでいない**。イベント駆動で期限まで飛ばすと、この「未適用の時間」が最大で期限ぶん (タイマ C なら 80,000 サイクル) 溜まる。この状態で ioWrite8 → mfp_.write が走ると、書き込みは「溜まった時間を旧設定で消化する前」に適用されることになる。したがって全ての無効化経路は **settle(遅延中の時間を旧設定へ適用) → 書き込みを適用 → 期限を…"
    },
    {
      "type": "workflow_agent",
      "index": 3,
      "label": "survey:interrupts",
      "phaseIndex": 1,
      "phaseTitle": "Survey",
      "agentId": "aebac904ca8f0f00f",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786962521822,
      "queuedAt": 1786962521812,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786962778202,
      "tokens": 113731,
      "toolCalls": 44,
      "durationMs": 256380,
      "resultPreview": "{"findings":[{"area":"結論: serviceInterrupts はイベント駆動化できるが、wake の契機は「デバイスが進んだとき」だけでは全く足りない","detail":"毎命令の判定は machine.h:311-321 の 3 項 OR (mfp_/scc_/iosc_.hasPendingInterrupt) と machine.h:340-343 の updateFdcInterruptLine の 2 つ。ゲート自体は 2 段構えになっている。第 1 ゲート = 各デバイスの「保留があるか」、第 2 ゲート = machine.cpp:371/406/440 の cpu_.state().interruptMask() 比較。さらに第 3 のラッチが CPU 内にある: M68k::requestInterrupt (m68k.cpp:590-597)…"
    },
    {
      "type": "workflow_agent",
      "index": 4,
      "label": "design:countdown",
      "phaseIndex": 2,
      "phaseTitle": "Design",
      "agentId": "a96bbfad7ef853c17",
      "model": "claude-opus-5[1m]",
      "state": "error",
      "startedAt": 1786962778218,
      "queuedAt": 1786962778207,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786963619364,
      "error": "agent({schema}): StructuredOutput retry cap (5) exceeded — 5 failed calls with no valid output",
      "tokens": 91491,
      "toolCalls": 19,
      "durationMs": 841145
    },
    {
      "type": "workflow_agent",
      "index": 5,
      "label": "design:deadline-cache",
      "phaseIndex": 2,
      "phaseTitle": "Design",
      "agentId": "a6f1a525c133c890c",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786962778219,
      "queuedAt": 1786962778208,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "## 全体像 デバイス (MFP タイマ x4 / CRTC / RTC) ごとに「次…",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786963432441,
      "tokens": 98239,
      "toolCalls": 29,
      "durationMs": 654222,
      "resultPreview": "{"approach":"## 全体像\
\
デバイス (MFP タイマ x4 / CRTC / RTC) ごとに「次に状態が変わるまでの絶対サイクル」を個別に持つ。ただし**毎命令のホットパスはその表を一切読まない**。最小値を 1 個の `std::int32_t debt_` に負数として畳み、毎命令は `debt_ += used; if (debt_ >= 0) slow();` だけ。\
\
個別に持つ利得は速度でなく**無効化の局所性**。TCDCR が書かれたとき CRTC/RTC の期限は再計算不要で、MFP の 4 本だけ引き直して最小値を取り直せばよい。単一期限だと「何が最小だったか」を失うので、全部引き直すか、保守的に 0 へ落として次命令で全デバイスを settle することになる。\
\
## Xtensa GCC 14.2.0 (-O2 -mlongcalls…"
    },
    {
      "type": "workflow_agent",
      "index": 6,
      "label": "design:fused",
      "phaseIndex": 2,
      "phaseTitle": "Design",
      "agentId": "af6ab198af78cf779",
      "model": "claude-opus-5[1m]",
      "state": "error",
      "startedAt": 1786962778219,
      "queuedAt": 1786962778208,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786963481054,
      "error": "agent({schema}): StructuredOutput retry cap (5) exceeded — 5 failed calls with no valid output",
      "tokens": 93991,
      "toolCalls": 23,
      "durationMs": 702834
    },
    {
      "type": "workflow_agent",
      "index": 7,
      "label": "attack:deadline-cache:timing",
      "phaseIndex": 3,
      "phaseTitle": "Adversarial",
      "agentId": "af18782c4cd7a2bb4",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786963619383,
      "queuedAt": 1786963619368,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786963801743,
      "tokens": 72501,
      "toolCalls": 23,
      "durationMs": 182360,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"IMRB=0 (垂直帰線割り込みマスク) の状態で `btst #4,$E88001` / `bne.s` の GPIP4 ポーリングループを回す。設計の期限表は crtcEdge を持つが、invalidation セクションが「トークンは Mfp::read / Rtc::read の側に置く」と定めているため、Mfp::read(Settled, kGpip) の Settled は「MFP が実体化済み」の証明でしかなく、CRTC を進める義務が型に現れない。タイマ C の期限 80,000 サイクルまで飛ぶと、その間に来るはずの V-DISP 立ち下がりが遅れて観測され、ループの反復回数が変わる。","why":"mfp.cpp:271-282 の raise() は `if ((reg_[i…"
    },
    {
      "type": "workflow_agent",
      "index": 8,
      "label": "attack:deadline-cache:interrupt-loss",
      "phaseIndex": 3,
      "phaseTitle": "Adversarial",
      "agentId": "afe59ba6565db184b",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786963619386,
      "queuedAt": 1786963619368,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786963828985,
      "tokens": 87382,
      "toolCalls": 28,
      "durationMs": 209599,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"MFP タイマ C (IERB=26, 分周 200/データ 200 = 80,000 サイクル周期) が、CPU が既にレベル 6 ハンドラ内 (m68k.cpp:647 の setSr で IPL=6) にいる瞬間に raise される。mfp.cpp:271 の raise() が IPR を立て、期限機構が debt_ = 0 を書く。次命令で reachSlow() → serviceMfpInterrupt() (machine.cpp:390) に入るが、machine.cpp:412-415 の `const u32 mask = cpu_.state().interruptMask(); if (kMfpInterruptLevel <= mask) return false;` で …"
    },
    {
      "type": "workflow_agent",
      "index": 9,
      "label": "attack:deadline-cache:perf",
      "phaseIndex": 3,
      "phaseTitle": "Adversarial",
      "agentId": "aeb82b4252f690a09",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786963619386,
      "queuedAt": 1786963619368,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786963824389,
      "tokens": 64955,
      "toolCalls": 23,
      "durationMs": 205003,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"Human68k のアイドル (STOP 中)。m68k.cpp:651-654 は `if (st_.stopped) { return 4; }` で、**0 ではなく 4 を返す**。提案のホットループは `used == 0` でしか抜けないので、STOP 中も `call8 M68k::step()` → 4 を返す、を延々繰り返す。タイマ C の期限 80,000 サイクルまで飛ぶには call8 を 20,000 回実行する。各回 step() 冒頭で st_.halted / pendingIrq_ / st_.stopped の 3 ロード + 3 分岐を通る (m68k.cpp:610-651)。","why":"設計は risks で「STOP 中はゲストが何もポーリングできない唯…"
    },
    {
      "type": "workflow_agent",
      "index": 10,
      "label": "synthesize",
      "phaseIndex": 4,
      "phaseTitle": "Synthesize",
      "agentId": "acb648c4cd2801aed",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786963828986,
      "queuedAt": 1786963828985,
      "attempt": 1,
      "lastToolName": "Write",
      "lastToolSummary": "/private/tmp/claude-501/-Users-kei-ghq-github-com-kexi-x68k…",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz)。実効 5137 kHz (実機比 51%)。

## 目的
命令ごとに Machine::tickDevices を呼ぶのをやめ、「次にデバイスの状態が変わる時点」まで
飛ばす (イベント駆動)。quantum と違い、状態が変わる瞬間は 1 サイクルもずらさない。

## 実測 (CoreS3 実機、Human68k 稼働中)
命令の実行を空回しにして段階的に外した内訳:
- tickDevices        118.76 ns/cycle (59%)
- serviceInterrupts   56.25 ns/cycle (28%)
- ループ運営          18.…",
      "lastProgressAt": 1786964147513,
      "tokens": 123268,
      "toolCalls": 14,
      "durationMs": 318527,
      "resultPreview": "I verified every load-bearing claim in the attack reports against the source. Here is the consolidated design.

---

# イベント駆動デバイス — 実装確定版

## 実装結果 (2026-08-17、CoreS3 実機、Human68k 稼働中)

**実装して実機で測った。同じ起動の中で `'$'` を切り替え、ON → OFF → ON で再現。**

| | 実効クロック | 実機比 |
|---|---|---|
| OFF | 4991 / 4989 kHz | 49.9% |
| **ON** | **7579 kHz** | **75.8%** |

**+51.9%。** 試算は 12655 kHz だったので、届いたのは 6 割ほど。残差の
原因は `serviceInterrupts` を毎命令のまま残していること (28% ぶん) と、
縮退 (`degraded_`) に落ちる区間があること。次段で詰める。

検証: ホストテスト 559 件 / 226,521 assertions、状態が完全に不変
(400M サイクルで 99998982 命令 / `$ED0000=82773638`)、Human68k が
`A>` まで起動して `dir` が動く。

## 0. 事前検証で確定した事実 (推測でなく実コードから)

攻撃レポートの主張のうち、設計を変える 5 件を実コードで確認した。

| 主張 | 確認 | 実体 |
|---|---|---|
| STOP は 0 でなく 4 を返す | **正しい** | `m68k.cpp:651-655` `if (st_.stopped) { return 4; }`。0 を返すのは `m68k.cpp:612` の halted のみ |
| `rasterNumber()` が同値テストの比較対象 | **正しい*…"
    }
  ],
  