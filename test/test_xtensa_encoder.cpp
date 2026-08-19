// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// xtensa_encoder.h が吐くバイト列が、xtensa-esp32s3-elf-as が吐くバイト列と
// 1 バイトも違わないこと。
//
// このファイルが段 2 の正しさの土台になる。Xtensa の機械語は、フィールドが
// 1 ニブルずれても「別の正当な命令」として実行されてしまう。実行可能メモリ
// 21KB の中でそれが起きると、失敗は別の命令の途中へ飛んだ先という**原因から
// 最も遠いところ**で表面化し、逆アセンブルしても正しく見える。
// だから期待値を人間が書かず、アセンブラに組ませた値だけを置く。
//
// ## 期待値の出どころ
//
// すべて xtensa-esp32s3-elf-as に組ませ、objdump -s で取り出したメモリ順の
// バイト列。命令 1 つを 1 セクションへ置いて逆アセンブル欄の手写しを排除してある。
//
// **objdump -d の逆アセンブル欄をそのまま写してはいけない。** あの列は 3 バイト
// 命令をバイト逆順で表示する。メモリ順を見るには objdump -s を使う。
//
// 各 CHECK の直前に元のニーモニックを書いてある。テストが落ちたときは、
// エンコーダではなくその行のアセンブリを組み直して正解を確かめ直すこと。
//
// ## なぜレジスタ番号とオフセットを振るのか
//
// 1 パターンだけだとフィールドの位置がずれていても通る。たとえば
// `l32i.n a0, a0, 0` は全フィールドが 0 なので、s と t を取り違えても同じ
// バイト列になる。境界 (a0 / a15、オフセット 0 / 最大値、即値の正負) と
// 非対称な組み合わせ (s != t) を必ず含める。

#include <cstdint>
#include <cstring>

#include "doctest.h"
#include "jit/xtensa_encoder.h"

using namespace x68k::jit;

namespace
{

// 生成したバイト列を期待値と比べる。長さの食い違いも捕まえる。
//
// Why not memcmp の結果だけを見ないか: 長さが違うとき memcmp は読みすぎるか
// 読み足りないかのどちらかで、どちらも「たまたま一致」を作りうる。
void expectBytes(const std::uint8_t* buf, std::size_t len, const std::uint8_t* want,
                 std::size_t wantLen, const char* what)
{
    INFO(what);
    REQUIRE(len == wantLen);
    for (std::size_t i = 0; i < wantLen; ++i)
    {
        INFO("byte ", i);
        CHECK(buf[i] == want[i]);
    }
}

}  // namespace

TEST_CASE("l32i.n / s32i.n がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // l32i.n a3, a2, 0
    {
        static constexpr std::uint8_t kWant[] = {0x38, 0x02};
        const std::size_t n = l32iN(buf, 3, 2, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i.n a3, a2, 0");
    }
    // l32i.n a3, a2, 4
    {
        static constexpr std::uint8_t kWant[] = {0x38, 0x12};
        const std::size_t n = l32iN(buf, 3, 2, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i.n a3, a2, 4");
    }
    // l32i.n a4, a2, 60
    {
        static constexpr std::uint8_t kWant[] = {0x48, 0xF2};
        const std::size_t n = l32iN(buf, 4, 2, 60);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i.n a4, a2, 60");
    }
    // l32i.n a0, a0, 0
    {
        static constexpr std::uint8_t kWant[] = {0x08, 0x00};
        const std::size_t n = l32iN(buf, 0, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i.n a0, a0, 0");
    }
    // l32i.n a5, a12, 8
    {
        static constexpr std::uint8_t kWant[] = {0x58, 0x2C};
        const std::size_t n = l32iN(buf, 5, 12, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i.n a5, a12, 8");
    }
    // l32i.n a15, a15, 60
    {
        static constexpr std::uint8_t kWant[] = {0xF8, 0xFF};
        const std::size_t n = l32iN(buf, 15, 15, 60);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i.n a15, a15, 60");
    }
    // l32i.n a0, a3, 36
    {
        static constexpr std::uint8_t kWant[] = {0x08, 0x93};
        const std::size_t n = l32iN(buf, 0, 3, 36);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i.n a0, a3, 36");
    }
    // s32i.n a3, a2, 4
    {
        static constexpr std::uint8_t kWant[] = {0x39, 0x12};
        const std::size_t n = s32iN(buf, 3, 2, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a3, a2, 4");
    }
    // s32i.n a0, a1, 0
    {
        static constexpr std::uint8_t kWant[] = {0x09, 0x01};
        const std::size_t n = s32iN(buf, 0, 1, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a0, a1, 0");
    }
    // s32i.n a15, a14, 60
    {
        static constexpr std::uint8_t kWant[] = {0xF9, 0xFE};
        const std::size_t n = s32iN(buf, 15, 14, 60);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a15, a14, 60");
    }
    // s32i.n a7, a3, 36
    {
        static constexpr std::uint8_t kWant[] = {0x79, 0x93};
        const std::size_t n = s32iN(buf, 7, 3, 36);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a7, a3, 36");
    }
    // s32i.n a0, a0, 0
    {
        static constexpr std::uint8_t kWant[] = {0x09, 0x00};
        const std::size_t n = s32iN(buf, 0, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a0, a0, 0");
    }
    // s32i.n a15, a2, 60
    {
        static constexpr std::uint8_t kWant[] = {0xF9, 0xF2};
        const std::size_t n = s32iN(buf, 15, 2, 60);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a15, a2, 60");
    }
    // s32i.n a4, a15, 8
    {
        static constexpr std::uint8_t kWant[] = {0x49, 0x2F};
        const std::size_t n = s32iN(buf, 4, 15, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a4, a15, 8");
    }
    // s32i.n a5, a12, 4
    {
        static constexpr std::uint8_t kWant[] = {0x59, 0x1C};
        const std::size_t n = s32iN(buf, 5, 12, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i.n a5, a12, 4");
    }
}

TEST_CASE("l32i / s32i (60 を超えるオフセット) がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // l32i a3, a2, 0
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0x22, 0x00};
        const std::size_t n = l32i(buf, 3, 2, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a3, a2, 0");
    }
    // l32i a3, a2, 4
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0x22, 0x01};
        const std::size_t n = l32i(buf, 3, 2, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a3, a2, 4");
    }
    // l32i a4, a5, 1020
    {
        static constexpr std::uint8_t kWant[] = {0x42, 0x25, 0xFF};
        const std::size_t n = l32i(buf, 4, 5, 1020);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a4, a5, 1020");
    }
    // l32i a15, a1, 256
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x21, 0x40};
        const std::size_t n = l32i(buf, 15, 1, 256);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a15, a1, 256");
    }
    // l32i a0, a0, 4
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0x20, 0x01};
        const std::size_t n = l32i(buf, 0, 0, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a0, a0, 4");
    }
    // l32i a6, a7, 128
    {
        static constexpr std::uint8_t kWant[] = {0x62, 0x27, 0x20};
        const std::size_t n = l32i(buf, 6, 7, 128);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a6, a7, 128");
    }
    // l32i a15, a14, 1020
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x2E, 0xFF};
        const std::size_t n = l32i(buf, 15, 14, 1020);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a15, a14, 1020");
    }
    // l32i a0, a14, 64
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0x2E, 0x10};
        const std::size_t n = l32i(buf, 0, 14, 64);
        expectBytes(buf, n, kWant, sizeof(kWant), "l32i a0, a14, 64");
    }
    // s32i a3, a2, 0
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0x62, 0x00};
        const std::size_t n = s32i(buf, 3, 2, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a3, a2, 0");
    }
    // s32i a0, a1, 0
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0x61, 0x00};
        const std::size_t n = s32i(buf, 0, 1, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a0, a1, 0");
    }
    // s32i a15, a14, 1020
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x6E, 0xFF};
        const std::size_t n = s32i(buf, 15, 14, 1020);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a15, a14, 1020");
    }
    // s32i a6, a7, 128
    {
        static constexpr std::uint8_t kWant[] = {0x62, 0x67, 0x20};
        const std::size_t n = s32i(buf, 6, 7, 128);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a6, a7, 128");
    }
    // s32i a4, a5, 256
    {
        static constexpr std::uint8_t kWant[] = {0x42, 0x65, 0x40};
        const std::size_t n = s32i(buf, 4, 5, 256);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a4, a5, 256");
    }
    // s32i a15, a1, 64
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x61, 0x10};
        const std::size_t n = s32i(buf, 15, 1, 64);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a15, a1, 64");
    }
    // s32i a0, a0, 4
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0x60, 0x01};
        const std::size_t n = s32i(buf, 0, 0, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a0, a0, 4");
    }
    // s32i a3, a2, 1020
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0x62, 0xFF};
        const std::size_t n = s32i(buf, 3, 2, 1020);
        expectBytes(buf, n, kWant, sizeof(kWant), "s32i a3, a2, 1020");
    }
}

