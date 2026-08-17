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
//
// 命令長として 0 は起こりえない (最短の命令が 2 バイト) ので、
// 命令長の番兵としては 0 で構わない。
inline constexpr u32 kUnknownLength = 0;

// 拡張ワード数が分からないことを表す。
//
// **命令長の番兵と分けてある。** 拡張ワード数は 0 が正当な値
// (Dn や (An) は拡張ワードを持たない) なので、kUnknownLength と
// 同じ 0 を使うと「拡張ワード無し」と「不明」を区別できない。
//
// 区別できなかったとき何が起きたか: レジスタ間 MOVE (MOVE.w D0,D1 など)
// が軒並み「翻訳できない」と判定され、実ワークロードでの MOVE の
// 翻訳率が 10.3% しかなかった。ブロックが 1〜2 命令で途切れる原因が
// これで、デコーダ自体は正しいのに「ブロックは効かない」と誤って
// 結論していた。
inline constexpr u32 kUnknownExtensionWords = 0xFFFFFFFFu;

// 実効アドレスが消費する拡張ワード数。size は 1/2/4 バイト。
// 使えないモードの組み合わせには kUnknownExtensionWords を返す。
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
                    return kUnknownExtensionWords;
            }
        default:
            return kUnknownExtensionWords;
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
            const bool srcUnknown = srcWords == kUnknownExtensionWords;
            const bool dstUnknown = dstWords == kUnknownExtensionWords;
            if (srcUnknown || dstUnknown)
            {
                return kUnknownLength;
            }
            // 転送先に PC 相対 (mode 7 reg 2/3) は来ない。来たら不正命令。
            const bool dstIsPcRelative = dstMode == 7 && (dstReg == 2 || dstReg == 3);
            if (dstIsPcRelative)
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

        case 0x4:
        {
            // $4 は実行の 32.3% を占める最大のグループ。実行頻度で測った
            // 内訳 (Human68k 稼働中、全体に対する割合):
            //   RTS 8.4% / TST,TAS 6.5% / MOVEM 6.1% / CLR 3.9%
            //   LEA 3.1% / JSR 2.4% / その他 1.9% / JMP,NOP,PEA,LINK ほぼ 0
            //
            // Why 形ごとに分けるか: $4 は「グループ」ではなく雑多な命令の
            // 寄せ集めで、実効アドレスを持つものと持たないものが混在する。
            // 上位ビットのパターンで振り分けるしかない。
            // 判定の順序と マスクは m68k_ops_group4.cpp の実装に合わせてある
            // (ずれると長さと実行が食い違う)。

            // 拡張ワードを持たない固定長の命令。
            const bool isRts = op == 0x4E75u;
            const bool isNop = op == 0x4E71u;
            const bool isRte = op == 0x4E73u;
            const bool isRtr = op == 0x4E77u;
            const bool isSwap = (op & 0xFFF8u) == 0x4840u;
            const bool isUnlk = (op & 0xFFF8u) == 0x4E58u;
            const bool isExt = (op & 0xFFB8u) == 0x4880u;
            if (isRts || isNop || isRte || isRtr || isSwap || isUnlk || isExt)
            {
                return 2;
            }

            // LINK は 16bit の変位が続く。
            const bool isLink = (op & 0xFFF8u) == 0x4E50u;
            if (isLink)
            {
                return 4;
            }

            // MOVEM はレジスタマスクを 1 ワード読んでから実効アドレス。
            //
            // Why EXT より後に見るか: EXT.W ($4880-$4887) は MOVEM の
            // マスク ($FB80) にも当たる。実装側も mode != 0 で切り分けて
            // いるので、同じ順序にする (逆にすると EXT を MOVEM と誤り、
            // 長さが 2 バイト過大になる)。
            const bool isMovem = (op & 0xFB80u) == 0x4880u && mode != 0;
            if (isMovem)
            {
                const u32 words = eaExtensionWords(mode, reg, 2);
                if (words == kUnknownExtensionWords)
                {
                    return kUnknownLength;
                }
                return 4 + words * 2;
            }

            // LEA <ea>,An : 0100 rrr 111 mmm rrr
            const bool isLea = (op & 0xF1C0u) == 0x41C0u;
            // JSR / JMP : 0100 1110 10/11 mmm rrr
            const bool isJsr = (op & 0xFFC0u) == 0x4E80u;
            const bool isJmp = (op & 0xFFC0u) == 0x4EC0u;
            const bool isPea = (op & 0xFFC0u) == 0x4840u;
            if (isLea || isJsr || isJmp || isPea)
            {
                // どれも実効アドレスを 1 つ取り、サイズはロング相当。
                // ただし即値 (mode 7 reg 4) は取れない。
                const bool isImmediate = mode == 7 && reg == 4;
                if (isImmediate)
                {
                    return kUnknownLength;
                }
                const u32 words = eaExtensionWords(mode, reg, 4);
                if (words == kUnknownExtensionWords)
                {
                    return kUnknownLength;
                }
                return 2 + words * 2;
            }

            // 単項演算 NEGX/CLR/NEG/NOT/TST : 0100 oooo ss mmm rrr
            //
            // **判別ビットは (op >> 8) & 0xF。** 実装 (m68k_ops_group4.cpp の
            // unary_ops) が switch している値と同じものを使う。
            // ここを (op >> 9) & 7 で書くと TST ($4A) が範囲から外れ、
            // 実行の 6.5% を占める命令を丸ごと取りこぼす。
            //
            // ss = 11 はサイズを持たない別命令 (TAS や $4Exx) なので除く。
            const u32 opcodeBits = static_cast<u32>((op >> 8) & 0xFu);
            const u32 sizeBits = static_cast<u32>((op >> 6) & 3u);
            const bool isUnaryOpcode = opcodeBits == 0x0 || opcodeBits == 0x2 ||
                                       opcodeBits == 0x4 || opcodeBits == 0x6 || opcodeBits == 0xA;
            const bool isUnary = isUnaryOpcode && sizeBits != 3;
            if (isUnary)
            {
                const u32 size = sizeBits == 0 ? 1u : (sizeBits == 1 ? 2u : 4u);
                // TST は読むだけなので PC 相対と即値も取れる。
                // 書き込む方 (NEGX/CLR/NEG/NOT) は取れない。
                const bool isReadOnly = opcodeBits == 0xA;
                const bool isImmediateOrPcRelative = mode == 7 && reg >= 2;
                if (!isReadOnly && isImmediateOrPcRelative)
                {
                    return kUnknownLength;
                }
                const u32 words = eaExtensionWords(mode, reg, size);
                if (words == kUnknownExtensionWords)
                {
                    return kUnknownLength;
                }
                return 2 + words * 2;
            }

            // TAS <ea> : 0100 1010 11 mmm rrr (byte のみ)
            const bool isTas = (op & 0xFFC0u) == 0x4AC0u;
            if (isTas)
            {
                const bool isImmediateOrPcRelative = mode == 7 && reg >= 2;
                if (isImmediateOrPcRelative)
                {
                    return kUnknownLength;
                }
                const u32 words = eaExtensionWords(mode, reg, 1);
                if (words == kUnknownExtensionWords)
                {
                    return kUnknownLength;
                }
                return 2 + words * 2;
            }

            // 残り (TRAP, MOVE to/from SR, CHK, ILLEGAL, RESET, STOP…) は
            // 扱わない。実行頻度の合計が 1.9% で、形が individually 違う。
            return kUnknownLength;
        }

        default:
            // ここに挙げていないグループは扱わない。
            //
            // Why 全部やらないか: 命令長を間違えるとブロックがずれる。
            // 残るのは $0 (即値/ビット操作 4.6%)、$D (ADD 4.7%)、
            // $B (CMP/EOR 3.8%) など。効果を測ってから広げる。
            return kUnknownLength;
    }
}

}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_LENGTH_H
