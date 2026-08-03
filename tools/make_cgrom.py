#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""東雲フォント (BDF) から X68000 の CGROM 相当のイメージを作る。

X68000 の CGROM (768KB) はシャープの無償公開の対象外で入手できない。
IOCS は $F00000 台から字形を読んでテキスト VRAM へ直接ドットを描くので、
CGROM が無いと漢字がまったく出ない。IPL-ROM 内蔵の 6x12 ANK フォント
(254 文字) で代用する経路はあるが、こちらは英数字しか持たない。

なぜ東雲フォントか:
    ライセンスに「自由な改造、他フォーマットへの変換、組込み、再配布を
    行うことができます」と明記された実質パブリックドメインで、BDF→CGROM
    変換と成果物の配布が明示的に許諾されている。すでにビットマップなので
    アウトラインからのラスタライズが要らず、品質が最もブレる工程を回避
    できる。JIS 第1・第2水準 6879 字を収録し、各サイズを人手でドット調整
    してある。

    Why not IPA フォントか:
        IPA フォントライセンス第1条3項の「派生プログラム」に該当し、
        再配布すると第3条1項が全部かかる。MIT との整合が崩れるうえ、
        ファイル名に "IPA" を含められない制約も付く。

    Why not KH ドットフォントか:
        平木敬太郎氏が X68000 向けにデザインした書体の復刻で、字形の
        由来は CGROM に最も近く、ライセンス (SIL OFL 1.1) も再配布可。
        ただし配布形式が TrueType のアウトラインなので、ラスタライズの
        工程が挟まる。BDF をそのまま読めば字形は設計どおりに出るので、
        まずは変換の確実さを採った。

CGROM の構造:

    | フォント    | オフセット | 絶対番地 | バイト/字 | 形式          |
    |-------------|-----------|----------|-----------|---------------|
    | 16x16 漢字  | $00000    | $F00000  | 32        | 2バイト×16行  |
    | 8x8 ANK     | $3A000    | $F3A000  | 8         | 1バイト×8行   |
    | 8x16 ANK    | $3A800    | $F3A800  | 16        | 1バイト×16行  |
    | 12x12       | $3B800    | $F3B800  | 24        | 2バイト×12行  |
    | 12x24       | $3D000    | $F3D000  | 48        | 2バイト×24行  |
    | 24x24 漢字  | $40000    | $F40000  | 72        | 3バイト×24行  |

    すべて MSB が左端・行は上から連続。

このツールが埋めるのは 16x16 漢字と 8x16 ANK だけ。Human68k の日本語表示は
このふたつでほぼ賄える。24x24 は東雲に該当サイズが無いので 0 のままにする。

    Why not 16x16 を 24x24 へ引き伸ばして埋めないか:
        ドット単位で調整されたビットマップを整数倍でない比率へ拡大すると
        線幅が不揃いになり、かえって読めなくなる。0 のままなら字形は
        空白として出る。ベタ塗りの矩形が並ぶより、何が出ていないのかが
        分かりやすい。

使い方:
    just make-cgrom /path/to/shinonome-0.9.11/bdf /tmp/cgrom.dat
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# CGROM 全体の大きさ。memmap.h の kCgromSize と同じ。
CGROM_SIZE = 0xC0000

# 各フォントの配置。オフセットと 1 文字あたりのバイト数。
KANJI16_OFFSET = 0x00000
KANJI16_BYTES = 32  # 2 バイト x 16 行
KANJI16_WIDTH = 16
KANJI16_HEIGHT = 16

ANK8X16_OFFSET = 0x3A800
ANK8X16_BYTES = 16  # 1 バイト x 16 行
ANK8X16_HEIGHT = 16
ANK8X16_GLYPHS = 256

# 漢字の 1 区あたりのバイト数。区の中に 0x5e (94) 文字が並ぶ。
TEN_PER_KU = 0x5E