TEST_CASE("movi.n (RI7: レジスタは s フィールド) がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // movi.n a4, 0
    {
        static constexpr std::uint8_t kWant[] = {0x0C, 0x04};
        const std::size_t n = moviN(buf, 4, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a4, 0");
    }
    // movi.n a3, 1
    {
        static constexpr std::uint8_t kWant[] = {0x0C, 0x13};
        const std::size_t n = moviN(buf, 3, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a3, 1");
    }
    // movi.n a5, 95
    {
        static constexpr std::uint8_t kWant[] = {0x5C, 0xF5};
        const std::size_t n = moviN(buf, 5, 95);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a5, 95");
    }
    // movi.n a6, -1
    {
        static constexpr std::uint8_t kWant[] = {0x7C, 0xF6};
        const std::size_t n = moviN(buf, 6, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a6, -1");
    }
    // movi.n a15, -32
    {
        static constexpr std::uint8_t kWant[] = {0x6C, 0x0F};
        const std::size_t n = moviN(buf, 15, -32);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a15, -32");
    }
    // movi.n a0, 47
    {
        static constexpr std::uint8_t kWant[] = {0x2C, 0xF0};
        const std::size_t n = moviN(buf, 0, 47);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a0, 47");
    }
    // movi.n a0, -32
    {
        static constexpr std::uint8_t kWant[] = {0x6C, 0x00};
        const std::size_t n = moviN(buf, 0, -32);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a0, -32");
    }
    // movi.n a15, 95
    {
        static constexpr std::uint8_t kWant[] = {0x5C, 0xFF};
        const std::size_t n = moviN(buf, 15, 95);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a15, 95");
    }
    // movi.n a3, -1
    {
        static constexpr std::uint8_t kWant[] = {0x7C, 0xF3};
        const std::size_t n = moviN(buf, 3, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a3, -1");
    }
    // movi.n a5, 0
    {
        static constexpr std::uint8_t kWant[] = {0x0C, 0x05};
        const std::size_t n = moviN(buf, 5, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi.n a5, 0");
    }
}

TEST_CASE("movi (12bit 即値) がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // movi a3, 0
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0xA0, 0x00};
        const std::size_t n = movi(buf, 3, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a3, 0");
    }
    // movi a4, 100
    {
        static constexpr std::uint8_t kWant[] = {0x42, 0xA0, 0x64};
        const std::size_t n = movi(buf, 4, 100);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a4, 100");
    }
    // movi a5, 2047
    {
        static constexpr std::uint8_t kWant[] = {0x52, 0xA7, 0xFF};
        const std::size_t n = movi(buf, 5, 2047);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a5, 2047");
    }
    // movi a7, -2048
    {
        static constexpr std::uint8_t kWant[] = {0x72, 0xA8, 0x00};
        const std::size_t n = movi(buf, 7, -2048);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a7, -2048");
    }
    // movi a0, -1
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0xAF, 0xFF};
        const std::size_t n = movi(buf, 0, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a0, -1");
    }
    // movi a15, 1234
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0xA4, 0xD2};
        const std::size_t n = movi(buf, 15, 1234);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a15, 1234");
    }
    // movi a0, -1000
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0xAC, 0x18};
        const std::size_t n = movi(buf, 0, -1000);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a0, -1000");
    }
    // movi a15, 2047
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0xA7, 0xFF};
        const std::size_t n = movi(buf, 15, 2047);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a15, 2047");
    }
    // movi a3, -2048
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0xA8, 0x00};
        const std::size_t n = movi(buf, 3, -2048);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a3, -2048");
    }
    // movi a7, 100
    {
        static constexpr std::uint8_t kWant[] = {0x72, 0xA0, 0x64};
        const std::size_t n = movi(buf, 7, 100);
        expectBytes(buf, n, kWant, sizeof(kWant), "movi a7, 100");
    }
}

