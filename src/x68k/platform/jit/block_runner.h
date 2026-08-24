// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ブロックキャッシュと実行器。core/ が教わる NativeExec の実体。
//
// ## 何をするか
//
// 現在の PC からブロックを 1 本探し、あれば生成コードを呼ぶ。無ければ
// 翻訳して置く。翻訳できなければインタプリタへ落とす。
//
// ## 鍵の照合 (設計 §5.5)
//
// **順序が意味を持つ。** kAlwaysStale の検査を世代の一致より先に置く。
// 逆順だと CodeGenMap 未配線時に 0xFFFF == 0xFFFF が成立して「常に有効」に
// なり、自己書き換えを一切検出しないブロックキャッシュができる。
//
// ## 割り込みの受理点 (設計 §5.4)
//
// 入口で M68k::mustDeferToStep() を見る。これが無いと、割り込みが立った後も
// ネイティブが成功し続ける限り step() が呼ばれず、次に reachSlow へ落ちる
// までの最大 80,000 サイクル受理が遅れる。

#ifndef X68K_PLATFORM_JIT_BLOCK_RUNNER_H
#define X68K_PLATFORM_JIT_BLOCK_RUNNER_H

#include <cstddef>
#include <cstdint>

#include "cpu/block_plan.h"
#include "cpu/m68k.h"
#include "cpu/native_exec.h"
#include "jit/block_emitter.h"
#include "jit/exec_memory.h"
#include "jit/negative_cache.h"

