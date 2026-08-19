// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// Xtensa LX7 (ESP32-S3) の命令エンコーダ。段 2 が BlockPlan を機械語へ落とすときの土台。
//
// ## エンコーディングは推測していない
//
// このファイルの定数は 1 つ残らず xtensa-esp32s3-elf-as に組ませ、objdump -s で
// **メモリ順のバイト列**を取り出して機械的に突き合わせて決めた。命令 1 つを 1 セクション
// (.section .iN) に置いて逆アセンブルの手写しを排除し、レジスタ番号・オフセット・即値・
// 変位を振った 1,412 通りで 0 件不一致まで詰めてある。test_xtensa_encoder.cpp が
// そのうちの代表を期待値として持ち、各所に「どのニーモニックから採ったか」を書いてある。
//
// Why not ISA リファレンスの表からフィールド位置を起こさないか: このプロジェクトは
// Xtensa のエンコーディングを手で組み立てて 2 度失敗している。表を読む工程が入ると、
// 読み違えたことを実行するまで誰も気づけない。アセンブラを正解として据えれば、
// 誤りは「テストが落ちる」という形でしか出てこない。
//
// 実際、この 1,412 通りの突き合わせは着手時の素朴な想定を 3 か所で覆した:
//   - movi.n は RRRN ではなく RI7。対象レジスタは t ではなく **s** フィールドに入り、
//     7bit 即値は imm[6:4] が t、imm[3:0] が r へ分かれる
//   - extui は op1 が maskimm-1、op2 が 0x4|sae[4] で、**素直に読むと逆に置きたくなる**
//   - j の下位 6bit は 0x06 (CALL 形式の 0x05 ではない)
// どれも実機で走らせるまで表面化しない種類の取り違えで、実行可能メモリ 21KB の中で
// 起きれば原因から最も遠いところ (別の命令の途中へ飛ぶ) で壊れる。
//
// ## バイト順
//
// Xtensa は little-endian で、命令も **下位バイトから** メモリへ並ぶ。
// objdump -d の逆アセンブル欄は 3 バイト命令をバイト逆順で表示するので、
// あの列をそのまま定数にすると必ず壊れる。ここでは objdump -s (メモリ順) を正とする。
//
// ## 呼び出し規約
//
// 生成コードは call0 ABI で発行する (windowed の entry/retw 9.3 サイクルに対し 6.7
// サイクル、実測)。したがって a1 (スタックポインタ) / PS / WINDOWBASE には触らない。
// 外側は windowed のゲートウェイで受け、callx0 で生成コードへ入る。

#ifndef X68K_PLATFORM_JIT_XTENSA_ENCODER_H
#define X68K_PLATFORM_JIT_XTENSA_ENCODER_H

#include <cstddef>
#include <cstdint>