TEST_CASE("add.n / sub / and / or / xor がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // add.n a5, a3, a4
    {
        static constexpr std::uint8_t kWant[] = {0x4A, 0x53};
        const std::size_t n = addN(buf, 5, 3, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "add.n a5, a3, a4");
    }
    // add.n a0, a0, a0
    {
        static constexpr std::uint8_t kWant[] = {0x0A, 0x00};
        const std::size_t n = addN(buf, 0, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "add.n a0, a0, a0");
    }
    // add.n a15, a14, a13
    {
        static constexpr std::uint8_t kWant[] = {0xDA, 0xFE};
        const std::size_t n = addN(buf, 15, 14, 13);
        expectBytes(buf, n, kWant, sizeof(kWant), "add.n a15, a14, a13");
    }
    // add.n a2, a3, a15
    {
        static constexpr std::uint8_t kWant[] = {0xFA, 0x23};
        const std::size_t n = addN(buf, 2, 3, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "add.n a2, a3, a15");
    }
    // add.n a0, a15, a0
    {
        static constexpr std::uint8_t kWant[] = {0x0A, 0x0F};
        const std::size_t n = addN(buf, 0, 15, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "add.n a0, a15, a0");
    }
    // add.n a15, a0, a15
    {
        static constexpr std::uint8_t kWant[] = {0xFA, 0xF0};
        const std::size_t n = addN(buf, 15, 0, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "add.n a15, a0, a15");
    }
    // sub a5, a3, a4
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x53, 0xC0};
        const std::size_t n = sub(buf, 5, 3, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "sub a5, a3, a4");
    }
    // sub a0, a0, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x00, 0xC0};
        const std::size_t n = sub(buf, 0, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "sub a0, a0, a0");
    }
    // sub a15, a14, a13
    {
        static constexpr std::uint8_t kWant[] = {0xD0, 0xFE, 0xC0};
        const std::size_t n = sub(buf, 15, 14, 13);
        expectBytes(buf, n, kWant, sizeof(kWant), "sub a15, a14, a13");
    }
    // sub a2, a3, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x23, 0xC0};
        const std::size_t n = sub(buf, 2, 3, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "sub a2, a3, a15");
    }
    // sub a0, a15, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x0F, 0xC0};
        const std::size_t n = sub(buf, 0, 15, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "sub a0, a15, a0");
    }
    // sub a15, a0, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0xF0, 0xC0};
        const std::size_t n = sub(buf, 15, 0, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "sub a15, a0, a15");
    }
    // and a5, a3, a4
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x53, 0x10};
        const std::size_t n = and_(buf, 5, 3, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "and a5, a3, a4");
    }
    // and a0, a0, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x00, 0x10};
        const std::size_t n = and_(buf, 0, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "and a0, a0, a0");
    }
    // and a15, a14, a13
    {
        static constexpr std::uint8_t kWant[] = {0xD0, 0xFE, 0x10};
        const std::size_t n = and_(buf, 15, 14, 13);
        expectBytes(buf, n, kWant, sizeof(kWant), "and a15, a14, a13");
    }
    // and a2, a3, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x23, 0x10};
        const std::size_t n = and_(buf, 2, 3, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "and a2, a3, a15");
    }
    // and a0, a15, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x0F, 0x10};
        const std::size_t n = and_(buf, 0, 15, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "and a0, a15, a0");
    }
    // and a15, a0, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0xF0, 0x10};
        const std::size_t n = and_(buf, 15, 0, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "and a15, a0, a15");
    }
    // or a5, a3, a4
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x53, 0x20};
        const std::size_t n = or_(buf, 5, 3, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "or a5, a3, a4");
    }
    // or a0, a0, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x00, 0x20};
        const std::size_t n = or_(buf, 0, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "or a0, a0, a0");
    }
    // or a15, a14, a13
    {
        static constexpr std::uint8_t kWant[] = {0xD0, 0xFE, 0x20};
        const std::size_t n = or_(buf, 15, 14, 13);
        expectBytes(buf, n, kWant, sizeof(kWant), "or a15, a14, a13");
    }
    // or a2, a3, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x23, 0x20};
        const std::size_t n = or_(buf, 2, 3, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "or a2, a3, a15");
    }
    // or a0, a15, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x0F, 0x20};
        const std::size_t n = or_(buf, 0, 15, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "or a0, a15, a0");
    }
    // or a15, a0, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0xF0, 0x20};
        const std::size_t n = or_(buf, 15, 0, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "or a15, a0, a15");
    }
    // xor a5, a3, a4
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x53, 0x30};
        const std::size_t n = xor_(buf, 5, 3, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "xor a5, a3, a4");
    }
    // xor a0, a0, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x00, 0x30};
        const std::size_t n = xor_(buf, 0, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "xor a0, a0, a0");
    }
    // xor a15, a14, a13
    {
        static constexpr std::uint8_t kWant[] = {0xD0, 0xFE, 0x30};
        const std::size_t n = xor_(buf, 15, 14, 13);
        expectBytes(buf, n, kWant, sizeof(kWant), "xor a15, a14, a13");
    }
    // xor a2, a3, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x23, 0x30};
        const std::size_t n = xor_(buf, 2, 3, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "xor a2, a3, a15");
    }
    // xor a0, a15, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x0F, 0x30};
        const std::size_t n = xor_(buf, 0, 15, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "xor a0, a15, a0");
    }
    // xor a15, a0, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0xF0, 0x30};
        const std::size_t n = xor_(buf, 15, 0, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "xor a15, a0, a15");
    }
}

TEST_CASE("extui (shiftimm の bit4 が op2 へ分かれる) がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // extui a3, a4, 0, 8
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x30, 0x74};
        const std::size_t n = extui(buf, 3, 4, 0, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a3, a4, 0, 8");
    }
    // extui a3, a4, 0, 16
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x30, 0xF4};
        const std::size_t n = extui(buf, 3, 4, 0, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a3, a4, 0, 16");
    }
    // extui a3, a4, 8, 8
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x38, 0x74};
        const std::size_t n = extui(buf, 3, 4, 8, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a3, a4, 8, 8");
    }
    // extui a3, a4, 16, 16
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x30, 0xF5};
        const std::size_t n = extui(buf, 3, 4, 16, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a3, a4, 16, 16");
    }
    // extui a3, a4, 24, 8
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x38, 0x75};
        const std::size_t n = extui(buf, 3, 4, 24, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a3, a4, 24, 8");
    }
    // extui a3, a4, 31, 1
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x3F, 0x05};
        const std::size_t n = extui(buf, 3, 4, 31, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a3, a4, 31, 1");
    }
    // extui a3, a4, 4, 12
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x34, 0xB4};
        const std::size_t n = extui(buf, 3, 4, 4, 12);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a3, a4, 4, 12");
    }
    // extui a0, a1, 0, 8
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x00, 0x74};
        const std::size_t n = extui(buf, 0, 1, 0, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a0, a1, 0, 8");
    }
    // extui a0, a1, 0, 16
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x00, 0xF4};
        const std::size_t n = extui(buf, 0, 1, 0, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a0, a1, 0, 16");
    }
    // extui a0, a1, 8, 8
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x08, 0x74};
        const std::size_t n = extui(buf, 0, 1, 8, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a0, a1, 8, 8");
    }
    // extui a0, a1, 16, 16
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x00, 0xF5};
        const std::size_t n = extui(buf, 0, 1, 16, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a0, a1, 16, 16");
    }
    // extui a0, a1, 24, 8
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x08, 0x75};
        const std::size_t n = extui(buf, 0, 1, 24, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a0, a1, 24, 8");
    }
    // extui a0, a1, 31, 1
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x0F, 0x05};
        const std::size_t n = extui(buf, 0, 1, 31, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a0, a1, 31, 1");
    }
    // extui a0, a1, 4, 12
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x04, 0xB4};
        const std::size_t n = extui(buf, 0, 1, 4, 12);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a0, a1, 4, 12");
    }
    // extui a15, a14, 0, 8
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0xF0, 0x74};
        const std::size_t n = extui(buf, 15, 14, 0, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a15, a14, 0, 8");
    }
    // extui a15, a14, 0, 16
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0xF0, 0xF4};
        const std::size_t n = extui(buf, 15, 14, 0, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a15, a14, 0, 16");
    }
    // extui a15, a14, 8, 8
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0xF8, 0x74};
        const std::size_t n = extui(buf, 15, 14, 8, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a15, a14, 8, 8");
    }
    // extui a15, a14, 16, 16
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0xF0, 0xF5};
        const std::size_t n = extui(buf, 15, 14, 16, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a15, a14, 16, 16");
    }
    // extui a15, a14, 24, 8
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0xF8, 0x75};
        const std::size_t n = extui(buf, 15, 14, 24, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a15, a14, 24, 8");
    }
    // extui a15, a14, 31, 1
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0xFF, 0x05};
        const std::size_t n = extui(buf, 15, 14, 31, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a15, a14, 31, 1");
    }
    // extui a15, a14, 4, 12
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0xF4, 0xB4};
        const std::size_t n = extui(buf, 15, 14, 4, 12);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a15, a14, 4, 12");
    }
    // extui a7, a8, 0, 8
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0x70, 0x74};
        const std::size_t n = extui(buf, 7, 8, 0, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a7, a8, 0, 8");
    }
    // extui a7, a8, 0, 16
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0x70, 0xF4};
        const std::size_t n = extui(buf, 7, 8, 0, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a7, a8, 0, 16");
    }
    // extui a7, a8, 8, 8
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0x78, 0x74};
        const std::size_t n = extui(buf, 7, 8, 8, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a7, a8, 8, 8");
    }
    // extui a7, a8, 16, 16
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0x70, 0xF5};
        const std::size_t n = extui(buf, 7, 8, 16, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a7, a8, 16, 16");
    }
    // extui a7, a8, 24, 8
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0x78, 0x75};
        const std::size_t n = extui(buf, 7, 8, 24, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a7, a8, 24, 8");
    }
    // extui a7, a8, 31, 1
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0x7F, 0x05};
        const std::size_t n = extui(buf, 7, 8, 31, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a7, a8, 31, 1");
    }
    // extui a7, a8, 4, 12
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0x74, 0xB4};
        const std::size_t n = extui(buf, 7, 8, 4, 12);
        expectBytes(buf, n, kWant, sizeof(kWant), "extui a7, a8, 4, 12");
    }
}