def kanji_offset(ku: int, ten: int) -> int | None:
    """区点から 16x16 漢字領域内のオフセットを返す。範囲外なら None。

    教科書式の ``((ku-1)*94 + (ten-1)) * 32`` ではない点に注意する。
    CGROM には特殊字形用に予約された行があり、区 8-15 が飛んでいる:

        1 <= ku <= 7   → line = ku - 1        (line 0-6)
        16 <= ku <= 84 → line = (ku - 16) + 8 (line 8-76)

    line 7 は予約で、区 8-15 に対応する字形は CGROM に無い。この段差を
    見落とすと、区 16 以降の漢字がまるごと 94 字ぶんずれる。JIS 第1水準の
    先頭 (亜 = 区16点1) が別の字になるので、症状としては「漢字は出るが
    全部違う字」という分かりにくい壊れ方をする。

    Why not 連番で詰めないか:
        px68k の mkcgrom.c および実機の CGROM がこの配置になっている。
        IOCS は区点からこの式でアドレスを計算して読みに来るので、
        こちらが詰めて置くと IOCS の読み先と食い違う。
    """
    is_symbol_ku = 1 <= ku <= 7
    is_kanji_ku = 16 <= ku <= 84

    if is_symbol_ku:
        line = ku - 1
    elif is_kanji_ku:
        line = (ku - 16) + 8
    else:
        return None

    if not 1 <= ten <= TEN_PER_KU:
        return None

    return KANJI16_OFFSET + line * (TEN_PER_KU * KANJI16_BYTES) + (ten - 1) * KANJI16_BYTES


class BdfFont:
    """BDF から字形のビットマップを読む。

    BDF は 1 文字が STARTCHAR から ENDCHAR までのブロックで、BITMAP の
    後ろに 1 行 1 エントリの 16 進文字列が並ぶ。1 行のバイト数は
    ``ceil(BBX幅 / 8)`` で決まる (16 ドット幅なら "3fff" のように 2 バイト)。

    ここで読むのは ENCODING と BITMAP だけ。DWIDTH や SWIDTH は
    プロポーショナル配置のための情報で、固定ピッチの CGROM には要らない。
    """

    def __init__(self, path: Path):
        self.path = path
        self.glyphs: dict[int, list[int]] = {}
        self._parse()

    def _parse(self) -> None:
        # BDF は EUC-JP のコメントを含むことがある。字形の読み取りに
        # コメントは要らないので、latin-1 で読んで復号の失敗を避ける。
        #
        # Why not EUC-JP で読まないか:
        #   COMMENT 行以外は ASCII だけで書かれている。文字コードを
        #   当てにいくと、フォントによって失敗しうる。latin-1 は
        #   任意のバイト列を必ず復号できるので、解析が止まらない。
        text = self.path.read_text(encoding="latin-1")

        encoding: int | None = None
        bbx_width = 0
        bitmap: list[int] | None = None

        for raw in text.splitlines():
            line = raw.strip()

            if line.startswith("ENCODING "):
                encoding = int(line.split()[1])
                continue

            if line.startswith("BBX "):
                bbx_width = int(line.split()[1])
                continue

            if line == "BITMAP":
                bitmap = []
                continue

            if line == "ENDCHAR":
                is_usable = bitmap is not None and encoding is not None and encoding >= 0
                if is_usable:
                    self.glyphs[encoding] = bitmap  # type: ignore[assignment]
                encoding = None
                bitmap = None
                continue

            if bitmap is None:
                continue

            # BITMAP から ENDCHAR までの各行が 1 スキャンライン。
            # 16 進 2 桁で 1 バイト、左詰めで並ぶ。
            bytes_per_row = (bbx_width + 7) // 8
            value = int(line, 16)
            for i in range(bytes_per_row):
                shift = (bytes_per_row - 1 - i) * 8
                bitmap.append((value >> shift) & 0xFF)

    def rows(self, code: int, height: int, bytes_per_row: int) -> list[int] | None:
        """指定コードの字形を「1 行 bytes_per_row バイト」の並びで返す。

        BDF 側の行数が足りないときは None を返す。寸法の食い違いに
        気付かず途中まで書き込むと、字形が縦にずれた CGROM ができる。
        """
        bitmap = self.glyphs.get(code)
        if bitmap is None:
            return None
        if len(bitmap) < height * bytes_per_row:
            return None
        return bitmap[: height * bytes_per_row]


