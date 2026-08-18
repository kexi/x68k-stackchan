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

std::uint32_t runBlock(void* state, const void* code)
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
    // **a4-a11 を明示的に退避する。**
    //
    // Why 要るか: コンパイラがこの関数へ置く entry は `entry a1, 32` で、
    // **窓は a0-a3 の 4 本しかない**。生成コードは a2-a11 を使うので、
    // a4 以降は窓の外 = 祖先フレームの生きた値を壊す。
    //
    // 壊れ方は「retw.n が IllegalInstruction で落ちる」だった (実機で踏んだ)。
    // 窓の状態そのものが壊れるので、戻る瞬間まで症状が出ない。
    //
    // Why not clobber リストに書くだけで済ませないか: 試したが GCC は
    // `entry a1, 32` のまま窓を広げない (実測)。窓の大きさは呼び出し規約で
    // 決まり、clobber は動かせない。**自分でスタックへ逃がすしかない。**
    //
    // Why not 生成コードを a0-a3 に閉じ込めないか: それが本筋だが、
    // 68000 の 1 命令に src / dst / 結果 / CCR / SR / 定数の置き場が要り、
    // 4 本では収まらない。段 3 でレジスタ割り当てを見直すときの課題として
    // 残す。ここでは 16 命令払って正しさを取る。
    std::uint32_t saved[8];
    register std::uint32_t arg __asm__("a2") = reinterpret_cast<std::uint32_t>(state);
    register const void* target __asm__("a3") = code;
    __asm__ __volatile__(
        "s32i a4, %2, 0\n s32i a5, %2, 4\n s32i a6, %2, 8\n s32i a7, %2, 12\n"
        "s32i a8, %2, 16\n s32i a9, %2, 20\n s32i a10, %2, 24\n s32i a11, %2, 28\n"
        "callx0 %1\n"
        "l32i a4, %2, 0\n l32i a5, %2, 4\n l32i a6, %2, 8\n l32i a7, %2, 12\n"
        "l32i a8, %2, 16\n l32i a9, %2, 20\n l32i a10, %2, 24\n l32i a11, %2, 28\n"
        : "+r"(arg)
        : "r"(target), "r"(saved)
        : "a0", "memory");
    return arg;
#else
    // ホストでは呼べない。**黙って 0 を返さない** — 0 は「実行できなかった」の
    // 意味を持ちうるので、呼ばれたこと自体を見えるようにする。
    (void)code;
    (void)state;
    std::abort();
#endif
}

}  // namespace x68k::jit