TEST_CASE("callx0 / ret.n がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // callx0 a0
    {
        static constexpr std::uint8_t kWant[] = {0xC0, 0x00, 0x00};
        const std::size_t n = callx0(buf, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "callx0 a0");
    }
    // callx0 a3
    {
        static constexpr std::uint8_t kWant[] = {0xC0, 0x03, 0x00};
        const std::size_t n = callx0(buf, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "callx0 a3");
    }
    // callx0 a8
    {
        static constexpr std::uint8_t kWant[] = {0xC0, 0x08, 0x00};
        const std::size_t n = callx0(buf, 8);
        expectBytes(buf, n, kWant, sizeof(kWant), "callx0 a8");
    }
    // callx0 a15
    {
        static constexpr std::uint8_t kWant[] = {0xC0, 0x0F, 0x00};
        const std::size_t n = callx0(buf, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "callx0 a15");
    }
    // ret.n
    {
        static constexpr std::uint8_t kWant[] = {0x0D, 0xF0};
        const std::size_t n = retN(buf);
        expectBytes(buf, n, kWant, sizeof(kWant), "ret.n");
    }
}

TEST_CASE("beqz / bnez (12bit 変位、基準は insnPc + 4) がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // beqz a0, . -64+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x00, 0xFC};
        const std::size_t n = beqz(buf, 0, -64);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a0, . -64+4");
    }
    // beqz a0, . -8+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x80, 0xFF};
        const std::size_t n = beqz(buf, 0, -8);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a0, . -8+4");
    }
    // beqz a0, . -4+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xC0, 0xFF};
        const std::size_t n = beqz(buf, 0, -4);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a0, . -4+4");
    }
    // beqz a0, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xF0, 0xFF};
        const std::size_t n = beqz(buf, 0, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a0, . -1+4");
    }
    // beqz a0, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x30, 0x00};
        const std::size_t n = beqz(buf, 0, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a0, . +3+4");
    }
    // beqz a0, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xB0, 0x03};
        const std::size_t n = beqz(buf, 0, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a0, . +59+4");
    }
    // beqz a0, . +254+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xE0, 0x0F};
        const std::size_t n = beqz(buf, 0, 254);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a0, . +254+4");
    }
    // beqz a15, . -64+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x0F, 0xFC};
        const std::size_t n = beqz(buf, 15, -64);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a15, . -64+4");
    }
    // beqz a15, . -8+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x8F, 0xFF};
        const std::size_t n = beqz(buf, 15, -8);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a15, . -8+4");
    }
    // beqz a15, . -4+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xCF, 0xFF};
        const std::size_t n = beqz(buf, 15, -4);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a15, . -4+4");
    }
    // beqz a15, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xFF, 0xFF};
        const std::size_t n = beqz(buf, 15, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a15, . -1+4");
    }
    // beqz a15, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x3F, 0x00};
        const std::size_t n = beqz(buf, 15, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a15, . +3+4");
    }
    // beqz a15, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xBF, 0x03};
        const std::size_t n = beqz(buf, 15, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a15, . +59+4");
    }
    // beqz a15, . +254+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xEF, 0x0F};
        const std::size_t n = beqz(buf, 15, 254);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a15, . +254+4");
    }
    // beqz a3, . -64+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x03, 0xFC};
        const std::size_t n = beqz(buf, 3, -64);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a3, . -64+4");
    }
    // beqz a3, . -8+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x83, 0xFF};
        const std::size_t n = beqz(buf, 3, -8);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a3, . -8+4");
    }
    // beqz a3, . -4+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xC3, 0xFF};
        const std::size_t n = beqz(buf, 3, -4);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a3, . -4+4");
    }
    // beqz a3, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xF3, 0xFF};
        const std::size_t n = beqz(buf, 3, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a3, . -1+4");
    }
    // beqz a3, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x33, 0x00};
        const std::size_t n = beqz(buf, 3, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a3, . +3+4");
    }
    // beqz a3, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xB3, 0x03};
        const std::size_t n = beqz(buf, 3, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a3, . +59+4");
    }
    // beqz a3, . +254+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xE3, 0x0F};
        const std::size_t n = beqz(buf, 3, 254);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a3, . +254+4");
    }
    // beqz a5, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xF5, 0xFF};
        const std::size_t n = beqz(buf, 5, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a5, . -1+4");
    }
    // beqz a5, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0x35, 0x00};
        const std::size_t n = beqz(buf, 5, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a5, . +3+4");
    }
    // beqz a5, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0x16, 0xB5, 0x03};
        const std::size_t n = beqz(buf, 5, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beqz a5, . +59+4");
    }
}

TEST_CASE("beq / bne (8bit 変位) がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // beq a0, a1, . -64+4
    {
        static constexpr std::uint8_t kWant[] = {0x17, 0x10, 0xC0};
        const std::size_t n = beq(buf, 0, 1, -64);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a0, a1, . -64+4");
    }
    // beq a0, a1, . -8+4
    {
        static constexpr std::uint8_t kWant[] = {0x17, 0x10, 0xF8};
        const std::size_t n = beq(buf, 0, 1, -8);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a0, a1, . -8+4");
    }
    // beq a0, a1, . -4+4
    {
        static constexpr std::uint8_t kWant[] = {0x17, 0x10, 0xFC};
        const std::size_t n = beq(buf, 0, 1, -4);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a0, a1, . -4+4");
    }
    // beq a0, a1, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0x17, 0x10, 0xFF};
        const std::size_t n = beq(buf, 0, 1, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a0, a1, . -1+4");
    }
    // beq a0, a1, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0x17, 0x10, 0x03};
        const std::size_t n = beq(buf, 0, 1, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a0, a1, . +3+4");
    }
    // beq a0, a1, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0x17, 0x10, 0x3B};
        const std::size_t n = beq(buf, 0, 1, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a0, a1, . +59+4");
    }
    // beq a0, a1, . +122+4
    {
        static constexpr std::uint8_t kWant[] = {0x17, 0x10, 0x7A};
        const std::size_t n = beq(buf, 0, 1, 122);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a0, a1, . +122+4");
    }
    // beq a15, a14, . -64+4
    {
        static constexpr std::uint8_t kWant[] = {0xE7, 0x1F, 0xC0};
        const std::size_t n = beq(buf, 15, 14, -64);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a15, a14, . -64+4");
    }
    // beq a15, a14, . -8+4
    {
        static constexpr std::uint8_t kWant[] = {0xE7, 0x1F, 0xF8};
        const std::size_t n = beq(buf, 15, 14, -8);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a15, a14, . -8+4");
    }
    // beq a15, a14, . -4+4
    {
        static constexpr std::uint8_t kWant[] = {0xE7, 0x1F, 0xFC};
        const std::size_t n = beq(buf, 15, 14, -4);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a15, a14, . -4+4");
    }
    // beq a15, a14, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0xE7, 0x1F, 0xFF};
        const std::size_t n = beq(buf, 15, 14, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a15, a14, . -1+4");
    }
    // beq a15, a14, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0xE7, 0x1F, 0x03};
        const std::size_t n = beq(buf, 15, 14, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a15, a14, . +3+4");
    }
    // beq a15, a14, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0xE7, 0x1F, 0x3B};
        const std::size_t n = beq(buf, 15, 14, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a15, a14, . +59+4");
    }
    // beq a15, a14, . +122+4
    {
        static constexpr std::uint8_t kWant[] = {0xE7, 0x1F, 0x7A};
        const std::size_t n = beq(buf, 15, 14, 122);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a15, a14, . +122+4");
    }
    // beq a2, a3, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0x37, 0x12, 0xFF};
        const std::size_t n = beq(buf, 2, 3, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a2, a3, . -1+4");
    }
    // beq a2, a3, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0x37, 0x12, 0x03};
        const std::size_t n = beq(buf, 2, 3, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a2, a3, . +3+4");
    }
    // beq a2, a3, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0x37, 0x12, 0x3B};
        const std::size_t n = beq(buf, 2, 3, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a2, a3, . +59+4");
    }
    // beq a2, a3, . +122+4
    {
        static constexpr std::uint8_t kWant[] = {0x37, 0x12, 0x7A};
        const std::size_t n = beq(buf, 2, 3, 122);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a2, a3, . +122+4");
    }
    // beq a3, a4, . -64+4
    {
        static constexpr std::uint8_t kWant[] = {0x47, 0x13, 0xC0};
        const std::size_t n = beq(buf, 3, 4, -64);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a3, a4, . -64+4");
    }
    // beq a3, a4, . -8+4
    {
        static constexpr std::uint8_t kWant[] = {0x47, 0x13, 0xF8};
        const std::size_t n = beq(buf, 3, 4, -8);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a3, a4, . -8+4");
    }
    // beq a3, a4, . -4+4
    {
        static constexpr std::uint8_t kWant[] = {0x47, 0x13, 0xFC};
        const std::size_t n = beq(buf, 3, 4, -4);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a3, a4, . -4+4");
    }
    // beq a3, a4, . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0x47, 0x13, 0xFF};
        const std::size_t n = beq(buf, 3, 4, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a3, a4, . -1+4");
    }
    // beq a3, a4, . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0x47, 0x13, 0x03};
        const std::size_t n = beq(buf, 3, 4, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a3, a4, . +3+4");
    }
    // beq a3, a4, . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0x47, 0x13, 0x3B};
        const std::size_t n = beq(buf, 3, 4, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "beq a3, a4, . +59+4");
    }
}

