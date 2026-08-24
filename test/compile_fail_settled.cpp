// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// **これはコンパイルが失敗しなければならないファイル。**
//
// x68k::Settled は「機械全体の時間が実体化されている」ことの証明で、
// Scheduler だけが発行できる。ここを迂回できると、実体化を忘れたまま
// デバイスのレジスタを読む経路が型に見えないまま作れてしまう。
//
// Why not static_assert(!std::is_default_constructible_v<Settled>) で
// 済ませないか: **それでは守れていない状態を見逃す。**
// private な defaulted default ctor (Settled() = default;) は、
// std::is_default_constructible_v を false にしたまま、`f({})` の
// 値初期化だけは通してしまう (clang / GCC の実測で確認)。
// 型特性が false なので「守れている」ように見えるが、実際には偽造できる。
//
// 本体を書いた (user-provided な) ctor にすると、値初期化も
// 「'Settled::Settled()' is private within this context」で落ちる。
// その違いを確かめるには **実際にコンパイルさせて失敗すること** を
// 見るしかない。CMake の SHOULD_FAIL でそれを固定する。
//
// このファイルがコンパイルを **通ってしまったら**、Settled の宣言が
// = default へ戻されている。scheduler.h を見よ。

#include "machine.h"
#include "scheduler.h"

namespace
{

void forge(x68k::Machine& m)
{
    // 証明を空の初期化子から捏造しようとする。
    // Settled() が user-provided なら、ここで private アクセス違反になる。
    (void)m;
    [[maybe_unused]] const x68k::Settled fake = {};
}

}  // namespace

int main()
{
    x68k::Machine m;
    forge(m);
    return 0;
}