namespace x68k::jit
{

// 1 スロットが照合のために控える命令語の上限。
//
// **照合する範囲は entryPc から fallThroughPc + 4 まで** (出口の ir と irc を
// 含む)。ブロックの前提はこの範囲のバイト列そのものなので、1 語でも短く
// 控えると「控えた範囲は同じだが、控えなかった語が変わっている」ブロックを
// 同一と判定する。それは古いコードを静かに実行し続ける形 = pull 型の失敗形。
//
// I4 が entryPc から fallThroughPc + 2 までを同一 1KB ページに閉じているので
// 範囲は連続で有界。ただし kMaxOps = 6 に最長命令 (10 バイト) が並ぶ最悪では
// 31 語に達し、控えるだけで 62 バイト要る。実測の平均は 4.40 語なので、
// **最悪に合わせて全スロットを太らせるのは割に合わない**。
//
// **8 語に決めた理由は「ここまでなら 1 スロット 40 バイトのまま」。**
// page (4 バイト) を落とし、bool 2 個をビットへ畳んだぶんがちょうど
// 16 バイトあり、8 語 = 16 バイトがそこへ収まる。**スロットを太らせずに
// 照合を足せる**ので、2048 スロットが 80KB (= 従来 512 スロットの 4 倍、
// 実測 largest free block 90KB の内側) に収まる。
//
// 上限を超えるブロックは控えを持たず (verifyWords = 0)、世代が動いたら
// 照合せずに翻訳し直す。**保守的な側へ倒れる**ので正しさは損なわれない。
// 収まらなかった本数は stats().verifyTooLong で観測できる (黙って被覆が
// 減る形を作らない)。
//
// Why not ハッシュにしないか: 偽陽性が「違うのに同じ」= 古いコードを静かに
// 実行し続ける形で、code_gen_map.h が push 型を棄却した論理
// (「症状が原因から遠い」) がそのまま当てはまる。語をそのまま持てば
// 偽陽性は原理的に存在しない。8 語 16 バイトの比較は、翻訳 1 本
// (平均 239 バイト発行) に比べて 2 桁安い。
//
// **9 語以上のブロックはサイドテーブルへ逃がす (段 G)。** スロットの中に
// 収まらないだけで、控えを持てないわけではない。下の VerifySide を見よ。
inline constexpr std::uint32_t kMaxVerifyWords = 8;

// サイドテーブル 1 件が控える命令語の上限。
//
// **kMaxOps = 6 の最悪 (最長命令 10 バイトが 6 本 + 出口の ir/irc) が
// 31 語**なので、そこまで持てば「長すぎて控えられない」形は原理的に
// 無くなる。1 件 = 31 語 x 2 + 索引 8 バイト = 70 バイト。
//
// Why not スロット側を 16 語へ広げないか: BlockSlot が 40 → 56 バイトになり、
// 2048 スロットが 80KB → 112KB で実測の largest free block 90KB を超える。
// スロットを 1024 へ半減させることになり、**段 F が実測で棄却した
// 「片方だけ当たる」形**に戻る (照合とスロット増は掛け算で効き、
// 片方ずつ +1.9% / +2.5% に対し両方で +5.6%)。長い側は実測 176 本しか
// 無いので、**全スロットを太らせるのではなく、その 176 本だけを逃がす**。
inline constexpr std::uint32_t kMaxSideVerifyWords = 31;

// スロットが「サイドテーブルを持たない」ことを表す索引。
//
// **0 を番兵にしない。** 0 は正当な索引なので、ゼロ初期化された
// BlockSlot が 0 番の控えを指してしまう (`BlockSlot{}` は reset() と
// スロット追い出しの両方で作られる)。
inline constexpr std::uint16_t kNoVerifySide = 0xFFFFu;

// スロットに収まらない控えの置き場 1 件。
//
// ## 寿命 —— 「解放済みの控えを読む」が原理的に存在しない形
//
// **個別の解放をしない。** 表はバンプアロケータ (ExecMemory と同じ規律) で、
// 空きへ戻る唯一の経路は BlockRunner::reset() の全捨てだけ。つまり
// 「解放してから読む」という順序自体が作れない。
//
// では**スロットが別の番地に上書きされたとき**に古い控えが読まれないのは
// なぜか。索引を返す側ではなく、**控えの側に持ち主の番地 (ownerPc) を
// 持たせて、引くたびに突き合わせる**からである。上書きされたスロットは
// entryPc が変わるので、古い索引をそのまま持っていても ownerPc と一致せず
// 「控え無し」に落ちる (= 現行と同じ保守的な側)。
//
// Why not 参照カウントや free list にしないか: どちらも「解放し忘れ」と
// 「早すぎる解放」の 2 つの失敗形を持ち込む。前者は表が埋まって縮退する
// だけだが、**後者は他人の控えを自分のものとして読む** = 「違うのに同じ」で
// 古いコードを静かに実行し続ける形になり、code_gen_map.h が push 型を
// 棄却した論理がそのまま当てはまる。持ち主を控えに書いておけば、
// 取り違えは**照合の前に必ず落ちる**。
struct VerifySide
{
    // この控えの持ち主の entryPc。**0 なら未使用。**
    // スロットの entryPc と一致しなければ、この控えは読まない。
    std::uint32_t ownerPc = 0;
    // 持ち主の写像世代。写像が変われば読めた語の意味も変わる。
    std::uint32_t mappingEpoch = 0;
    std::uint16_t words = 0;  // 実際に控えた語数
    std::uint16_t verify[kMaxSideVerifyWords] = {};
};

// キャッシュの 1 スロット。
//
// **BlockPlan をまるごと持たない。** 実行時に要るのは鍵と生成コードの
// 位置とサイクル数だけで、ops[] (20 バイト x 4 = 80 バイト) は翻訳の
// 途中でしか使わない。落とすと 1 スロット 120 → 40 バイトになり、
// 同じ内部 SRAM で **3 倍のスロット**が置ける。
//
// タグ外れが実行の 21% あったので、ここが効く。
//
// ## page を持たない
//
// 世代を引く番地は entryPc から導ける (entryPc >> kPageShift)。I4 が
// ブロック全体を 1 ページに閉じているので、**先頭バイトのページ = ブロックの
// ページ**であり、別に持つ意味が無い。4 バイト削るとスロットが 2048 個でも
// 内部 SRAM に収まる。
// **欄の並びが容量を決める。** 4 バイト欄をまとめて先頭へ、2 バイト欄、
// 1 バイト欄の順に置くと詰め物が 1 バイトも出ず、ESP32-S3 (ポインタ 4 バイト)
// で ちょうど 40 バイトに収まる。意味で並べ替えると 44 バイトになり、
// 2048 スロットが 88KB へ膨らんで実測の largest free block 90KB に対して
// 余裕が 2KB しか残らない。**並びを変えるときは 2048 x sizeof を計算し直すこと。**
struct BlockSlot
{
    // --- 鍵 (§5.5 の順で照合する) ---
    std::uint32_t entryPc = 0;       // 0 は空きスロットの番兵
    std::uint32_t mappingEpoch = 0;  // 写像の世代
    // 分岐成立時の飛び先。**動的分岐では使わない** (メールボックスを見る)。
    std::uint32_t branchTarget = 0;
    // 生成コードのエントリポイント。nullptr なら未翻訳。
    const std::uint8_t* code = nullptr;

