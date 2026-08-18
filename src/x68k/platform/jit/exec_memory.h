// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 生成コードを置く実行可能メモリと、そこへ call0 で入るゲートウェイ。
//
// ## 確保に失敗したら JIT を使わない
//
// ESP32-S3 で MALLOC_CAP_EXEC が取れるのは IRAM の余りだけで、実測で 21KB
// しか無い。しかも他の機能 (IRAM_ATTR を付けた関数、ESP-IDF 自身) と食い合う。
// **取れなかったら黙って諦め、現行のインタプリタのまま動く**のが正しい振る舞い。
// ここで確保を必須にすると、IRAM が減ったビルドで起動しなくなる。
//
// Why not PSRAM へ置かないか: 命令フェッチが PSRAM へ落ちると、
// 「散らばったアクセスで実機が止まった」実績のある経路をコード実行で踏む。
// そもそも PSRAM は実行可能属性を持たない。
//
// ## call0 のゲートウェイ
//
// 生成コードは call0 ABI (a0 = 戻りアドレス、a2 = 引数と戻り値) で、
// ret.n で戻る。ESP-IDF は既定で windowed ABI なので、C++ の関数ポインタ
// 呼び出し (call8) では ret.n と食い違って戻ってこない。
//
// **windowed の関数の中からインラインアセンブラで callx0 する**のが接続点。
// 外側の runBlock は普通の C++ 関数なので entry / retw はコンパイラが置き、
// a0 の保存も面倒を見る。生成コードは a0 と a2-a11 を潰してよい。
//
// Why not 生成コード側を windowed (entry/retw) にしないか: 実測で windowed の
// 出入りが 9.3 サイクル、call0 が 6.7 サイクル。平均区間長 1.81 命令の
// ブロックでは、この 2.6 サイクルがブロックあたりの固定費として効く。

#ifndef X68K_PLATFORM_JIT_EXEC_MEMORY_H
#define X68K_PLATFORM_JIT_EXEC_MEMORY_H

#include <cstddef>
#include <cstdint>

#if defined(ESP_PLATFORM)
#include <esp_attr.h>
#endif

// 実機では IRAM へ置く。ホストでは無意味なので空にする。
#if defined(ESP_PLATFORM)
#define X68K_JIT_IRAM IRAM_ATTR
#else
#define X68K_JIT_IRAM
#endif

namespace x68k::jit
{

// 実行可能メモリの塊。確保できなければ isReady() が false を返す。
class ExecMemory
{
public:
    ExecMemory() = default;
    ExecMemory(const ExecMemory&) = delete;
    ExecMemory& operator=(const ExecMemory&) = delete;
    ~ExecMemory();

    // bytes バイトを MALLOC_CAP_EXEC から確保する。
    // **失敗しても致命ではない。** 呼び出し側は isReady() を見て JIT を諦める。
    bool acquire(std::size_t bytes);

    [[nodiscard]] bool isReady() const
    {
        return base_ != nullptr;
    }
    [[nodiscard]] std::uint8_t* base()
    {
        return base_;
    }
    [[nodiscard]] std::size_t capacity() const
    {
        return capacity_;
    }
    [[nodiscard]] std::size_t used() const
    {
        return used_;
    }

    // size バイトを切り出す。**32bit 整列で返す** (l32r のリテラルが
    // 4 バイト整列を要求する)。足りなければ nullptr。
    std::uint8_t* allocate(std::size_t size);

    // 全部を捨てて先頭から詰め直す。ブロックキャッシュを作り直すときに呼ぶ。
    void reset()
    {
        used_ = 0;
        // **確定済みの印も戻す。** 同じ番地へ別のコードを書いたのに
        // isync を省くと、古い命令が残ったまま実行される。
        committedTo_ = 0;
    }

    // 書き込んだコードを命令キャッシュへ反映させる。
    //
    // Xtensa はデータ書き込みと命令フェッチが別の経路を通るので、isync を
    // 挟まないと**書く前の中身**を実行しうる。失敗すると「1 回目だけ壊れる」
    // という最も追いにくい形になる。
    //
    // **直接呼ぶ必要はない。** commit() が呼ぶ。個別に呼びたい場面
    // (自分で領域を書いた場合など) のために公開している。
    static void syncInstructionCache();

    // 書き込みを確定させ、実行してよい状態にする。
    //
    // **allocate して書いたら、実行する前に必ずこれを通す。**
    //
    // Why not 「isync を呼ぶ規律」で済ませないか: 呼び忘れの失敗が
    // 「1 回目だけ壊れる」形なので、テストでも実機でも再現しない。
    // 規律で守るには症状が原因から遠すぎる。**呼んだかどうかを型で
    // 追えるようにし、runBlock が未確定の領域を実行しないようにする。**
    //
    // reset() の後は再び未確定に戻る。同じ番地へ別のコードを書いたのに
    // isync を省くと、**古い命令が残ったまま実行される**。
    void commit()
    {
        syncInstructionCache();
        committedTo_ = used_;
    }

    // used_ までが確定済みか。runBlock の事前条件。
    [[nodiscard]] bool isCommitted() const
    {
        return committedTo_ >= used_;
    }

private:
    std::uint8_t* base_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t used_ = 0;
    // commit() 済みのバイト数。allocate で used_ が伸びると未確定に戻る。
    std::size_t committedTo_ = 0;
};

// 生成コードを 1 回呼ぶ。戻り値は block_emitter.h の
// kBranchTakenFlag | サイクル数。
//
// code は ExecMemory から切り出した領域の**エントリポイント**
// (EmittedBlock::entryOffset を足した位置)。state は M68kState の先頭。
// **引数の順は (state, code)。** a2 = 第 1 引数 / a3 = 第 2 引数なので、
// この順なら飛び先を a3 へ置くのに mov が 1 つも要らない。
// **IRAM に置く。** flash に置くと、生成コード (IRAM) へ callx0 したときに
// IllegalInstruction で落ちた (実機で確認)。インラインアセンブラで同じ
// callx0 を書くと通るのに、関数にすると落ちるのが手がかりだった。
std::uint32_t runBlock(void* state, const void* code) X68K_JIT_IRAM;

}  // namespace x68k::jit

#endif  // X68K_PLATFORM_JIT_EXEC_MEMORY_H
