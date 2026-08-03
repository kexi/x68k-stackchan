#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "pillow>=11",
# ]
# ///
"""IPL-ROM に埋め込まれた 6x12 ドットの ANK フォントを抽出して PNG にする。

CGROM はシャープの無償公開の対象外で入手できないことがあるが、6x12 の ANK
フォントは IPL-ROM の中にある。これを使えば CGROM なしでも Human68k の
コマンドラインを英数字で表示できる。

このスクリプトはその経路が成立するかを目視で確かめるためのもの。抽出結果を
グリフ一覧の PNG にして、英数字がちゃんと読めるかを人間が見る。

使い方:
    just extract-font path/to/iplrom.dat
    just extract-font path/to/iplrom.dat /tmp/font.png

注意: 文字数は 256 ではなく 254。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image

# src/x68k/core/memmap.h と同じ値。ここを変えるならあちらも直すこと。
IPLROM_BASE = 0xFE0000
IPLROM_SIZE = 0x20000
# 実測値。資料でよく挙がる $FFCFF6 には 68000 の命令列があり、フォントは
# その $22 バイト後ろから始まる。$FFD018 から 254 文字 x 12 バイトが並び
# $FFDC00 でちょうど終わる。
ANK_6X12_ADDR = 0xFFD018
ANK_6X12_COUNT = 254

GLYPH_WIDTH = 6
GLYPH_HEIGHT = 12
# 1 ライン 1 バイト (上位 6 bit が有効) × 12 ライン。
BYTES_PER_GLYPH = GLYPH_HEIGHT

# 一覧 PNG の並び。16 列にすると文字コードの上位/下位が読み取りやすい。
COLUMNS = 16
SCALE = 4


def extract_glyphs(rom: bytes) -> list[list[int]]:
    """IPL-ROM のバイト列から 254 個のグリフを取り出す。

    戻り値は各グリフの 12 バイト（1 バイト = 1 ライン、上位 6 bit が有効）。
    """
    offset = ANK_6X12_ADDR - IPLROM_BASE
    end = offset + ANK_6X12_COUNT * BYTES_PER_GLYPH
    if end > len(rom):
        raise ValueError(
            f"IPL-ROM が短すぎます: フォント末尾 {end} バイト目まで必要ですが "
            f"{len(rom)} バイトしかありません"
        )
    return [
        list(rom[offset + i * BYTES_PER_GLYPH : offset + (i + 1) * BYTES_PER_GLYPH])
        for i in range(ANK_6X12_COUNT)
    ]


def render_sheet(glyphs: list[list[int]]) -> Image.Image:
    """グリフ一覧を 1 枚の画像にする。白地に黒で描く。"""
    rows = (len(glyphs) + COLUMNS - 1) // COLUMNS
    # グリフ間に 1px の隙間を空けて、文字の境界が分かるようにする。
    cell_w = GLYPH_WIDTH + 1
    cell_h = GLYPH_HEIGHT + 1
    img = Image.new("L", (COLUMNS * cell_w, rows * cell_h), color=255)
    px = img.load()

    for index, glyph in enumerate(glyphs):
        gx = (index % COLUMNS) * cell_w
        gy = (index // COLUMNS) * cell_h
        for y, line in enumerate(glyph):
            for x in range(GLYPH_WIDTH):
                # 6 dot 幅なので最上位ビットから 6 bit を使う。
                if line & (0x80 >> x):
                    px[gx + x, gy + y] = 0

    return img.resize((img.width * SCALE, img.height * SCALE), Image.Resampling.NEAREST)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iplrom", type=Path, help="IPL-ROM のファイル (128KB)")
    parser.add_argument(
        "--out", type=Path, default=Path("/tmp/ank6x12.png"), help="出力する PNG のパス"
    )
    args = parser.parse_args()

    rom = args.iplrom.read_bytes()
    if len(rom) != IPLROM_SIZE:
        print(
            f"警告: IPL-ROM のサイズが {len(rom)} バイトです (期待値 {IPLROM_SIZE})。"
            "別のバージョンかもしれません。",
            file=sys.stderr,
        )

    glyphs = extract_glyphs(rom)
    sheet = render_sheet(glyphs)
    sheet.save(args.out)

    print(f"{len(glyphs)} グリフを {args.out} に書き出しました。")
    print("英数字が読める形になっているか目視で確認してください。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
