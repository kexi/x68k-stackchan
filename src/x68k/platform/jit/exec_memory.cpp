// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ExecMemory の本体。方針はヘッダの冒頭に書いた。
//
// ESP32 の外 (ホストのテスト) でもビルドできるようにしてある。ホストでは
// 実行可能メモリを普通の new で取り、runBlock は呼べない (呼んだら
// Xtensa の機械語をホストの CPU が実行することになる) ので必ず落とす。
//
// Why not ESP32 専用にして #if で丸ごと外さないか: 切り出し (allocate) の
// 整列と容量の判断は、実機でしか試せないと壊れたままになる種類のロジック。
// ホストでも同じコードを回せる形にしておく。

#include "exec_memory.h"

#include <cstdlib>
#include <new>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

namespace x68k::jit
{
namespace
{

#if defined(ESP_PLATFORM)
constexpr const char* kTag = "jit";
#endif

// 切り出しの整列。l32r のリテラルが 4 バイト整列を要求する。
constexpr std::size_t kAlign = 4;

constexpr std::size_t alignUp(std::size_t v)
{
    return (v + (kAlign - 1)) & ~(kAlign - 1);
}

}  // namespace

ExecMemory::~ExecMemory()
{
    if (base_ == nullptr)
    {
        return;
    }
#if defined(ESP_PLATFORM)
    heap_caps_free(base_);
#else
    ::operator delete[](base_, std::align_val_t{kAlign});
#endif
    base_ = nullptr;
}

bool ExecMemory::acquire(std::size_t bytes)
{
    if (base_ != nullptr || bytes == 0)
    {
        return false;
    }
    const std::size_t want = alignUp(bytes);

#if defined(ESP_PLATFORM)
    // MALLOC_CAP_32BIT が要る。IRAM は 32bit 単位でしか読み書きできないので、
    // これを付けないと確保できてもバイト書き込みで例外に入る。
    void* p = heap_caps_malloc(want, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (p == nullptr)
    {
        // **失敗を致命にしない。** IRAM は他の機能と食い合うので、
        // 減ったビルドでは普通に取れなくなる。JIT を諦めれば動く。
        ESP_LOGW(kTag, "exec memory %u bytes unavailable; JIT disabled",
                 static_cast<unsigned>(want));
        return false;
    }
#else
    void* p = ::operator new[](want, std::align_val_t{kAlign}, std::nothrow);
    if (p == nullptr)
    {
        return false;
    }
#endif

    base_ = static_cast<std::uint8_t*>(p);
    capacity_ = want;
    used_ = 0;
    return true;
}

std::uint8_t* ExecMemory::allocate(std::size_t size)
{
    if (base_ == nullptr)
    {
        return nullptr;
    }
    const std::size_t at = alignUp(used_);
    const std::size_t next = at + alignUp(size);
    if (next > capacity_)
    {
        return nullptr;
    }
    used_ = next;
    return base_ + at;
}

void ExecMemory::syncInstructionCache()
{
#if defined(ESP_PLATFORM) && defined(__XTENSA__)
    // 書いた直後に必ず呼ぶ。isync が無いと、命令フェッチが**書く前の中身**を
    // 拾うことがある。1 回目だけ壊れる形で出るので、最も追いにくい。
    __asm__ __volatile__("isync" ::: "memory");
#endif
}

#if !defined(ESP_PLATFORM) || !defined(__XTENSA__)
namespace
{
// ホストのテストが差し替える生成コードの代役。**実機のビルドには無い。**
RunBlockHook g_runBlockHook = nullptr;
}  // namespace
#endif

X68K_JIT_IRAM std::uint32_t runBlock(void* state, const void* code)
{
#if defined(ESP_PLATFORM) && defined(__XTENSA__)
    // この関数は windowed ABI でコンパイルされるので、entry / retw と a0 の
    // 保存はコンパイラが置く。
    //
    // **飛び先は a3 に置く。a12 ではない。**
    //
    // Why not a12 か: コンパイラがこの関数へ置く entry は `entry a1, 32` で、
    // これは **call4 の窓 (a0-a3 のみ)**。a12 はこの関数の窓の外にあり、
    // 物理レジスタファイル上では**祖先フレームの生きた値**が入っている。
    // WINDOWBASE を回さずに書くと、呼び出し元のさらに呼び出し元の
    // ローカル変数を静かに壊す。窓のオーバーフロー例外で物理レジスタが
    // メモリへ落ちて初めて症状が出るので、呼び出し深さに依存して散発的に
    // 現れ、原因から最も遠い形で壊れる。
    //
    // Why not "r" 制約でアドレスを任せないか: callx0 が a0 を潰すので、
    // コンパイラがアドレスを a0 へ置くと飛ぶ前に壊れる。飛び先も引数も
    // レジスタを名指しし、潰す側 (a0 と a4-a11) を clobber に書き出す。
    //
    // Why 引数の順が (state, code) なのか: a2 = 第 1 引数、a3 = 第 2 引数
    // なので、この順なら mov が 1 つも要らない。生成コードは
    // entry / callx0 a3 / retw.n の 3 命令だけになる (実測で確認)。
    // **a0 を自分で退避する。** これが無いと必ず落ちる。
    //
    // `callx0` は戻り番地を **a0** に書く。ところが a0 は windowed ABI の
    // 戻り番地でもあり、この関数の出口の `retw.n` はそれを使って戻る。
    // callx0 が上書きした a0 のまま retw.n を実行すると、**自分自身の
    // retw.n の番地へ戻ろうとして** IllegalInstruction で落ちる。
    //
    // 実機で踏んだときの証拠: PC == A0 == retw.n の番地。
    //
    // Why not clobber に "a0" と書くだけで済まないか: 書いてあるが GCC は
    // 退避コードを出さない。a0 は「entry / retw が面倒を見る」前提の
    // 特別なレジスタなので、通常の caller-saved と同じには扱われない。
    // **自分で mov して戻すしかない。**
    //
    // a4-a11 は clobber に書けば足りる (呼び先が潰してよいレジスタなので
    // コンパイラが前後で生きた値を置かない)。特別扱いが要るのは a0 だけ。
    register std::uint32_t arg __asm__("a2") = reinterpret_cast<std::uint32_t>(state);
    register const void* target __asm__("a3") = code;
    // **退避先はスタック。** レジスタに逃がすとどれを選んでも問題が出る:
    // 制約なしだと GCC は a12 を選び (実測)、それは entry a1, 32 の窓の外。
    // a3 に固定すると入力オペランドと衝突してコンパイルが通らない。
    // メモリなら窓と無関係に安全で、コストは 2 命令 (s32i/l32i)。
    std::uint32_t savedA0 = 0;
    __asm__ __volatile__(
        "s32i a0, %0, 0\n"
        "callx0 %2\n"
        "l32i a0, %0, 0\n"
        :
        : "r"(&savedA0), "r"(arg), "r"(target)
        : "memory");
    __asm__ __volatile__("" : "+r"(arg));
    return arg;
#else
    // ホストではテストが差し替えた関数があればそれを呼ぶ。
    if (g_runBlockHook != nullptr)
    {
        return g_runBlockHook(state, code);
    }
    // 差し替えが無ければ呼べない。**黙って 0 を返さない** — 0 は
    // 「実行できなかった」の意味を持ちうるので、呼ばれたこと自体を
    // 見えるようにする。
    (void)code;
    (void)state;
    std::abort();
#endif
}

#if !defined(ESP_PLATFORM) || !defined(__XTENSA__)
void setRunBlockHookForTest(RunBlockHook hook)
{
    g_runBlockHook = hook;
}
#endif

}  // namespace x68k::jit