def fill_kanji16(image: bytearray, font: BdfFont) -> tuple[int, int]:
    """16x16 漢字領域を埋める。(書けた字数, 領域外で捨てた字数) を返す。

    BDF の ENCODING は JIS コードそのもの (区+0x20 を上位、点+0x20 を下位)。
    たとえば 亜 = 区16点1 は 0x3021 = 12321 で入っている。ここから区点を
    復元し、kanji_offset() で CGROM 内の位置を決める。
    """
    written = 0
    skipped = 0

    for code, _ in font.glyphs.items():
        ku = (code >> 8) - 0x20
        ten = (code & 0xFF) - 0x20

        offset = kanji_offset(ku, ten)
        if offset is None:
            skipped += 1
            continue

        rows = font.rows(code, KANJI16_HEIGHT, KANJI16_WIDTH // 8)
        if rows is None:
            skipped += 1
            continue

        image[offset : offset + KANJI16_BYTES] = bytes(rows)
        written += 1

    return written, skipped


def fill_ank8x16(image: bytearray, font: BdfFont) -> int:
    """8x16 ANK 領域を埋める。書けた字数を返す。

    添字は文字コードそのもの。JIS X 0201 の BDF を使うので、$A1-$DF に
    半角カタカナが入る。Human68k の画面下部のファンクションキー表示は
    ここを読むため、ISO8859-1 版では半角カナが出ない。
    """
    written = 0

    for code in range(ANK8X16_GLYPHS):
        rows = font.rows(code, ANK8X16_HEIGHT, 1)
        if rows is None:
            continue

        offset = ANK8X16_OFFSET + code * ANK8X16_BYTES
        image[offset : offset + ANK8X16_BYTES] = bytes(rows)
        written += 1

    return written


def build_cgrom(bdf_dir: Path, output: Path) -> None:
    kanji_path = bdf_dir / "shnmk16.bdf"
    ank_path = bdf_dir / "shnm8x16r.bdf"

    for path in (kanji_path, ank_path):
        if not path.exists():
            raise FileNotFoundError(f"{path.name} が {bdf_dir} にありません")

    # 埋めない領域は 0 のままにする (24x24 漢字、8x8 / 12x12 / 12x24)。
    image = bytearray(CGROM_SIZE)

    kanji_font = BdfFont(kanji_path)
    kanji_written, kanji_skipped = fill_kanji16(image, kanji_font)

    ank_font = BdfFont(ank_path)
    ank_written = fill_ank8x16(image, ank_font)

    output.write_bytes(bytes(image))

    print(f"{output} を生成しました ({CGROM_SIZE} バイト)")
    print(f"  16x16 漢字 (${KANJI16_OFFSET:05X}): {kanji_written} 字 / {kanji_path.name}")
    if kanji_skipped:
        print(f"    区点の範囲外で置かなかった字形: {kanji_skipped}")
    print(f"  8x16 ANK  (${ANK8X16_OFFSET:05X}): {ank_written} 字 / {ank_path.name}")
    print("  24x24 漢字 ($40000): 東雲に該当サイズが無いので 0 のまま")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bdf_dir", type=Path, help="東雲フォントの bdf/ ディレクトリ")
    parser.add_argument("output", type=Path, help="生成する CGROM イメージ")
    args = parser.parse_args()

    if not args.bdf_dir.is_dir():
        print(f"ディレクトリが見つかりません: {args.bdf_dir}", file=sys.stderr)
        return 1

    try:
        build_cgrom(args.bdf_dir, args.output)
    except (FileNotFoundError, ValueError) as e:
        print(f"エラー: {e}", file=sys.stderr)
        return 1

    print()
    print("次の手順:")
    print(f"  just run --cgrom {args.output} --hdd /tmp/human.hdf --ppm /tmp/kanji.ppm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