    std::uint16_t pageGen = 0;  // そのページの世代

    // --- 偽共有を弾くための控え (段 F) ---
    //
    // 世代だけが外れたとき、翻訳し直す前にこの語列を実メモリと照合する。
    // 同じなら pageGen を控え直してそのまま実行する。
    //
    // 実測で**世代が動いたブロックの 100.00% は命令語が 1 バイトも
    // 変わっていなかった** (1,153,764 件中 1,153,764 件)。自己書き換えは 0 件。
    //
    // 照合する語列は **entryPc から** 2 バイトずつ verifyWords 語。
    // 起点を別に持たないのは、範囲が必ず entryPc から始まるから。
    std::uint16_t verify[kMaxVerifyWords] = {};

    std::uint8_t count = 0;  // 命令数 (統計とゴミ検査に使う)
    // 控えた語数。0 なら控えを持たない (範囲が kMaxVerifyWords を超えたか、
    // 読めない語が混ざった) ので、世代が動いたら必ず翻訳し直す。
    std::uint8_t verifyWords = 0;
    // 分岐で終端したか (戻り値の bit31 を見てよいか)。
    bool endsWithBranch = false;
    // 動的分岐で終端したか (戻り値の bit29 を見てよいか、Tier D)。
    bool endsWithDynamicBranch = false;

    // --- 長すぎてスロットに収まらない控えの索引 (段 G) ---
    //
    // kNoVerifySide なら持たない。**この欄はスロットを 1 バイトも太らせない**:
    // 上の欄を並べた時点で 38 バイトあり、4 バイト整列のために 2 バイトの
    // 詰め物ができていた。そこへちょうど収まる (下の static_assert が
    // 40 バイトのままであることを縛る)。
    //
    // **索引だけでは古い控えを指しうる** (スロットが別の番地に上書きされても
    // 索引は残る)。指した先の ownerPc と突き合わせて初めて有効になる
    // —— 寿命の設計は VerifySide の冒頭にある。
    std::uint16_t verifySide = kNoVerifySide;
};

// **スロットの大きさを固定する。** ポインタ 4 バイトの実機 (ESP32-S3) で
// 40 バイトに収まることが 2048 スロット = 80KB の前提で、これを超えると
// 内部 SRAM の largest free block (実測 90KB) に対する余裕が消える。
//
// 欄を足したり並べ替えたりして 40 を超えたら、ここで**ビルドが落ちる**。
// 黙って容量が減って「なぜか遅くなった」を追う羽目にならないようにする。
// ホスト (ポインタ 8 バイト) では 4 バイト増えるので、そちらは 48 で見る。
//
// **段 G で「以下」ではなく「ちょうど」に締めた。** サイドテーブルの索引は
// 既存の詰め物へ入れる約束で足したので、40 を下回ることも上回ることも
// 「詰め物の勘定が変わった」ことを意味する。上限だけを縛っていると、
// 詰め物を食い潰した次の 1 バイトで黙って 44 へ跳ねる。
static_assert(sizeof(BlockSlot) == (sizeof(void*) == 4 ? 40u : 48u),
              "BlockSlot が太ると 2048 スロットが内部 SRAM に収まらない");

// ブロックキャッシュ。
//
// **内部 SRAM に置く。** ブロックの入口で毎回引くので、PSRAM への
// 散らばったアクセスは避ける (実機でエミュレーションを止めた実績がある)。
class BlockRunner
{
public:
    // スロットと実行可能メモリを教わる。どちらも外が確保する
    // (core/ と同じ「外から教わる」流儀)。
    void setStorage(BlockSlot* slots, std::uint32_t slotCount, ExecMemory* code)
    {
        slots_ = slots;
        slotCount_ = slots != nullptr ? slotCount : 0;
        code_ = code;
    }

    // 「翻訳できない」を覚える表を教わる。**成功ブロックとは別に持つ**
    // (negative_cache.h の冒頭に理由がある)。教わらなくても動く。
    void setNegativeStorage(NegEntry* entries, std::uint32_t count)
    {
        neg_.setStorage(entries, count);
    }