TEST_CASE("j (18bit 変位、下位 6bit は 0x06) がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // j . -259+4
    {
        static constexpr std::uint8_t kWant[] = {0x46, 0xBF, 0xFF};
        const std::size_t n = j(buf, -259);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . -259+4");
    }
    // j . -64+4
    {
        static constexpr std::uint8_t kWant[] = {0x06, 0xF0, 0xFF};
        const std::size_t n = j(buf, -64);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . -64+4");
    }
    // j . -8+4
    {
        static constexpr std::uint8_t kWant[] = {0x06, 0xFE, 0xFF};
        const std::size_t n = j(buf, -8);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . -8+4");
    }
    // j . -4+4
    {
        static constexpr std::uint8_t kWant[] = {0x06, 0xFF, 0xFF};
        const std::size_t n = j(buf, -4);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . -4+4");
    }
    // j . -1+4
    {
        static constexpr std::uint8_t kWant[] = {0xC6, 0xFF, 0xFF};
        const std::size_t n = j(buf, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . -1+4");
    }
    // j . +3+4
    {
        static constexpr std::uint8_t kWant[] = {0xC6, 0x00, 0x00};
        const std::size_t n = j(buf, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . +3+4");
    }
    // j . +59+4
    {
        static constexpr std::uint8_t kWant[] = {0xC6, 0x0E, 0x00};
        const std::size_t n = j(buf, 59);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . +59+4");
    }
    // j . +254+4
    {
        static constexpr std::uint8_t kWant[] = {0x86, 0x3F, 0x00};
        const std::size_t n = j(buf, 254);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . +254+4");
    }
    // j . +999+4
    {
        static constexpr std::uint8_t kWant[] = {0xC6, 0xF9, 0x00};
        const std::size_t n = j(buf, 999);
        expectBytes(buf, n, kWant, sizeof(kWant), "j . +999+4");
    }
}

TEST_CASE("範囲判定が短縮形と通常形を正しく選び分ける")
{
    // 短縮形のオフセットは 0..60 の 4 の倍数だけ。境界と、4 の倍数でない値。
    CHECK(canNarrowOffset(0));
    CHECK(canNarrowOffset(60));
    CHECK_FALSE(canNarrowOffset(64));
    CHECK_FALSE(canNarrowOffset(2));
    CHECK_FALSE(canNarrowOffset(61));

    CHECK(canWideOffset(0));
    CHECK(canWideOffset(1020));
    CHECK_FALSE(canWideOffset(1024));
    CHECK_FALSE(canWideOffset(6));

    // movi.n は -32..95 という**非対称な**範囲。7bit の 2 の補数 (-64..63) ではない。
    CHECK(canMoviN(-32));
    CHECK(canMoviN(95));
    CHECK_FALSE(canMoviN(-33));
    CHECK_FALSE(canMoviN(96));

    CHECK(canMovi(-2048));
    CHECK(canMovi(2047));
    CHECK_FALSE(canMovi(-2049));
    CHECK_FALSE(canMovi(2048));

    // beqz/bnez は 12bit、beq/bne は 8bit。**同じ「分岐」でも届く距離が違う。**
    CHECK(canBranch12(-2048));
    CHECK(canBranch12(2047));
    CHECK_FALSE(canBranch12(2048));

    CHECK(canBranch8(-128));
    CHECK(canBranch8(127));
    CHECK_FALSE(canBranch8(128));
    CHECK_FALSE(canBranch8(-129));

    CHECK(canJump(-131072));
    CHECK(canJump(131071));
    CHECK_FALSE(canJump(131072));
}

TEST_CASE("エンコーダは指定した長さだけを書き、その先を汚さない")
{
    // 生成コードは 21KB の実行可能メモリへ詰めて置く。1 バイトでも書きすぎると
    // 直前に置いた命令の先頭を潰す。戻り値が実際の書き込み量と一致することを、
    // 番兵で確かめる。
    std::uint8_t buf[8];

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(l32iN(buf, 3, 2, 0) == 2);
    CHECK(buf[2] == 0xCC);

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(retN(buf) == 2);
    CHECK(buf[2] == 0xCC);

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(moviN(buf, 5, 95) == 2);
    CHECK(buf[2] == 0xCC);

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(addN(buf, 5, 3, 4) == 2);
    CHECK(buf[2] == 0xCC);

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(l32i(buf, 3, 2, 64) == 3);
    CHECK(buf[3] == 0xCC);

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(extui(buf, 3, 4, 0, 8) == 3);
    CHECK(buf[3] == 0xCC);

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(j(buf, -4) == 3);
    CHECK(buf[3] == 0xCC);

    std::memset(buf, 0xCC, sizeof(buf));
    CHECK(callx0(buf, 3) == 3);
    CHECK(buf[3] == 0xCC);
}

TEST_CASE("フィールドの取り違えが必ず見える組み合わせ")
{
    // s と t を取り違えても同じバイト列になる組み合わせ (全フィールドが等しい) では
    // 位置のずれを検出できない。**非対称な組み合わせで両方向を問う。**
    std::uint8_t a[8];
    std::uint8_t b[8];

    // l32i.n: ソースとデスティネーションを入れ替えると別のバイト列になること。
    l32iN(a, 3, 2, 0);
    l32iN(b, 2, 3, 0);
    CHECK(std::memcmp(a, b, 2) != 0);

    // オフセットが違えば別のバイト列になること (r フィールドがオフセットを運ぶ)。
    l32iN(a, 3, 2, 0);
    l32iN(b, 3, 2, 4);
    CHECK(std::memcmp(a, b, 2) != 0);

    // sub は非可換。オペランドの順序が保たれること。
    sub(a, 5, 3, 4);
    sub(b, 5, 4, 3);
    CHECK(std::memcmp(a, b, 3) != 0);

    // and / or / xor は op2 だけが違う。取り違えると別の演算になる。
    and_(a, 5, 3, 4);
    or_(b, 5, 3, 4);
    CHECK(std::memcmp(a, b, 3) != 0);
    xor_(b, 5, 3, 4);
    CHECK(std::memcmp(a, b, 3) != 0);

    // beq と bne は r フィールドの 1 ビットだけが違う。条件が反転しないこと。
    beq(a, 3, 4, 8);
    bne(b, 3, 4, 8);
    CHECK(std::memcmp(a, b, 3) != 0);

    // beqz と bnez も同様。
    beqz(a, 3, 8);
    bnez(b, 3, 8);
    CHECK(std::memcmp(a, b, 3) != 0);

    // extui: shiftimm 15 と 16 は bit4 が op2 へ繰り上がる境界。
    // 分割を忘れると 16 が 0 に化けて同じバイト列になる。
    extui(a, 3, 4, 15, 8);
    extui(b, 3, 4, 16, 8);
    CHECK(std::memcmp(a, b, 3) != 0);
    extui(a, 3, 4, 0, 8);
    extui(b, 3, 4, 16, 8);
    CHECK(std::memcmp(a, b, 3) != 0);

    // movi.n は RI7 でレジスタが s フィールドへ入る。即値とレジスタが混ざらないこと。
    moviN(a, 3, 1);
    moviN(b, 1, 3);
    CHECK(std::memcmp(a, b, 2) != 0);

    // j と callx0 は下位バイトが近い。j が呼び出しに化けないこと。
    j(a, 0);
    callx0(b, 0);
    CHECK(std::memcmp(a, b, 3) != 0);
}

// ---------------------------------------------------------------------------
// 段 2 の発行器 (block_emitter.cpp) が要求した命令。上と同じく、期待値は
// xtensa-esp32s3-elf-as に組ませて objdump -s から取り出したメモリ順のバイト列。
//
// **アセンブラは既定で addi を addi.n へ、mov を mov.n へ縮める。** 上の値は
// --no-transform で縮小を止めて採った。エンコーダは常に 3 バイト形を吐くので、
// 縮んだ値を期待値にすると全部落ちる。
// ---------------------------------------------------------------------------

TEST_CASE("l16ui / s16i がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // l16ui a5, a12, 76
    {
        static constexpr std::uint8_t kWant[] = {0x52, 0x1C, 0x26};
        const std::size_t n = l16ui(buf, 5, 12, 76);
        expectBytes(buf, n, kWant, sizeof(kWant), "l16ui a5, a12, 76");
    }
    // l16ui a0, a1, 0
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0x11, 0x00};
        const std::size_t n = l16ui(buf, 0, 1, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "l16ui a0, a1, 0");
    }
    // l16ui a15, a14, 510
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x1E, 0xFF};
        const std::size_t n = l16ui(buf, 15, 14, 510);
        expectBytes(buf, n, kWant, sizeof(kWant), "l16ui a15, a14, 510");
    }
    // l16ui a3, a2, 78
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0x12, 0x27};
        const std::size_t n = l16ui(buf, 3, 2, 78);
        expectBytes(buf, n, kWant, sizeof(kWant), "l16ui a3, a2, 78");
    }
    // s16i a5, a12, 76
    {
        static constexpr std::uint8_t kWant[] = {0x52, 0x5C, 0x26};
        const std::size_t n = s16i(buf, 5, 12, 76);
        expectBytes(buf, n, kWant, sizeof(kWant), "s16i a5, a12, 76");
    }
    // s16i a0, a1, 0
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0x51, 0x00};
        const std::size_t n = s16i(buf, 0, 1, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "s16i a0, a1, 0");
    }
    // s16i a15, a14, 510
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x5E, 0xFF};
        const std::size_t n = s16i(buf, 15, 14, 510);
        expectBytes(buf, n, kWant, sizeof(kWant), "s16i a15, a14, 510");
    }
    // s16i a4, a3, 80
    {
        static constexpr std::uint8_t kWant[] = {0x42, 0x53, 0x28};
        const std::size_t n = s16i(buf, 4, 3, 80);
        expectBytes(buf, n, kWant, sizeof(kWant), "s16i a4, a3, 80");
    }
}