namespace x68k::jit
{

using std::size_t;

// Xtensa の汎用レジスタ番号。a0-a15。
using XReg = std::uint8_t;

// call0 ABI で意味が決まっているレジスタ。生成コードが踏んではいけない境界。
inline constexpr XReg kA0 = 0;  // 戻りアドレス。callx0 が書き潰す
inline constexpr XReg kA1 = 1;  // スタックポインタ。生成コードは触らない
inline constexpr XReg kA2 = 2;  // 第 1 引数 / 戻り値

// 命令長。エンコーダはこの 2 つしか吐かない。
inline constexpr size_t kNarrowLen = 2;  // .n 付きの短縮形
inline constexpr size_t kWideLen = 3;    // 通常の 24bit 形

namespace detail
{

// 24bit 命令を下位バイトから並べる。Xtensa は little-endian。
inline size_t emit24(std::uint8_t* out, std::uint32_t op)
{
    out[0] = static_cast<std::uint8_t>(op & 0xFFu);
    out[1] = static_cast<std::uint8_t>((op >> 8) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((op >> 16) & 0xFFu);
    return kWideLen;
}

// 16bit 命令 (.n 付き短縮形)。
inline size_t emit16(std::uint8_t* out, std::uint32_t op)
{
    out[0] = static_cast<std::uint8_t>(op & 0xFFu);
    out[1] = static_cast<std::uint8_t>((op >> 8) & 0xFFu);
    return kNarrowLen;
}

inline constexpr std::uint32_t nib(std::uint32_t v)
{
    return v & 0xFu;
}

// RRRN 形式 (16bit): op0[3:0] | t[7:4] | s[11:8] | r[15:12]
inline size_t rrrn(std::uint8_t* out, std::uint32_t op0, std::uint32_t r, std::uint32_t s,
                   std::uint32_t t)
{
    return emit16(out, nib(op0) | (nib(t) << 4) | (nib(s) << 8) | (nib(r) << 12));
}

// RRR 形式 (24bit): op0[3:0] | t[7:4] | s[11:8] | r[15:12] | op1[19:16] | op2[23:20]
inline size_t rrr(std::uint8_t* out, std::uint32_t op0, std::uint32_t op1, std::uint32_t op2,
                  std::uint32_t r, std::uint32_t s, std::uint32_t t)
{
    return emit24(out, nib(op0) | (nib(t) << 4) | (nib(s) << 8) | (nib(r) << 12) |
                           (nib(op1) << 16) | (nib(op2) << 20));
}

// RRI8 形式 (24bit): op0[3:0] | t[7:4] | s[11:8] | r[15:12] | imm8[23:16]
inline size_t rri8(std::uint8_t* out, std::uint32_t op0, std::uint32_t r, std::uint32_t s,
                   std::uint32_t t, std::uint32_t imm8)
{
    return emit24(
        out, nib(op0) | (nib(t) << 4) | (nib(s) << 8) | (nib(r) << 12) | ((imm8 & 0xFFu) << 16));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ロード / ストア
// ---------------------------------------------------------------------------

// l32i.n at, as, off  — off は 0..60 の 4 の倍数。
// 確認元: `l32i.n a3, a2, 0` = 38 02 / `l32i.n a4, a2, 60` = 48 f2
//
// オフセットは 4 で割った商が r フィールドへ入る。範囲外を渡すと別のオフセットの
// 命令に化けるので、呼び出し側は canL32iN() で先に問うこと。
inline size_t l32iN(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rrrn(out, 0x8u, off >> 2, as, at);
}

// s32i.n at, as, off  — off は 0..60 の 4 の倍数。
// 確認元: `s32i.n a3, a2, 4` = 39 12 / `s32i.n a15, a14, 60` = f9 fe
inline size_t s32iN(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rrrn(out, 0x9u, off >> 2, as, at);
}

// 短縮形が使えるオフセットか。段 2 のエミッタはこれで .n と通常形を選び分ける。
inline constexpr bool canNarrowOffset(std::uint32_t off)
{
    return off <= 60u && (off % 4u) == 0u;
}

// l32i at, as, off  — off は 0..1020 の 4 の倍数。短縮形の 60 を超える場合に使う。
// 確認元: `l32i a15, a1, 256` = f2 21 40 / `l32i a4, a5, 1020` = 42 25 ff
inline size_t l32i(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rri8(out, 0x2u, 0x2u, as, at, off >> 2);
}

// s32i at, as, off  — off は 0..1020 の 4 の倍数。
// 確認元: `s32i a6, a7, 128` = 62 67 20 / `s32i a15, a14, 1020` = f2 6e ff
inline size_t s32i(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rri8(out, 0x2u, 0x6u, as, at, off >> 2);
}

inline constexpr bool canWideOffset(std::uint32_t off)
{
    return off <= 1020u && (off % 4u) == 0u;
}

// l8ui at, as, off  — 符号なし 8bit ロード。off は 0..255。
// 確認元: `l8ui a4, a2, 0` = 42 02 00 / `l8ui a5, a2, 1` = 52 02 01
//         `l8ui a15, a14, 255` = f2 0e ff / `l8ui a11, a3, 128` = b2 03 80
//
// **オフセットは割らない。** l32i は 4 で、l16ui は 2 で割った商を imm8 へ
// 入れるが、こちらはバイト単位なので生の値がそのまま入る。ここを 32bit 版に
// そろえて割ると、4 倍離れた番地を読む**別の正当な命令**になる。
//
// Why ゲストの word / long もこれで組むか: 68000 はビッグエンディアンなので、
// ホスト (リトルエンディアン) の l16ui / l32i で読むとバイトが逆になる。
// バイトずつ読んで slli / or で組めば、ホストのバイト順に依存しない
// (m68k.cpp の read16 / read32 が同じ理由で同じ形をしている)。
// l16ui を使う形は「ホスト側 2 整列」も前提に入り、検証面が増える。
inline size_t l8ui(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rri8(out, 0x2u, 0x0u, as, at, off);
}

inline constexpr bool canByteOffset(std::uint32_t off)
{
    return off <= 255u;
}

// s8i at, as, off  — 8bit ストア。off は 0..255。
// 確認元: `s8i a4, a2, 0` = 42 42 00 / `s8i a5, a2, 1` = 52 42 01
//         `s8i a15, a14, 255` = f2 4e ff / `s8i a7, a8, 128` = 72 48 80
//         `s8i a11, a3, 17` = b2 43 11 / `s8i a0, a1, 3` = 02 41 03
//
// **オフセットは割らない。** l8ui と同じ粒度 (r フィールドが 0x0 → 0x4 に
// 変わるだけ)。s16i (2 で割る) / s32i (4 で割る) にそろえて割ると、
// 2 倍・4 倍離れた番地を書く**別の正当な命令**になる。読みなら値が狂うだけだが、
// 書きだと窓の中の無関係な番地を潰す。
//
// Why ゲストの word / long もこれで組むか: l8ui と同じ理由。68000 は
// ビッグエンディアンなので、ホスト (リトルエンディアン) の s16i / s32i で
// 書くとバイトが逆になる。バイトずつ書けば m68k.cpp の write16 / write32 の
// 代入文をそのまま写せる (ホスト側の整列も前提に入らない)。
inline size_t s8i(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rri8(out, 0x2u, 0x4u, as, at, off);
}

// l16ui at, as, off  — 符号なし 16bit ロード。off は 0..510 の偶数。
// 確認元: `l16ui a5, a12, 76` = 52 1c 26 / `l16ui a0, a1, 0` = 02 11 00
//
// **オフセットの粒度が 32bit 版と違う。** l32i は 4 の倍数で 1020 まで届くが、
// こちらは 2 の倍数で 510 まで。M68kState の sr / ir / irc がこの範囲に入る。
inline size_t l16ui(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rri8(out, 0x2u, 0x1u, as, at, off >> 1);
}

// s16i at, as, off  — 16bit ストア。off は 0..510 の偶数。
// 確認元: `s16i a5, a12, 76` = 52 5c 26 / `s16i a15, a14, 510` = f2 5e ff
inline size_t s16i(std::uint8_t* out, XReg at, XReg as, std::uint32_t off)
{
    return detail::rri8(out, 0x2u, 0x5u, as, at, off >> 1);
}

inline constexpr bool canHalfOffset(std::uint32_t off)
{
    return off <= 510u && (off % 2u) == 0u;
}

// l32r at, literal  — PC 相対でリテラルプールから 32bit 定数を読む。
//
// **変位は必ず負。** リテラルはこの命令より前になければならない。
// 基準は「命令アドレス + 3 を 4 へ切り上げた値」で、命令長 (3) を足してから
// 整列させる形になる。imm16 = (literal - ((insnPc + 3) & ~3)) >> 2。
// 確認元: リテラル数 1/2/5 × 前置 nop 0..5 の全組み合わせで一致を確認済み
//         (`l32r a3, .-4` = 31 ff ff)。
//
// Why not movi を並べて 32bit を組み立てないか: movi は -2048..2047 しか
// 届かないので、24bit のゲストアドレスや 0xFFFF のマスクを作るには
// movi + slli + or の 3 命令 9 バイトが要る。l32r なら 3 バイトで、
// 同じ定数を使い回せば 2 度目以降はリテラルの追加すら要らない。
inline size_t l32r(std::uint8_t* out, XReg at, std::int32_t imm16)
{
    return detail::emit24(
        out, 0x1u | (detail::nib(at) << 4) | ((static_cast<std::uint32_t>(imm16) & 0xFFFFu) << 8));
}

// insnPc に置く l32r が literalPc のリテラルを読むための変位。
// 呼び出し側は canL32r() で先に届くかを問うこと。
inline constexpr std::int32_t l32rOffset(std::uint32_t insnPc, std::uint32_t literalPc)
{
    return (static_cast<std::int32_t>(literalPc) -
            static_cast<std::int32_t>((insnPc + 3u) & ~3u)) >>
           2;
}

inline constexpr bool canL32r(std::int32_t imm16)
{
    return imm16 >= -65536 && imm16 <= -1;
}

// ---------------------------------------------------------------------------
// 即値
// ---------------------------------------------------------------------------

// movi.n as, imm  — imm は -32..95。
//
// **RI7 形式で、他の .n 命令とフィールドの使い方が違う。** 対象レジスタは t ではなく
// s へ入り、7bit 即値は imm[6:4] が t、imm[3:0] が r へ分かれる。
// 確認元: `movi.n a4, 0` = 0c 04 / `movi.n a15, -32` = 6c 0f / `movi.n a0, -1` = 7c f0
//
// 負の値は 7bit の 2 の補数ではなく「imm[6:4] == 7 なら -32..-1」という規則で、
// -32 が 0x60、-1 が 0x7F になる。& 0x7F がちょうどその形を作る。
inline size_t moviN(std::uint8_t* out, XReg as, std::int32_t imm)
{
    const std::uint32_t v = static_cast<std::uint32_t>(imm) & 0x7Fu;
    return detail::rrrn(out, 0xCu, v & 0xFu, as, (v >> 4) & 0x7u);
}

inline constexpr bool canMoviN(std::int32_t imm)
{
    return imm >= -32 && imm <= 95;
}

// movi at, imm  — imm は -2048..2047。短縮形の範囲を超える即値に使う。
// 確認元: `movi a15, 1234` = f2 a4 d2 / `movi a6, -2048` = 62 a8 00 / `movi a7, -1` = 72 af ff
inline size_t movi(std::uint8_t* out, XReg at, std::int32_t imm)
{
    const std::uint32_t v = static_cast<std::uint32_t>(imm) & 0xFFFu;
    return detail::rri8(out, 0x2u, 0xAu, (v >> 8) & 0xFu, at, v & 0xFFu);
}

inline constexpr bool canMovi(std::int32_t imm)
{
    return imm >= -2048 && imm <= 2047;
}

// mov.n at, as  — レジスタ間コピー。
// 確認元: `mov.n a12, a2` = cd 02 / `mov.n a0, a15` = 0d 0f
inline size_t movN(std::uint8_t* out, XReg at, XReg as)
{
    return detail::rrrn(out, 0xDu, 0x0u, as, at);
}

// ---------------------------------------------------------------------------
// ALU
// ---------------------------------------------------------------------------

// add.n ar, as, at
// 確認元: `add.n a5, a3, a4` = 4a 53 / `add.n a15, a14, a13` = da fe
inline size_t addN(std::uint8_t* out, XReg ar, XReg as, XReg at)
{
    return detail::rrrn(out, 0xAu, ar, as, at);
}

// sub ar, as, at
// 確認元: `sub a5, a3, a4` = 40 53 c0 / `sub a0, a1, a2` = 20 01 c0
inline size_t sub(std::uint8_t* out, XReg ar, XReg as, XReg at)
{
    return detail::rrr(out, 0x0u, 0x0u, 0xCu, ar, as, at);
}

// and ar, as, at
// 確認元: `and a5, a3, a4` = 40 53 10 / `and a15, a0, a7` = 70 f0 10
inline size_t and_(std::uint8_t* out, XReg ar, XReg as, XReg at)
{
    return detail::rrr(out, 0x0u, 0x0u, 0x1u, ar, as, at);
}

// or ar, as, at
// 確認元: `or a5, a3, a4` = 40 53 20 / `or a6, a7, a8` = 80 67 20
inline size_t or_(std::uint8_t* out, XReg ar, XReg as, XReg at)
{
    return detail::rrr(out, 0x0u, 0x0u, 0x2u, ar, as, at);
}

// xor ar, as, at
// 確認元: `xor a5, a3, a4` = 40 53 30 / `xor a15, a12, a11` = b0 fc 30
inline size_t xor_(std::uint8_t* out, XReg ar, XReg as, XReg at)
{
    return detail::rrr(out, 0x0u, 0x0u, 0x3u, ar, as, at);
}

// addi at, as, imm  — as + imm を at へ。imm は 8bit 符号付き (-128..127)。
// 確認元: `addi a3, a4, 127` = 32 c4 7f / `addi a15, a0, -1` = f2 c0 ff
//
// **アセンブラは既定でこれを addi.n へ縮める。** 上の確認値は --no-transform で
// 縮小を止めて採った。エンコーダは常に 3 バイト形を吐く (長さが命令ごとに
// 変わらない方が、l32r の変位計算が単純になる)。
inline size_t addi(std::uint8_t* out, XReg at, XReg as, std::int32_t imm)
{
    return detail::rri8(out, 0x2u, 0xCu, as, at, static_cast<std::uint32_t>(imm) & 0xFFu);
}

inline constexpr bool canAddi(std::int32_t imm)
{
    return imm >= -128 && imm <= 127;
}

// neg ar, at  — 0 - at。
// 確認元: `neg a3, a4` = 40 30 60 / `neg a15, a0` = 00 f0 60
//
// **s フィールドは 0 固定。** RRR 形式の 3 オペランド目が無い形なので、
// ここへレジスタ番号を入れると別の命令になる。
inline size_t neg(std::uint8_t* out, XReg ar, XReg at)
{
    return detail::rrr(out, 0x0u, 0x0u, 0x6u, ar, 0x0u, at);
}

// moveqz ar, as, at  — at が 0 なら ar = as。0 でなければ ar は変わらない。
// 確認元: `moveqz a6, a7, a3` = 30 67 83 / `moveqz a0, a15, a14` = e0 0f 83
//
// **op1 が 0x3、op2 が 0x8。** 分岐なしで Z フラグを作るのに使う。
// 分岐で作ると、条件分岐が入ったぶんだけ変位の計算が発行順に依存する。
inline size_t moveqz(std::uint8_t* out, XReg ar, XReg as, XReg at)
{
    return detail::rrr(out, 0x0u, 0x3u, 0x8u, ar, as, at);
}

// movnez ar, as, at  — at が 0 でなければ ar = as。
// 確認元: `movnez a6, a7, a3` = 30 67 93 / `movnez a0, a15, a14` = e0 0f 93
inline size_t movnez(std::uint8_t* out, XReg ar, XReg as, XReg at)
{
    return detail::rrr(out, 0x0u, 0x3u, 0x9u, ar, as, at);
}

// slli ar, as, n  — 左シフト。n は 1..31。**0 は符号化できない。**
// 確認元: `slli a7, a7, 3` = d0 77 11 / `slli a3, a4, 31` = 10 34 01
//         `slli a3, a4, 16` = 00 34 11 / `slli a3, a4, 17` = f0 34 01
//
// **シフト量ではなく 32 - n が符号化される。** 下位 4bit が t フィールドへ、
// bit4 が op2 へ分かれる。n = 16 と n = 17 で op2 が切り替わるので、
// 分割を忘れると 16 を跨いだところで 16 だけずれた命令になる。
// n = 0 を渡すと 32 - 0 = 32 が t = 0 / op2 = 2 に化けて別の命令になるので、
// 呼び出し側は canSlli() で先に問うこと。
inline size_t slli(std::uint8_t* out, XReg ar, XReg as, std::uint32_t n)
{
    const std::uint32_t sa = 32u - n;
    return detail::rrr(out, 0x0u, 0x1u, (sa >> 4) & 0xFu, ar, as, sa & 0xFu);
}

inline constexpr bool canSlli(std::uint32_t n)
{
    return n >= 1u && n <= 31u;
}

// extui ar, at, shiftimm, maskimm  — at を shiftimm ビット右へ寄せ、下位 maskimm ビットを残す。
// shiftimm は 0..31、maskimm は 1..16。MOVE.b / MOVE.w のサイズ切り出しに使う。
//
// **op1 に maskimm-1、op2 に 0x4|shiftimm[4] が入る。** 素直に読むと逆に置きたくなる
// ところで、実際 1,412 通りの突き合わせで最初に落ちたのがここ。
// 確認元: `extui a3, a4, 0, 8` = 40 30 74 / `extui a15, a14, 24, 8` = e0 f8 75
//         `extui a0, a1, 31, 1` = 10 0f 05 / `extui a7, a8, 16, 16` = 80 70 f5
//
// shiftimm の下位 4bit が s フィールドへ、bit4 が op2 の最下位ビットへ分かれる。
// この分割を忘れると shiftimm 16 以上が 16 だけ小さい値に化ける。
inline size_t extui(std::uint8_t* out, XReg ar, XReg at, std::uint32_t shiftimm,
                    std::uint32_t maskimm)
{
    const std::uint32_t op2 = 0x4u | ((shiftimm >> 4) & 0x1u);
    return detail::rrr(out, 0x0u, op2, (maskimm - 1u) & 0xFu, ar, shiftimm & 0xFu, at);
}

// extui が受けられる組み合わせか。
//
// **maskimm の上限は 16。** 32bit のうち上位 24 ビットを残そうとして
// maskimm = 24 を渡すと (24 - 1) & 0xF = 7 に折り返し、下位 8 ビットだけを
// 残す**別の正当な命令**になる。呼び出し側で先に問うこと。
// shiftimm + maskimm > 32 もアセンブラが拒否する組み合わせなので弾く。
inline constexpr bool canExtui(std::uint32_t shiftimm, std::uint32_t maskimm)
{
    return shiftimm <= 31u && maskimm >= 1u && maskimm <= 16u && (shiftimm + maskimm) <= 32u;
}

// ---------------------------------------------------------------------------
// 分岐 / ジャンプ
//
// 変位はすべて **命令の先頭アドレス + 4** を基準にする。命令長 (2 か 3) ではなく
// 定数の 4 なので、3 バイト命令なら「次の命令の 1 バイト先」が原点になる。
// 呼び出し側は `imm = target - (insnPc + 4)` を渡す。
// ---------------------------------------------------------------------------

// 分岐変位の基準。target からこれを引いた値を各関数へ渡す。
inline constexpr std::int32_t kBranchOrigin = 4;

// beqz as, target  — as が 0 なら分岐。変位は 12bit 符号付き (-2048..2047)。
// 確認元: `beqz a3, +34` = 16 23 02 / `beqz a3, -8` = 16 83 ff / `beqz a4, +99` = 16 34 06
//
// BRI12 形式: op0=0x6 | op1[7:4]=0x1 | s[11:8] | imm12[23:12]
inline size_t beqz(std::uint8_t* out, XReg as, std::int32_t imm)
{
    return detail::emit24(out, 0x6u | (0x1u << 4) | (detail::nib(as) << 8) |
                                   ((static_cast<std::uint32_t>(imm) & 0xFFFu) << 12));
}

// bnez as, target  — as が 0 でなければ分岐。変位は 12bit 符号付き。
// 確認元: `bnez a3, +31` = 56 f3 01 / `bnez a5, -1` = 56 f5 ff / `bnez a15, +25` = 56 9f 01
inline size_t bnez(std::uint8_t* out, XReg as, std::int32_t imm)
{
    return detail::emit24(out, 0x6u | (0x5u << 4) | (detail::nib(as) << 8) |
                                   ((static_cast<std::uint32_t>(imm) & 0xFFFu) << 12));
}

inline constexpr bool canBranch12(std::int32_t imm)
{
    return imm >= -2048 && imm <= 2047;
}

// beq as, at, target  — 等しければ分岐。変位は **8bit** 符号付き (-128..127)。
// 確認元: `beq a3, a4, +22` = 47 13 16 / `beq a2, a3, -1` = 37 12 ff
//
// BRI8 形式: op0=0x7 | t[7:4] | s[11:8] | r[15:12]=0x1 | imm8[23:16]
// **beqz/bnez より変位が短い。** 同じ「分岐」でも届く距離が 16 分の 1 なので、
// 段 2 でブロックが伸びたときに先に溢れるのはこちら。
inline size_t beq(std::uint8_t* out, XReg as, XReg at, std::int32_t imm)
{
    return detail::rri8(out, 0x7u, 0x1u, as, at, static_cast<std::uint32_t>(imm) & 0xFFu);
}

// bne as, at, target  — 等しくなければ分岐。変位は 8bit 符号付き。
// 確認元: `bne a3, a4, +19` = 47 93 13 / `bne a15, a14, +13` = e7 9f 0d
inline size_t bne(std::uint8_t* out, XReg as, XReg at, std::int32_t imm)
{
    return detail::rri8(out, 0x7u, 0x9u, as, at, static_cast<std::uint32_t>(imm) & 0xFFu);
}

inline constexpr bool canBranch8(std::int32_t imm)
{
    return imm >= -128 && imm <= 127;
}

// j target  — 無条件ジャンプ。変位は 18bit 符号付き (-131072..131071)。
// 確認元: `j -4` = 06 ff ff / `j 0` = 06 00 00 / `j -19` = 46 fb ff
//
// **下位 6bit は 0x06。** CALL 形式 (call0 などの 0x05) と 1 違いで、取り違えると
// サブルーチン呼び出しになって a0 を潰す。
inline size_t j(std::uint8_t* out, std::int32_t imm)
{
    return detail::emit24(out, 0x6u | ((static_cast<std::uint32_t>(imm) & 0x3FFFFu) << 6));
}

inline constexpr bool canJump(std::int32_t imm)
{
    return imm >= -131072 && imm <= 131071;
}

// ---------------------------------------------------------------------------
// 呼び出し / 復帰
// ---------------------------------------------------------------------------

// callx0 as  — as の指すアドレスへ call0 で飛ぶ。戻りアドレスは a0 へ入る。
// 確認元: `callx0 a3` = c0 03 00 / `callx0 a8` = c0 08 00 / `callx0 a0` = c0 00 00
inline size_t callx0(std::uint8_t* out, XReg as)
{
    return detail::emit24(out, 0x0000C0u | (static_cast<std::uint32_t>(detail::nib(as)) << 8));
}

// ret.n  — call0 の復帰 (a0 へ飛ぶ)。
// 確認元: `ret.n` = 0d f0
inline size_t retN(std::uint8_t* out)
{
    return detail::emit16(out, 0xF00Du);
}

}  // namespace x68k::jit

#endif  // X68K_PLATFORM_JIT_XTENSA_ENCODER_H
