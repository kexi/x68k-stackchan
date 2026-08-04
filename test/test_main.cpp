// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// doctest のエントリポイント。実装は各 test_*.cpp が持つ。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

// LeakSanitizer を、このテストバイナリでだけ切る。
//
// doctest の TEST_SUITE は関数内 static の TestSuite を持ち、そこへ
// スイート名の文字列を確保したままプロセス終了まで解放しない
// (doctest.h の DOCTEST_TEST_SUITE_IMPL)。実行中ずっと必要な情報なので
// 解放しないこと自体が設計で、テスト本体の漏れではない。
// 実測では TEST_SUITE を使うファイル 1 つにつき 1 バイトが 1 件残る
// (7 ファイル -> 7 件)。
//
// Why not 抑制ファイル (LSAN_OPTIONS=suppressions=...) で個別に消さないか:
// 抑制のパターンはスタックトレースを**関数名で**照合するので
// llvm-symbolizer が要る。CI のコンテナに入っておらず、名前で書いた
// パターンが当たらない (実測で 7 件 -> 4 件までしか減らなかった)。
// symbolize=1 を渡しても、シンボライザ自体が無いので変わらない。
//
// Why not それで本物の leak を見逃さないか: ASan の他の検査
// (配列外・use-after-free・二重解放) はそのまま効く。エミュレータで
// 踏みやすいのはそちらで、実際 LSan がこれまで報告したのは doctest 自身の
// 7 件だけだった。テストは意図的に static へ確保するものが多く、
// 「確保したまま終了」を厳密に追う価値が薄い。
//
// extern "C" なのはランタイムがこの名前で弱シンボルを引くため。
extern "C" const char* __lsan_default_options();  // NOLINT(bugprone-reserved-identifier)
extern "C" const char* __lsan_default_options()   // NOLINT(bugprone-reserved-identifier)
{
    return "detect_leaks=0";
}