// 保証: l8ui のオフセットは割らずにそのまま imm8 へ入る。
//
// l32i (4 で割る) / l16ui (2 で割る) と粒度が違うので、そろえて割ると
// 4 倍離れた番地を読む別の正当な命令になる。オフセット 1 / 3 / 17 /
// 127 / 128 / 255 を混ぜてあるのは、割り算が入っていたらどれかで必ず
// 食い違わせるため (0 と偶数だけだと 2 で割る実装を見逃す)。
TEST_CASE("l8ui がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // l8ui a4, a2, 0
    {
        static constexpr std::uint8_t kWant[] = {0x42, 0x02, 0x00};
        const std::size_t n = l8ui(buf, 4, 2, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a4, a2, 0");
    }
    // l8ui a5, a2, 1
    {
        static constexpr std::uint8_t kWant[] = {0x52, 0x02, 0x01};
        const std::size_t n = l8ui(buf, 5, 2, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a5, a2, 1");
    }
    // l8ui a15, a14, 255
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x0E, 0xFF};
        const std::size_t n = l8ui(buf, 15, 14, 255);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a15, a14, 255");
    }
    // l8ui a2, a3, 0
    {
        static constexpr std::uint8_t kWant[] = {0x22, 0x03, 0x00};
        const std::size_t n = l8ui(buf, 2, 3, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a2, a3, 0");
    }
    // l8ui a7, a8, 17
    {
        static constexpr std::uint8_t kWant[] = {0x72, 0x08, 0x11};
        const std::size_t n = l8ui(buf, 7, 8, 17);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a7, a8, 17");
    }
    // l8ui a11, a3, 128
    {
        static constexpr std::uint8_t kWant[] = {0xB2, 0x03, 0x80};
        const std::size_t n = l8ui(buf, 11, 3, 128);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a11, a3, 128");
    }
    // l8ui a6, a6, 127
    {
        static constexpr std::uint8_t kWant[] = {0x62, 0x06, 0x7F};
        const std::size_t n = l8ui(buf, 6, 6, 127);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a6, a6, 127");
    }
    // l8ui a9, a0, 255
    {
        static constexpr std::uint8_t kWant[] = {0x92, 0x00, 0xFF};
        const std::size_t n = l8ui(buf, 9, 0, 255);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a9, a0, 255");
    }
    // l8ui a3, a15, 3
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0x0F, 0x03};
        const std::size_t n = l8ui(buf, 3, 15, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "l8ui a3, a15, 3");
    }

    // l16ui と同じバイト列にならないこと。r フィールド (0x0 と 0x1) だけが
    // 違う隣り合った符号なので、取り違えても「動くが別の命令」になる。
    {
        std::uint8_t other[8];
        const std::size_t n = l8ui(buf, 4, 2, 2);
        const std::size_t m = l16ui(other, 4, 2, 2);
        REQUIRE(n == m);
        CHECK(std::memcmp(buf, other, n) != 0);
    }

    CHECK(canByteOffset(0u));
    CHECK(canByteOffset(255u));
    CHECK_FALSE(canByteOffset(256u));
}