    // スロットに収まらない控えの置き場を教わる (段 G)。
    //
    // **教わらなくても動く。** 内部 SRAM は逼迫しているので確保に失敗しうる。
    // その場合は長いブロックが控えを持たないだけで、段 F と同じ動作
    // (世代が動いたら翻訳し直す) に縮退する。**JIT ごと死なせない**
    // (段 0-I で 8192 エントリの確保に失敗したときと同じ流儀)。
    void setVerifySideStorage(VerifySide* entries, std::uint32_t count)
    {
        sides_ = entries;
        sideCount_ = entries != nullptr ? count : 0;
        // **索引が表より外を指す状態を残さない。** 表を差し替えたら、
        // 前の表を指していた索引は全部無効になる。
        sideUsed_ = 0;
        for (std::uint32_t i = 0; i < slotCount_; ++i)
        {
            slots_[i].verifySide = kNoVerifySide;
        }
    }

    [[nodiscard]] bool isReady() const
    {
        return slots_ != nullptr && code_ != nullptr && code_->isReady();
    }

    // NativeExec に渡す関数。
    static NativeResult runThunk(void* context, M68k& cpu);
    static const NativeStats* statsThunk(void* context);

    [[nodiscard]] NativeExec exec()
    {
        return NativeExec{&BlockRunner::runThunk, &BlockRunner::statsThunk, this};
    }

    [[nodiscard]] const NativeStats& stats() const
    {
        return stats_;
    }

    // 満杯のまま何回諦めたら全部捨てるか。
    //
    // 捨てる費用は常駐ブロック (16KB に 100 本前後) の再翻訳まるごと。
    // それを取り返せるだけの要求が積もってから捨てる。
    //
    // **実機で測って決めた値。** 8192 だと 45 秒で 1,952 回捨てて
    // 5900 kHz、100000 なら 192 回で 6890 kHz、1000000 なら 19 回で
    // 6970 kHz。捨てる回数がそのまま速度に効く = **費用はホットセットを
    // 失うことであって、諦めている間の取りこぼしではない**。
    //
    // 翻訳量は 19,053 本 → 2,335 本と 8 分の 1 になるのに被覆率は
    // 51.7% → 51.8% で動かない。**翻訳のコスト自体は律速ではなく、
    // 効いているのは「捨てた直後の冷たい期間」の長さ**である。
    //
    // Why not 無限にしないか: 回復経路が無いのと同じになる。凍結して
    // いた頃の欠陥がそのまま戻る (生存性を失う)。100 万回は実測で
    // 60 秒に 19 回なので、数秒に 1 回は必ず採り直せる。
    //
    // **public なのはテストが実値で上限を組み立てるため。** 定数を写すと
    // 値を変えたときにテストの前提だけ古くなる (実際に踏んだ)。
    static constexpr std::uint32_t kCapacityResetThreshold = 1000000;

    // 全部捨てる。実行可能メモリも巻き戻す。
    void reset();

private:
    NativeResult run(M68k& cpu);
    void rememberFailure(std::uint32_t entryPc, std::uint16_t gen);
    // 設計 §5.5 の鍵照合 7 項。**唯一の実装。**
    //
    // Why not 呼ぶ側それぞれに書かないか: 段 D-0 の計測は「チェーンの hop が
    // 踏む判定と同一の判定を、飛ばずに数える」形なので、実物と計測が同じ式で
    // なければ意味を持たない。切り出す前は計測側が 7 項を**写していて**、
    // **実物だけ直せば計測が古い判定を測り続ける**構造だった。索引式を写して
    // 変異が素通りした前例と同じ形 (test_block_emitter.cpp の runnerSlotIndex)。
    //
    // **順序が意味を持つ。** kAlwaysStale の検査を世代の一致より先に置く
    // (このヘッダ冒頭の理由)。
    //
    // nowGen と nowMappingEpoch は呼ぶ側が渡す。鍵外れの帰属
    // (keyMissStale か keyMissGen か) が同じ値を要るので、ここで読み直すと
    // 2 回引くことになる。
    [[nodiscard]] static bool keyMatches(const BlockSlot& slot, std::uint32_t entryPc,
                                         std::uint16_t nowGen, std::uint32_t nowMappingEpoch)
    {
        return slot.code != nullptr &&                  // 翻訳済み
               slot.entryPc != 0 &&                     // 空きの番兵
               slot.entryPc == entryPc &&               // タグ
               slot.count <= kMaxOps &&                 // ゴミ検査
               slot.mappingEpoch == nowMappingEpoch &&  // 写像
               nowGen != CodeGenMap::kAlwaysStale &&    // **先に見る**
               nowGen == slot.pageGen;                  // 世代の一致
    }

    // 翻訳して置く。置けたらスロットを返す。
    BlockSlot* translate(M68k& cpu, std::uint32_t entryPc);

