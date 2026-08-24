// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ネイティブ実行器を CPU へ教わらせるための口。core/ は「どう走らせるか」を持たない。
//
// core/ が作るのは BlockPlan という**データ**までで、それを解釈するのも
// 機械語へ落とすのも外側の仕事にする。段 1 の参照実行器 (PlanInterpreter) も
// 段 2 の Xtensa エミッタも、同じ NativeExec を満たす別実装として
// platform/jit/ に並び、main/ が差し替える。setResetCallback / setFastRam と
// 同じ「教わる」流儀で、virtual も std::function も使わない。

#ifndef X68K_CORE_CPU_NATIVE_EXEC_H
#define X68K_CORE_CPU_NATIVE_EXEC_H

#include "m68k_types.h"

namespace x68k
{

class M68k;

// ネイティブ実行器が「なぜ戻ってきたか」。
//
// Why not 単一の u32 (0 = 実行できなかった) にしないか: 0 が次の 3 つを
// 兼ねてしまい、呼び出し側が区別できない。
//   (a) 未対応命令の手前で戻った  → step() を 1 回回して続ける
//   (b) CPU が halted / stopped だった → step() が停止として処理する
//   (c) 割り込みの受理が要る      → step() が例外を積む
// 区別しないと、halt を「未対応」と読み違えて step() を無限に呼ぶか、
// 割り込みを飲み込んだまま走り続ける。どちらも実際に起こる形。
enum class NativeExit : u8
{
    // 1 ブロック以上を実行した。cycles が有効。
    kRan = 0,