// 保証: s8i のオフセットは割らずにそのまま imm8 へ入り、s16i / s32i とは
// 別のバイト列になる。
//
// l8ui と同じ理由でオフセットの粒度がバイト単位。s16i (2 で割る) や
// s32i (4 で割る) にそろえて割ると、2 倍・4 倍離れた番地を書く**別の正当な
// 命令**になる。読みなら値が狂うだけだが、書きだと窓の中の無関係な番地を潰す。
// オフセット 1 / 2 / 3 / 17 / 127 / 128 / 255 を混ぜてあるのは、
// 割り算が入っていたらどれかで必ず食い違わせるため。
TEST_CASE("s8i がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // s8i a4, a2, 0
    {
        static constexpr std::uint8_t kWant[] = {0x42, 0x42, 0x00};
        const std::size_t n = s8i(buf, 4, 2, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a4, a2, 0");
    }
    // s8i a5, a2, 1
    {
        static constexpr std::uint8_t kWant[] = {0x52, 0x42, 0x01};
        const std::size_t n = s8i(buf, 5, 2, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a5, a2, 1");
    }
    // s8i a15, a14, 255
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0x4E, 0xFF};
        const std::size_t n = s8i(buf, 15, 14, 255);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a15, a14, 255");
    }
    // s8i a7, a8, 128
    {
        static constexpr std::uint8_t kWant[] = {0x72, 0x48, 0x80};
        const std::size_t n = s8i(buf, 7, 8, 128);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a7, a8, 128");
    }
    // s8i a11, a3, 17
    {
        static constexpr std::uint8_t kWant[] = {0xB2, 0x43, 0x11};
        const std::size_t n = s8i(buf, 11, 3, 17);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a11, a3, 17");
    }
    // s8i a0, a1, 3
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0x41, 0x03};
        const std::size_t n = s8i(buf, 0, 1, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a0, a1, 3");
    }
    // s8i a2, a3, 0
    {
        static constexpr std::uint8_t kWant[] = {0x22, 0x43, 0x00};
        const std::size_t n = s8i(buf, 2, 3, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a2, a3, 0");
    }
    // s8i a6, a6, 127
    {
        static constexpr std::uint8_t kWant[] = {0x62, 0x46, 0x7F};
        const std::size_t n = s8i(buf, 6, 6, 127);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a6, a6, 127");
    }
    // s8i a9, a0, 255
    {
        static constexpr std::uint8_t kWant[] = {0x92, 0x40, 0xFF};
        const std::size_t n = s8i(buf, 9, 0, 255);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a9, a0, 255");
    }
    // s8i a3, a15, 3
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0x4F, 0x03};
        const std::size_t n = s8i(buf, 3, 15, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a3, a15, 3");
    }
    // s8i a4, a5, 2
    {
        static constexpr std::uint8_t kWant[] = {0x42, 0x45, 0x02};
        const std::size_t n = s8i(buf, 4, 5, 2);
        expectBytes(buf, n, kWant, sizeof(kWant), "s8i a4, a5, 2");
    }

    // s16i / s32i / l8ui と同じバイト列にならないこと。
    // r フィールド (0x4 / 0x5 / 0x6 / 0x0) だけが違う隣り合った符号なので、
    // 取り違えても「動くが別の命令」になる。
    {
        std::uint8_t other[8];
        const std::size_t n = s8i(buf, 4, 2, 4);
        CHECK(s16i(other, 4, 2, 4) == n);
        CHECK(std::memcmp(buf, other, n) != 0);
        CHECK(s32i(other, 4, 2, 4) == n);
        CHECK(std::memcmp(buf, other, n) != 0);
        CHECK(l8ui(other, 4, 2, 4) == n);
        CHECK(std::memcmp(buf, other, n) != 0);
    }
}

TEST_CASE("mov.n / neg がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // mov.n a12, a2
    {
        static constexpr std::uint8_t kWant[] = {0xCD, 0x02};
        const std::size_t n = movN(buf, 12, 2);
        expectBytes(buf, n, kWant, sizeof(kWant), "mov.n a12, a2");
    }
    // mov.n a0, a15
    {
        static constexpr std::uint8_t kWant[] = {0x0D, 0x0F};
        const std::size_t n = movN(buf, 0, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "mov.n a0, a15");
    }
    // mov.n a3, a3
    {
        static constexpr std::uint8_t kWant[] = {0x3D, 0x03};
        const std::size_t n = movN(buf, 3, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "mov.n a3, a3");
    }
    // neg a3, a4
    {
        static constexpr std::uint8_t kWant[] = {0x40, 0x30, 0x60};
        const std::size_t n = neg(buf, 3, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "neg a3, a4");
    }
    // neg a15, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0xF0, 0x60};
        const std::size_t n = neg(buf, 15, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "neg a15, a0");
    }
    // neg a0, a15
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x00, 0x60};
        const std::size_t n = neg(buf, 0, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "neg a0, a15");
    }
}

TEST_CASE("addi がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // addi a3, a4, -1
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0xC4, 0xFF};
        const std::size_t n = addi(buf, 3, 4, -1);
        expectBytes(buf, n, kWant, sizeof(kWant), "addi a3, a4, -1");
    }
    // addi a3, a4, 127
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0xC4, 0x7F};
        const std::size_t n = addi(buf, 3, 4, 127);
        expectBytes(buf, n, kWant, sizeof(kWant), "addi a3, a4, 127");
    }
    // addi a3, a4, -128
    {
        static constexpr std::uint8_t kWant[] = {0x32, 0xC4, 0x80};
        const std::size_t n = addi(buf, 3, 4, -128);
        expectBytes(buf, n, kWant, sizeof(kWant), "addi a3, a4, -128");
    }
    // addi a15, a0, 0
    {
        static constexpr std::uint8_t kWant[] = {0xF2, 0xC0, 0x00};
        const std::size_t n = addi(buf, 15, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "addi a15, a0, 0");
    }
    // addi a0, a15, 1
    {
        static constexpr std::uint8_t kWant[] = {0x02, 0xCF, 0x01};
        const std::size_t n = addi(buf, 0, 15, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "addi a0, a15, 1");
    }
}

TEST_CASE("moveqz / movnez がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // moveqz a6, a7, a3
    {
        static constexpr std::uint8_t kWant[] = {0x30, 0x67, 0x83};
        const std::size_t n = moveqz(buf, 6, 7, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "moveqz a6, a7, a3");
    }
    // moveqz a0, a15, a14
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0x0F, 0x83};
        const std::size_t n = moveqz(buf, 0, 15, 14);
        expectBytes(buf, n, kWant, sizeof(kWant), "moveqz a0, a15, a14");
    }
    // moveqz a15, a0, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0xF0, 0x83};
        const std::size_t n = moveqz(buf, 15, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "moveqz a15, a0, a0");
    }
    // movnez a6, a7, a3
    {
        static constexpr std::uint8_t kWant[] = {0x30, 0x67, 0x93};
        const std::size_t n = movnez(buf, 6, 7, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "movnez a6, a7, a3");
    }
    // movnez a0, a15, a14
    {
        static constexpr std::uint8_t kWant[] = {0xE0, 0x0F, 0x93};
        const std::size_t n = movnez(buf, 0, 15, 14);
        expectBytes(buf, n, kWant, sizeof(kWant), "movnez a0, a15, a14");
    }
    // movnez a15, a0, a0
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0xF0, 0x93};
        const std::size_t n = movnez(buf, 15, 0, 0);
        expectBytes(buf, n, kWant, sizeof(kWant), "movnez a15, a0, a0");
    }
}