    // 世代だけが外れたスロットの命令語を実メモリと照合する。
    //
    // **偽共有 (同じ 1KB ページのデータを書いただけ) を弾くためだけにある。**
    // 実測で世代が動いたブロックの 100.00% がこれだった。
    //
    // true を返すのは「控えた語列と実メモリが 1 語残らず一致した」ときだけ。
    // **控えを持たない (verifyWords == 0) なら必ず false** で、呼ぶ側は
    // 翻訳し直す。照合を飛ばす方向には決して働かない。
    //
    // Why not 読めなかった語を「一致」とみなさないか: 窓の外へ出た番地は
    // peekCodeWord が false を返す。そこを一致扱いにすると、写像が変わって
    // 読めなくなったブロックを**古いまま実行し続ける**。読めなければ不一致。
    //
    // **段 G で static をやめた。** 長いブロックの控えはサイドテーブルにあり、
    // 表そのものは runner のメンバなので this が要る。
    [[nodiscard]] bool verifyMatches(const BlockSlot& slot, M68k& cpu) const;

    // スロットに紐づく控えを取り出す。**無ければ nullptr。**
    //
    // 索引が有効であることと、指した先が**このスロットのもの**であることは
    // 別。上書きされたスロットは索引を持ったままなので、持ち主 (ownerPc と
    // mappingEpoch) を突き合わせて初めて読んでよい (VerifySide の冒頭)。
    [[nodiscard]] const VerifySide* sideFor(const BlockSlot& slot) const
    {
        const bool hasIndex = slot.verifySide != kNoVerifySide && slot.verifySide < sideCount_;
        if (!hasIndex)
        {
            return nullptr;
        }
        const VerifySide& side = sides_[slot.verifySide];
        const bool isOwner = side.ownerPc != 0 && side.ownerPc == slot.entryPc &&
                             side.mappingEpoch == slot.mappingEpoch;
        if (!isOwner)
        {
            return nullptr;
        }
        return &side;
    }

    // 長いブロックの控えをサイドテーブルへ取る。取れたら索引を返す。
    //
    // **溢れたら kNoVerifySide** (控えを持たない = 段 F と同じ保守的な側)。
    [[nodiscard]] std::uint16_t captureSide(M68k& cpu, std::uint32_t entryPc,
                                            std::uint32_t mappingEpoch, std::uint32_t words);
    [[nodiscard]] std::uint32_t slotIndex(std::uint32_t pc) const
    {
        // PC は必ず偶数なので 1 bit 落としてから畳む。
        //
        // **下位ビットだけで畳むと 2*slotCount_ バイト周期の番地が全部
        // 衝突する。** 512 スロットなら 1KB 周期で、2MB に散った常駐部と
        // サブルーチン群に対して系統的に当たる。ページ番号のビットを
        // 混ぜて散らす (negative_cache.h が同じ罠を踏んで直した形)。
        //
        // Why not >> 11 か: 512 スロット (マスク 0x1FF = ビット 0-8) だと、
        // pc のビット 10 は (pc >> 1) でビット 9 へ行って**マスクの外へ
        // 落ち**、(pc >> 11) にも乗らないので索引から完全に消える。
        // その結果ちょうど 1KB 離れた 2 番地が必ず衝突したままになる
        // ——**直そうとした当の周期が残る**。>> 10 ならビット 10 が
        // 索引のビット 0 に入り、1KB 周期も 2KB 周期も完全に散る
        // (16 番地 → 8 スロット だったものが 16 スロットになる)。
        return ((pc >> 1) ^ (pc >> 10)) & (slotCount_ - 1);
    }

