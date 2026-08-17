// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 命令長を命令語から求める。ブロックを組むための前提。
//
// ## なぜ要るか
//
// デコード済みブロックキャッシュを作ろうとして失敗した。原因は
// **68000 の命令長を静的に決められない**こと。命令は 2〜10 バイトで、
// 長さは実効アドレスのモードと拡張ワードの有無で決まる。
//
// 2 バイト固定で並べると 2 命令目以降の位置がずれ、「PC が想定と違えば
// 抜ける」という安全弁が毎回作動して 1 命令で抜ける。ブロックが機能せず、
// 毎命令デコードし直すことになって 100 倍以上遅くなった
// (docs/knowledge/event-driven-implementation.md)。
//
// ここを解けばブロックが組める。
//
// ## 何を返すか
//
// 命令語 (と、必要なら続く拡張ワード) から **命令全体のバイト数** を返す。
// 分からない命令には 0 を返し、呼び出し側はそこでブロックを打ち切る。
//
// **正しさの基準は「実行時に進む量と一致すること」**。多く見積もっても
// 少なく見積もってもブロックがずれるので、一致しなければ 0 を返す方が安全。
// ブロックの実行は「PC が想定と違えば抜ける」で守られているので、
// 0 を返して打ち切っても正しさは損なわれない (遅くなるだけ)。
//
// ## 実効アドレスのモードと拡張ワード
//
// m68k.cpp の effectiveAddressSlow / readEaSlow が読む数と一致させる:
//
//   mode 0-4        拡張ワード無し
//   mode 5 (d16,An) 1 ワード
//   mode 6 (d8,An,Xn) 1 ワード
//   mode 7 reg 0 (xxx).W    1 ワード
//   mode 7 reg 1 (xxx).L    2 ワード
//   mode 7 reg 2 (d16,PC)   1 ワード
//   mode 7 reg 3 (d8,PC,Xn) 1 ワード
//   mode 7 reg 4 #immediate サイズによる (byte/word は 1、long は 2)

#ifndef X68K_CORE_CPU_M68K_LENGTH_H
#define X68K_CORE_CPU_M68K_LENGTH_H

#include "m68k_types.h"

namespace x68k
{

// 命令長が分からないことを表す。
inline constexpr u32 kUnknownLength = 0;

// 実効アドレスが消費する拡張ワード数。size は 1/2/4 バイト。
// 使えないモードの組み合わせには kUnknownLength を返す。
inline u32 eaExtensionWords(u32 mode, u32 reg, u32 size)
{
    switch (mode)
    {
        case 0:  // Dn
        case 1:  // An
        case 2:  // (An)
        case 3:  // (An)+
        case 4:  // -(An)
            return 0;
        case 5:  // (d16,An)
        case 6:  // (d8,An,Xn)
            return 1;
        case 7:
            switch (reg)
            {
                case 0:  // (xxx).W
                    return 1;
                case 1:  // (xxx).L
                    return 2;
                case 2:  // (d16,PC)
                case 3:  // (d8,PC,Xn)
                    return 1;
                case 4:  // #immediate
                    return size == 4 ? 2u : 1u;
                default:
                    return kUnknownLength;
            }
        default:
            return kUnknownLength;
    }
}

// 命令全体のバイト数を返す。分からなければ kUnknownLength。
//
// **保守的に振る舞う。** 少しでも怪しければ 0 を返す。ブロックが短く
// なるだけで、正しさは損なわれない。
inline u32 instructionLength(u16 op)
{
    const u32 group = static_cast<u32>(op >> 12);
    const u32 mode = static_cast<u32>((op >> 3) & 7u);
    const u32 reg = static_cast<u32>(op & 7u);

    switch (group)
    {
        case 0x1:  // MOVE.b
        case 0x2:  // MOVE.l
        case 0x3:  // MOVE.w
        {
            // 転送元と転送先の両方が実効アドレスを持つ。
            // 転送先のモードとレジスタはビットの並びが逆になっている。
            const u32 size = group == 0x1 ? 1u : (group == 0x2 ? 4u : 2u);
            const u32 srcWords = eaExtensionWords(mode, reg, size);
            const u32 dstMode = static_cast<u32>((op >> 6) & 7u);
            const u32 dstReg = static_cast<u32>((op >> 9) & 7u);
            // 転送先に即値は来ない。来たら不正命令なので 0 を返す。
            const bool dstIsImmediate = dstMode == 7 && dstReg == 4;
            if (dstIsImmediate)
            {
                return kUnknownLength;
            }
            const u32 dstWords = eaExtensionWords(dstMode, dstReg, size);
            if (srcWords == kUnknownLength && !(mode == 7 && reg > 4))
            {
                return kUnknownLength;
            }
            if (dstWords == kUnknownLength && !(dstMode == 7 && dstReg > 4))
            {
                return kUnknownLength;
            }
            if ((mode == 7 && reg > 4) || (dstMode == 7 && dstReg > 4))
            {
                return kUnknownLength;
            }
            return 2 + (srcWords + dstWords) * 2;
        }

        case 0x7:  // MOVEQ。拡張ワード無し
            // bit8 が立っていると MOVEQ ではない (不正)。
            return (op & 0x0100u) == 0 ? 2u : kUnknownLength;

        case 0x6:  // Bcc / BRA / BSR
        {
            // 変位が 8bit なら 2 バイト、0 なら 16bit 変位が続く。
            // $FF (32bit 変位) は 68020 以降なので扱わない。
            const u32 disp8 = static_cast<u32>(op & 0xFFu);
            if (disp8 == 0)
            {
                return 4;
            }
            if (disp8 == 0xFF)
            {
                return kUnknownLength;
            }
            return 2;
        }

        default:
            // ここに挙げていないグループは扱わない。
            //
            // Why 全部やらないか: 命令長を間違えるとブロックがずれる。
            // 実測で分布の上位を占めるのは misc (32.7%) と分岐 (24.4%) と
            // MOVE (22.5%) だが、**misc は命令ごとに形が違いすぎる**
            // (MOVEM のレジスタマスク、LEA、JSR、単項演算…)。
            // まず確実に分かるものだけで組み、効果が出てから広げる。
            return kUnknownLength;
    }
}

}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_LENGTH_H
