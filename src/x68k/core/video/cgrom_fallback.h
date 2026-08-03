// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// CGROM が無いときに、IPL-ROM 内蔵の 6x12 ANK フォントで代用する。
//
// なぜ必要か:
//   X68000 のテキスト画面は 4 プレーン x 1bit/dot のビットマップで、IOCS は
//   CGROM ($F00000 台) から字形を読んでテキスト VRAM へ直接ドットを描く。
//   CGROM はシャープの無償公開の対象外 (漢字フォントのベンダ権利のため) なので
//   ユーザーが用意できないことがある。無いまま起動すると IOCS は 0 ではなく
//   「読めた値」をそのまま字形として使うため、画面がベタ塗りの矩形になり、
//   Human68k が何を出しているのか読めない。
//
// Why not IOCS を横取りする方式にしなかったか:
//   IOCS の文字描画コール ($FF ...) をエミュレータ側でフックして自前で描く手も
//   あるが、Human68k と IOCS の内部ワーク (カーソル位置・スクロール・アトリビュート)
//   をすべて再現する羽目になる。CGROM の読み出しだけを肩代わりすれば、描画の
//   段取りは実物の IOCS がそのままやってくれる。差し替える面が小さいほうが安全。
//
// Why not IPL-ROM 内のフォントを直接 $F00000 台へ写像しなかったか:
//   IPL-ROM のフォントは 6x12 (1 ライン 1 バイト、上位 6bit)、CGROM の ANK は
//   8x16 (1 ライン 1 バイト) で、寸法も 1 文字あたりのバイト数も違う。
//   そのまま重ねると字形がずれる。ここで 8x16 のコマへ描き直す。

#ifndef X68K_CORE_VIDEO_CGROM_FALLBACK_H
#define X68K_CORE_VIDEO_CGROM_FALLBACK_H

#include <cstddef>

#include "../cpu/m68k_types.h"
#include "../memmap.h"

namespace x68k
{

// IPL-ROM 内の 6x12 ANK フォントの実測値。
//
// メモリ $FFD018 (rom/iplrom.dat のオフセット $1D018) から、
// 1 文字 12 バイト (1 バイト = 1 ライン、上位 6bit が有効) が 254 文字ぶん並ぶ。
// 添字は文字コードそのもの (index 0 = コード $00)。
//
// memmap.h の kIplromAnk6x12Addr ($FFCFF6) は資料由来の値で、実物とは合わない。
// $FFCFF6 の位置には 68000 の命令列があり、フォントは $22 バイト後ろから始まる。
inline constexpr u32 kIplromAnk6x12Base = 0xFFD018u;
inline constexpr u32 kIplromAnk6x12Glyphs = 254u;
inline constexpr u32 kAnk6x12Width = 6u;
inline constexpr u32 kAnk6x12Height = 12u;
inline constexpr u32 kAnk6x12BytesPerGlyph = kAnk6x12Height;

// CGROM の 8x16 ANK フォントの位置。実測 (IOCS が実際に読みに来たアドレス)。
//
// 文字コード c の字形は $F3A800 + c * 16 から 16 バイト。1 バイトが 1 ライン、
// 最上位ビットが左端。IOCS の 1 文字描画はここを 16 回読んでテキスト VRAM の
// プレーン 0 へ書く。
inline constexpr u32 kCgromAnk8x16Offset = 0xF3A800u - kCgromBase;
inline constexpr u32 kCgromAnk8x16Height = 16u;
inline constexpr u32 kCgromAnk8x16Glyphs = 256u;

// IPL-ROM の 6x12 フォントから、CGROM の体裁をした代替イメージを組み立てる。
//
//   iplRom : IPL-ROM の先頭 (kIplromSize バイト)
//   out    : 書き込み先 (kCgromSize バイト)。全体を 0 で埋めてから字形を置く
//
// 8x16 のコマの中で 6x12 の字形を左寄せ・ベースライン合わせで置く。
// 上に 2 ライン、下に 2 ライン空ける (6x12 のフォント自体が上下 2 ラインを
// 空けているので、実際の字形は 8x16 のコマのほぼ中央に来る)。
//
// Why not 6x12 を 8x16 へ引き伸ばさないか:
//   ドットを複製して拡大すると縦横比が崩れて読みにくくなる。原寸のまま
//   置いたほうが英数字としては読める。字間が広がるが判読性を優先する。
void buildCgromFromIplRom(const u8* iplRom, u8* out);

// 代替 CGROM 全体の大きさ。呼び出し側が確保するときに使う。
inline constexpr std::size_t kCgromFallbackSize = kCgromSize;

}  // namespace x68k

#endif  // X68K_CORE_VIDEO_CGROM_FALLBACK_H