    // 発行はここへ書き、確定するときに実行可能メモリへ 32bit 単位で写す。
    //
    // **IRAM へ直接バイト書き込みをしてはいけない。** ESP32-S3 の IRAM は
    // 32bit 単位でしか読み書きできず、バイト書き込みは LoadStoreError で
    // 落ちる。実際に踏んだ (emit16 の out[0] = ... で panic)。
    // MALLOC_CAP_32BIT を付けても「32bit でアクセスすれば使える」だけで、
    // バイト書き込みが許されるようにはならない。
    //
    // 1 ブロックの上限は requiredSize が返す値なので、kMaxOps = 4 の
    // 最悪ケースに余裕を見て取る。
    //
    // **Tier B (読みガード) で 512 → 1024 へ広げた。** ガード付き 1 命令は
    // EA 計算 + ガード + commit + バイト 4 本の合成 + 本体で、Tier A の
    // 2-3 倍になる。加えて脱出用の出口の島が命令ごとに 1 つ増える。
    // 足りなければ translate が諦めるだけなので正しさは損なわれないが、
    // 諦めた分は素通りするので気づきにくい。
    //
    // **Tier C (書きガード) で 1024 → 1536 へ広げた。** 実測の最悪
    // (CLR.l (An) x 4) が 949 バイトで、1024 に対して余裕が 7% しか無かった。
    // 書き 1 命令は「ガード 3 本 + touch 2 組 + バイト 4 本の store + 島 2 つ」
    // で読み形の 1.5 倍近くあり、命令を 1 つ足しただけで超える。
    // 超えたら黙って諦める (素通りする) 形なので、先に広げておく。
    //
    // **値はエミッタ側の kMaxBlockBytes を使う。** ここに数字を書き写すと、
    // 片方だけ広げたときに「エミッタは吐けるが runner が収まらないと言って
    // 捨てる」形ができ、被覆が黙って減る (Tier E で実際に踏んだ)。
    static constexpr std::size_t kStagingBytes = jit::kMaxBlockBytes;
    alignas(4) std::uint8_t staging_[kStagingBytes]{};

    BlockSlot* slots_ = nullptr;
    std::uint32_t slotCount_ = 0;
    ExecMemory* code_ = nullptr;

    // --- スロットに収まらない控えの表 (段 G) ---
    //
    // **バンプアロケータ。** sideUsed_ を進めるだけで、個別には解放しない。
    // 空きへ戻るのは reset() の全捨てだけ (ExecMemory と同じ規律)。
    // 「解放済みの控えを読む」順序が作れないのはこのため (VerifySide の冒頭)。
    VerifySide* sides_ = nullptr;
    std::uint32_t sideCount_ = 0;
    std::uint32_t sideUsed_ = 0;
    // 飽和したページに当たった回数。閾値を超えたら世代を捨てて数え直す。
    //
    // Why 即座に捨てないか: 1 回の飽和で全部の世代を捨てると、正常な
    // ブロックまで再翻訳になる。まとまった数が溜まってからにする。
    static constexpr std::uint32_t kSaturationResetThreshold = 64;

    std::uint32_t saturatedSeen_ = 0;
    NegativeCache neg_{};
    // 直近に見た写像の世代。変わったら負の記憶を全部捨てる。
    std::uint32_t seenEpoch_ = 0;
    NativeStats stats_{};
    // 実行可能メモリを使い切ったら、それ以上翻訳しない。
    bool codeFull_ = false;
    // 満杯のまま諦めた回数 (kCapacityResetThreshold に達したら捨てる)。
    std::uint32_t fullSeen_ = 0;

    // --- Tier D: 動的分岐 (RTS / JSR) の飛び先を受け取る 1 語 ---
    //
    // 生成コードはここへ s32i で飛び先を書き、戻り値に kDynamicBranchFlag を
    // 立てる。run() はビットを見てからこの語を読み、M68k::branchTo へ渡す。
    //
    // **アドレスを生成コードへ焼いてよい根拠。** このメンバは BlockRunner の
    // 中にあり、runner 自身が動かない限り不変。setStorage / setNegativeStorage /
    // reset はどれも**他のメンバを差し替えるだけで this を動かさない**ので、
    // 焼いた値が古くなる経路が存在しない。窓 (ramBaseAddr) と違って
    // mappingEpoch による保護が要らないのはこのため。
    //
    // Why not M68kState へ 1 語足さないか: 出口の契約 (§5.1) が
    // 「M68kState はインタプリタで実行し終えた直後とビット単位で同一」を
    // 言っている。インタプリタが一度も書かない欄を足すと、その欄を
    // **同一性の比較から外さねばならなくなる**。一度外した欄は、以後どんな
    // 書き漏らしも検出できない。
    //
    // Why not 実行のたびにゼロへ戻さないか: 動的分岐が成立したときだけ読み、
    // そのときは必ず生成コードが直前に書いている。古い値が残っていても
    // 読む条件 (bit29) が立たないので届かない。ゼロ埋めはホットパスに
    // 1 store を足すだけで、何も守らない。
    std::uint32_t branchMailbox_ = 0;
};

}  // namespace x68k::jit

#endif  // X68K_PLATFORM_JIT_BLOCK_RUNNER_H