TEST_CASE("slli がアセンブラと一致する")
{
    std::uint8_t buf[8];
    // slli a3, a4, 1
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x34, 0x11};
        const std::size_t n = slli(buf, 3, 4, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a3, a4, 1");
    }
    // slli a7, a7, 3
    {
        static constexpr std::uint8_t kWant[] = {0xD0, 0x77, 0x11};
        const std::size_t n = slli(buf, 7, 7, 3);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a7, a7, 3");
    }
    // slli a3, a4, 4
    {
        static constexpr std::uint8_t kWant[] = {0xC0, 0x34, 0x11};
        const std::size_t n = slli(buf, 3, 4, 4);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a3, a4, 4");
    }
    // slli a3, a4, 15
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x34, 0x11};
        const std::size_t n = slli(buf, 3, 4, 15);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a3, a4, 15");
    }
    // slli a3, a4, 16
    {
        static constexpr std::uint8_t kWant[] = {0x00, 0x34, 0x11};
        const std::size_t n = slli(buf, 3, 4, 16);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a3, a4, 16");
    }
    // slli a3, a4, 17
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x34, 0x01};
        const std::size_t n = slli(buf, 3, 4, 17);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a3, a4, 17");
    }
    // slli a3, a4, 31
    {
        static constexpr std::uint8_t kWant[] = {0x10, 0x34, 0x01};
        const std::size_t n = slli(buf, 3, 4, 31);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a3, a4, 31");
    }
    // slli a0, a15, 1
    {
        static constexpr std::uint8_t kWant[] = {0xF0, 0x0F, 0x11};
        const std::size_t n = slli(buf, 0, 15, 1);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a0, a15, 1");
    }
    // slli a15, a0, 24
    {
        static constexpr std::uint8_t kWant[] = {0x80, 0xF0, 0x01};
        const std::size_t n = slli(buf, 15, 0, 24);
        expectBytes(buf, n, kWant, sizeof(kWant), "slli a15, a0, 24");
    }
}

TEST_CASE("l32r の変位がアセンブラと一致する")
{
    // リテラルをコードの前に置いたときの変位。**基準は「命令アドレス + 3 を
    // 4 へ切り上げた値」**で、命令長ではなく整列で決まる。ここを取り違えると
    // 「別の正当な定数」を読むので、演算結果が静かに間違う形でしか出てこない。
    //
    // 確認元: リテラル 1/2/5 語 × 前置 nop 0..5 語の全組み合わせをアセンブラに
    // 組ませ、l32rOffset() の式と一致することを確かめた。ここはその代表。
    std::uint8_t buf[8];

    // リテラルが 1 語 (0番地)、l32r が 4 番地にある場合。
    // 元: `.align 4 / L0: .word ... / l32r a3, L0` の l32r
    {
        static constexpr std::uint8_t kWant[] = {0x31, 0xFF, 0xFF};
        CHECK(l32rOffset(4, 0) == -1);
        const std::size_t n = l32r(buf, 3, l32rOffset(4, 0));
        expectBytes(buf, n, kWant, sizeof(kWant), "l32r a3, .-4 (整列済み)");
    }
    // 3 バイト命令が 1 つ挟まった位置 (7 番地) から 0 番地のリテラルを引く。
    // (7 + 3) & ~3 == 8 なので変位は -2。
    {
        CHECK(l32rOffset(7, 0) == -2);
        static constexpr std::uint8_t kWant[] = {0x41, 0xFE, 0xFF};
        const std::size_t n = l32r(buf, 4, l32rOffset(7, 0));
        expectBytes(buf, n, kWant, sizeof(kWant), "l32r a4 (整列で切り上がる)");
    }
    // 2 語のリテラルの 2 語目 (4 番地) を 8 番地から引く。
    {
        CHECK(l32rOffset(8, 4) == -1);
        static constexpr std::uint8_t kWant[] = {0x31, 0xFF, 0xFF};
        const std::size_t n = l32r(buf, 3, l32rOffset(8, 4));
        expectBytes(buf, n, kWant, sizeof(kWant), "l32r a3, L1");
    }

    // 変位は必ず負。前方 (コードより後ろ) のリテラルは引けない。
    CHECK_FALSE(canL32r(0));
    CHECK_FALSE(canL32r(1));
    CHECK(canL32r(-1));
    CHECK(canL32r(-65536));
    CHECK_FALSE(canL32r(-65537));
}

TEST_CASE("範囲外を先に問える")
{
    // エンコーダは範囲外を渡されても assert しない (実行可能メモリを作る
    // ホットパスに置きたくないため)。代わりに canXxx() で先に問う契約に
    // してある。**その canXxx() が実際に境界を正しく持っていること。**

    // extui の maskimm は 1..16。24 を渡すと (24-1)&0xF = 7 に折り返して
    // 「下位 8 ビットだけを残す」別の正当な命令になる。段 2 の発行器は
    // 実際にこれを踏み、アセンブラに拒否されるまで気づかなかった。
    CHECK(canExtui(0, 8));
    CHECK(canExtui(0, 16));
    CHECK(canExtui(16, 16));
    CHECK(canExtui(31, 1));
    CHECK_FALSE(canExtui(0, 17));
    CHECK_FALSE(canExtui(0, 24));
    CHECK_FALSE(canExtui(8, 0));
    CHECK_FALSE(canExtui(24, 16));  // shiftimm + maskimm > 32
    CHECK_FALSE(canExtui(32, 1));

    // slli は 1..31。0 は符号化できない (32 - 0 = 32 が別の命令に化ける)。
    CHECK_FALSE(canSlli(0));
    CHECK(canSlli(1));
    CHECK(canSlli(31));
    CHECK_FALSE(canSlli(32));

    // 16bit のオフセットは 2 の倍数で 510 まで。32bit 版 (4 の倍数 / 1020) と
    // 粒度も上限も違う。
    CHECK(canHalfOffset(0));
    CHECK(canHalfOffset(76));
    CHECK(canHalfOffset(510));
    CHECK_FALSE(canHalfOffset(1));
    CHECK_FALSE(canHalfOffset(512));

    CHECK(canAddi(-128));
    CHECK(canAddi(127));
    CHECK_FALSE(canAddi(-129));
    CHECK_FALSE(canAddi(128));
}

TEST_CASE("フィールドの取り違えが必ず別のバイト列になる")
{
    // 1 パターンだけだとフィールドがずれていても通る。**取り違えたら
    // 必ず違うバイト列になる**組み合わせを明示的に置く。
    std::uint8_t a[8];
    std::uint8_t b[8];

    // l16ui と s16i は r フィールドだけが違う (0x1 と 0x5)。
    l16ui(a, 5, 12, 76);
    s16i(b, 5, 12, 76);
    CHECK(std::memcmp(a, b, 3) != 0);

    // moveqz と movnez は op2 だけが違う (0x8 と 0x9)。
    moveqz(a, 6, 7, 3);
    movnez(b, 6, 7, 3);
    CHECK(std::memcmp(a, b, 3) != 0);

    // mov.n の at と as を入れ替えたら違う命令になること。
    movN(a, 12, 2);
    movN(b, 2, 12);
    CHECK(std::memcmp(a, b, 2) != 0);

    // neg の s フィールドは 0 固定。ar と at を入れ替えたら変わること。
    neg(a, 3, 4);
    neg(b, 4, 3);
    CHECK(std::memcmp(a, b, 3) != 0);

    // slli の 15 と 16 は op2 が切り替わる境界。**分割を忘れると同じになる。**
    slli(a, 3, 4, 15);
    slli(b, 3, 4, 16);
    CHECK(std::memcmp(a, b, 3) != 0);
    slli(a, 3, 4, 16);
    slli(b, 3, 4, 17);
    CHECK(std::memcmp(a, b, 3) != 0);

    // addi の即値の符号。
    addi(a, 3, 4, 1);
    addi(b, 3, 4, -1);
    CHECK(std::memcmp(a, b, 3) != 0);
}