    // 1 命令も実行していない。呼び出し側は step() を 1 回回す。
    //
    // (a)(b)(c) を全部これで表す。step() は 3 つとも既に正しく処理する
    // (halted → 0、割り込み → 受理、stopped → 4) ので、実行器が理由を
    // 区別する必要はない。区別が要るのは NativeStats の内訳だけ。
    kDeferToStep,
};

struct NativeResult
{
    u32 cycles;  // kRan のときだけ有効
    NativeExit exit;
};

// ネイティブ実行器の口。
//
// 事前条件 (呼び出し側が保証する):
//   - cpu の状態が命令境界にある。次に実行する命令語のアドレスを X として
//     pc == X + 4 / ir == mem16(X) / irc == mem16(X + 2)
//
// 事後条件 (実行器が保証する):
//   - kRan を返したなら、M68kState は「最後の命令をインタプリタで実行し
//     終えた直後」とビット単位で同一。pc / ir / irc の書き戻しを含む
//   - kDeferToStep を返したなら、**状態を 1 ビットも変えていない**
//   - 例外に入らない。入りうる命令はブロックへ入れない
//   - 割り込みを自分で配送しない。呼び出し側が step() 経由で見る
using NativeRunFn = NativeResult (*)(void* context, M68k& cpu);

// 実行器の内訳。速度そのものではなく、**どこで諦めたか**を数える。
//
// 段 0 の計測で「プロファイル上で大きいこと」と「そこを削れること」が
// 別だと繰り返し分かっているので、対応範囲を広げる判断は必ずこの内訳で行う。
struct NativeStats
{
    u64 blocksRun = 0;
    u64 insnsRun = 0;
    u64 deferHalted = 0;       // halted / stopped で降りた
    u64 deferInterrupt = 0;    // 割り込み保留で降りた
    u64 deferUnsupported = 0;  // 未対応命令で降りた
    u64 deferCapacity = 0;     // kMaxOps に達した
    // タグが違う (別の PC がこのスロットを使っていた)。
    u64 keyMissTag = 0;
    u64 keyMissEpoch = 0;  // 写像 epoch 不一致
    u64 keyMissGen = 0;    // ページ世代不一致
    u64 keyMissStale = 0;  // kAlwaysStale で捨てた
    // **世代が外れた後のバイト照合の結果** (段 F)。keyMissGen の内訳で、
    // verifyHit + verifyMiss == keyMissGen が常に成り立つ。
    //
    // verifyHit は偽共有 (同じページのデータが書かれただけで、命令語は
    // 変わっていない) を弾いた回数。翻訳し直さずに実行した。
    //
    // **「照合して違った」と「控えが無くて照合できなかった」を分ける。**
    // 1 本にまとめていたせいで、実機の 26.7% を「命令語が書き換わっている」
    // と読み違えた。実際は実差 0 件で、全部が控え無しだった。
    // 前者は本物の自己書き換え、後者は控えの語数が足りないことを意味し、
    // **打つ手がまったく違う**。
    u64 verifyHit = 0;
    u64 verifyMiss = 0;        // 照合して実際に違った
    u64 verifyNoSnapshot = 0;  // 控えが無いので照合しなかった
    // 照合の控えを持てなかった翻訳の本数。範囲が kMaxVerifyWords を超え、
    // **かつサイドテーブルにも逃がせなかった** (表が無い / 満杯 / 31 語でも
    // 足りない) か、読めない語が混ざった。**黙って被覆が減る形を作らない
    // ために数える**。
    u64 verifyTooLong = 0;
    // スロットに収まらずサイドテーブルへ逃がした翻訳の本数 (段 G)。
    //
    // **verifyTooLong と対になる。** 逃がせた本数と逃がせなかった本数を
    // 分けておかないと、表が満杯で静かに段 F へ縮退しても
    // 「控えを持てず」が増えるだけで**原因が表の容量だと分からない**。
    u64 verifySideUsed = 0;
    u64 translateFail = 0;
    // **コード領域が満杯で翻訳を試みもしなかった回数。**
    //
    // ここにカウンタが無かったせいで、実機 45 秒の「未対応 14,815,821」の
    // うち 99.997% が実は満杯だったことに気づけなかった。
    // deferUnsupported == negativeHit + translateFail + fullDeferred が
    // 成り立つ (収支が閉じる) ことをテストで縛る。
    u64 fullDeferred = 0;
    // 空きスロットを引いた回数 (コールドミス)。
    //
    // 鍵外れの帰属は slot->code != nullptr の中でしか数えていないので、
    // 空きスロットの取りこぼしはどのカウンタにも乗らなかった。
    u64 keyMissCold = 0;
    // 満杯から回復するために全部捨てた回数。
    u64 capacityReset = 0;
    // **翻訳に成功した回数と、そのバイト数の合計。**
    //
    // 商が平均ブロックサイズで、これが無いと「16KB に何本入るか」を
    // 推定でしか言えない (arena の適正値も admission の閾値も、
    // 常駐本数を根拠にしか決められない)。
    //
    // 再翻訳率も出る: translated が blocksRun に対して大きいなら、
    // 同じ番地を何度も翻訳し直している (チャーンの浪費)。
    u64 translated = 0;
    u64 translatedBytes = 0;
    // 世代が飽和して翻訳できなくなり、全部捨てて数え直した回数。
    u64 generationReset = 0;
    // 分岐で終端したブロックの実行回数 (direct chaining の的)。
    u64 endedWithBranch = 0;
    // 動的分岐 (RTS / JSR) で終端したブロックの実行回数 (Tier D)。
    u64 endedWithDynamicBranch = 0;
    // そのうち実際に飛んだ回数。
    //
    // **endedWithDynamicBranch との差がガード脱出の回数。** 差が大きいなら
    // RTS のスタックか JSR の飛び先が窓の外を指し続けている。その形は
    // 翻訳に成功してしまうので負のキャッシュが効かず、毎周
    // 「鍵照合 → 実行 → 不成立 → step()」を踏んで Tier C より遅くなりうる。
    u64 dynamicBranch = 0;
    // 「翻訳できない」と覚えていたので再翻訳を省いた回数。
    u64 negativeHit = 0;  // 入口の命令からして翻訳できなかった
    // 読みガードが不成立でブロックを降りた回数 (Tier B)。
    //
    // **blocksRun に対する比率を見る。** 高いままなら、(An) 経由の
    // I/O ポーリングがガードを空回りさせている。その形は翻訳に成功して
    // しまうので負のキャッシュが効かず、毎周「鍵照合 → 実行 → 不成立 →
    // step()」を踏んで Tier A より遅くなりうる。
    u64 guardExit = 0;
    // そのうち 1 命令も進めずに降りた回数 (G10)。
    u64 deferGuard = 0;
    // 自ページ書き換えでブロックを降りた回数 (Tier C の G13/G18)。
    //
    // **blocksRun に対する比率を見る。0.1% を超えたら要注意。**
    // この脱出は負のキャッシュへ「世代不問」を焼くので、その番地は
    // mappingEpoch が動くまで JIT を失う。正しさは壊れないが、
    // 一度きりの自己パッチ (起動時にコード近傍のフラグを書く類) でも
    // 同じ扱いになるので、諦めすぎている可能性がここにしか映らない。
    u64 selfPageExit = 0;
};
using NativeStatsFn = const NativeStats* (*)(void* context);

struct NativeExec
{
    NativeRunFn run = nullptr;
    NativeStatsFn stats = nullptr;
    void* context = nullptr;

    [[nodiscard]] bool isReady() const
    {
        return run != nullptr;
    }
};

}  // namespace x68k

#endif  // X68K_CORE_CPU_NATIVE_EXEC_H
