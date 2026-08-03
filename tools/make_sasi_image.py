#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Human68k の配布ファイル群から、SASI から起動できる HDD イメージを作る。

シャープが無償公開している Human68k 3.02 は LZH で配られ、展開すると
human.sys / command.x などのファイルがそのまま出てくる。フロッピーの
イメージではないので、起動可能なディスクを自分で組み立てる必要がある。

なぜ FDC ではなく SASI から起動するのか:
    FDC (uPD72065) はデータ転送が DMAC 経由になるうえ、ドライブの
    状態機械も要る。SASI なら $C2 / READ 程度で足りる。

IPL-ROM がディスクに求めるもの (実物の ROM を逆アセンブルして確かめた):

    1. LBA 4 の先頭 4 バイトが "X68K" ($5836384B)。
       IPL-ROM は LBA 4 から 256 バイトを読み ($FF91E6)、
       $FF91FA の CMPI.L で識別子を検査する。ここが合わないと
       「起動できないディスク」として次のデバイスへ行ってしまう。

    2. LBA 0 から 4 セクタ (1024 バイト) が $002000 へ読み込まれ、
       そのまま実行される。これがブートコード本体。

    LBA 4 が識別用で LBA 0 が実行される、という順序が直感に反するので
    注意する。

生成するイメージの構造:

    LBA 0-3   : ブートコード。$002000 へ読み込まれて実行される
    LBA 4     : 識別子 "X68K" とディスクの諸元
    LBA 8-    : HUMAN.SYS 以下のファイル

重要な制約:
    HUMAN.SYS は連続したセクタに置く。ブートコードは FAT のチェーンを
    辿らず、開始セクタから連続して読むだけだから。このツールが連続配置を
    保証する。

使い方:
    just make-hdd /path/to/human302 rom/human.hdf
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# SASI のセクタ長。X68000 の SASI HDD は 256 バイト/セクタ。
SASI_SECTOR_SIZE = 256

# ブートコードが置かれる位置。IPL-ROM が $002000 へ読み込んで実行する。
BOOT_CODE_LBA = 0
BOOT_CODE_SECTORS = 4
BOOT_CODE_LOAD_ADDR = 0x002000

# 識別子が置かれる位置。IPL-ROM が最初に読んで検査する。
BOOT_ID_LBA = 4
BOOT_MAGIC = b"X68K"

# IPL-ROM が「小さすぎる」と判断する下限 ($FF920E の比較値)。
MIN_TOTAL_SECTORS = 0x9FD9

# 生成する HDD の容量。Human68k のシステム一式が入れば十分。
DEFAULT_HDD_BYTES = 20 * 1024 * 1024

# ファイルを置き始める位置。ブートコードと識別子の後ろに余裕を取る。
FILE_AREA_LBA = 8

# HUMAN.SYS を読み込むアドレス。Human68k の実行ファイルヘッダが
# 指定するロードアドレスに合わせる。
HUMAN_LOAD_ADDR = 0x006800


def build_boot_code(human_lba: int, human_sectors: int, entry: int) -> bytes:
    """HUMAN.SYS を読み込んで実行する 68000 のコードを組む。

    IOCS の B_READ ($47) を使ってセクタを読む。ブートコードが動く時点で
    IOCS ベクタは既に張られており、SASI のドライバも使える状態にある。

    先頭バイトは $60 (BRA) でなければならない。IPL-ROM は $002000 へ
    読み込んだ後、$FF02BE の CMPI.B #$60,(A1) でここを検査し、違えば
    JMP せずに次の起動デバイスへ行ってしまう。「先頭は必ず分岐命令」
    という約束で、壊れたセクタを実行しないようにしている。

    Why not アセンブラを devShell に足すか:
        やることが「連続セクタを読んで飛ぶ」だけで数十バイトにしかならない。
        アセンブラを 1 つ増やすより、命令ごとに注釈を付けたバイト列として
        持つ方が依存が減り、何をしているかも追いやすい。
    """
    code = bytearray()

    def emit(data: bytes, comment: str) -> None:
        code.extend(data)
        _ = comment  # 注釈はソースを読む人のためのもの

    # IPL-ROM が要求する先頭の分岐。
    #
    # BRA.W の変位は「オペコードの次のワードの位置」からの相対。$002000 に
    # 置かれるので、$002000+2+disp が飛び先になる。直後の $002004 へ
    # 行きたいので disp = 2。
    emit(b"\x60\x00\x00\x02", "BRA.W *+4")

    # IOCS B_READ ($47) は次のレジスタを取る。
    #   D1.L = デバイス番号 ($8000 + SASI の ID)
    #   D2.L = 開始レコード番号
    #   D3.L = 読み込むバイト数
    #   A1   = 読み込み先アドレス
    emit(b"\x22\x3c" + struct.pack(">I", 0x00008000), "MOVE.L #$8000,D1  ; SASI 0")
    emit(b"\x24\x3c" + struct.pack(">I", human_lba), "MOVE.L #human_lba,D2")
    emit(
        b"\x26\x3c" + struct.pack(">I", human_sectors * SASI_SECTOR_SIZE),
        "MOVE.L #bytes,D3",
    )
    emit(b"\x22\x7c" + struct.pack(">I", HUMAN_LOAD_ADDR), "MOVEA.L #$6800,A1")
    emit(b"\x70\x47", "MOVEQ #$47,D0     ; IOCS B_READ")
    emit(b"\x4e\x4f", "TRAP #15")

    # 読み込んだ HUMAN.SYS へ飛ぶ。イメージ生成時にヘッダを外して
    # メモリ上の姿にしてあるので、ヘッダぶんの補正は要らない。
    emit(b"\x4e\xf9" + struct.pack(">I", entry), "JMP entry")

    return bytes(code)


def build_id_sector(total_sectors: int) -> bytes:
    """IPL-ROM が最初に読む識別セクタを組む。"""
    sector = bytearray(SASI_SECTOR_SIZE)
    sector[0:4] = BOOT_MAGIC
    struct.pack_into(">I", sector, 4, total_sectors)
    return bytes(sector)


class XFile:
    """Human68k の X 形式実行ファイル。

    先頭 64 バイトがヘッダで、その後ろに text / data / 再配置表が続く。
    ブートコードに解釈させると分量が増えるので、ここでメモリ上の姿へ
    展開してしまい、ブートコードは「読んで飛ぶ」だけにする。
    """

    HEADER_SIZE = 64
    MAGIC = b"HU"

    def __init__(self, data: bytes):
        if data[0:2] != self.MAGIC:
            raise ValueError("X 形式の実行ファイルではありません")

        self.load_addr = struct.unpack_from(">I", data, 0x04)[0]
        self.entry = struct.unpack_from(">I", data, 0x08)[0]
        self.text_size = struct.unpack_from(">I", data, 0x0C)[0]
        self.data_size = struct.unpack_from(">I", data, 0x10)[0]
        self.bss_size = struct.unpack_from(">I", data, 0x14)[0]
        self.reloc_size = struct.unpack_from(">I", data, 0x18)[0]
        self._raw = data

    def to_memory_image(self, base: int) -> bytes:
        """指定アドレスへ置いたときのメモリ上の姿を返す。

        base がヘッダの load_addr と違えば再配置を適用する。同じなら
        再配置表を読む必要はない。
        """
        body_start = self.HEADER_SIZE
        body_end = body_start + self.text_size + self.data_size
        image = bytearray(self._raw[body_start:body_end])
        image.extend(bytes(self.bss_size))

        if base == self.load_addr:
            return bytes(image)

        # 再配置表は「前の位置からの差分」の並び。差分が 1 のときは
        # 次の 4 バイトが 32bit の差分を表す、という約束になっている。
        delta = base - self.load_addr
        reloc = self._raw[body_end : body_end + self.reloc_size]
        pos = 0
        offset = 0
        first = True
        while pos + 1 < len(reloc):
            step = struct.unpack_from(">H", reloc, pos)[0]
            pos += 2
            if step == 1:
                step = struct.unpack_from(">I", reloc, pos)[0]
                pos += 4
            offset = step if first else offset + step
            first = False
            if offset + 4 > len(image):
                break
            value = struct.unpack_from(">I", image, offset)[0]
            struct.pack_into(">I", image, offset, (value + delta) & 0xFFFFFFFF)

        return bytes(image)


def collect_files(source: Path) -> list[tuple[str, bytes]]:
    """イメージへ入れるファイルを集める。

    Human68k の起動に要るのは HUMAN.SYS と COMMAND.X。
    """
    wanted = ["human.sys", "command.x"]
    files: list[tuple[str, bytes]] = []

    for name in wanted:
        path = source / name
        if not path.exists():
            raise FileNotFoundError(f"{name} が {source} にありません")
        files.append((name.upper(), path.read_bytes()))

    return files


def build_image(source: Path, output: Path, hdd_bytes: int) -> None:
    total_sectors = hdd_bytes // SASI_SECTOR_SIZE
    if total_sectors < MIN_TOTAL_SECTORS:
        raise ValueError(
            f"容量が小さすぎます: {total_sectors} セクタ "
            f"(IPL-ROM は {MIN_TOTAL_SECTORS} 以上を要求する)"
        )

    image = bytearray(hdd_bytes)
    files = collect_files(source)

    # HUMAN.SYS はメモリ上の姿へ展開してから置く。
    #
    # Why not 生のファイルをそのまま置くか: X 形式は先頭 64 バイトが
    # ヘッダで、その後ろに text / data / 再配置表が続く。生のまま
    # $6800 へ読み込むとヘッダごと実行してしまい、"HU" ($4855) が
    # 命令として解釈されて即座に暴走する。ブートコードにヘッダを
    # 解釈させると分量が増えるので、ここで展開しておく。
    human_name, human_raw = files[0]
    human = XFile(human_raw)
    human_data = human.to_memory_image(HUMAN_LOAD_ADDR)

    human_lba = FILE_AREA_LBA
    offset = human_lba * SASI_SECTOR_SIZE
    image[offset : offset + len(human_data)] = human_data

    human_sectors = (len(human_data) + SASI_SECTOR_SIZE - 1) // SASI_SECTOR_SIZE
    next_lba = human_lba + human_sectors

    placed = [(human_name, human_lba, len(human_data))]
    for name, data in files[1:]:
        offset = next_lba * SASI_SECTOR_SIZE
        image[offset : offset + len(data)] = data
        placed.append((name, next_lba, len(data)))
        next_lba += (len(data) + SASI_SECTOR_SIZE - 1) // SASI_SECTOR_SIZE

    boot_code = build_boot_code(human_lba, human_sectors, human.entry)
    if len(boot_code) > BOOT_CODE_SECTORS * SASI_SECTOR_SIZE:
        raise ValueError(f"ブートコードが長すぎます: {len(boot_code)} バイト")
    offset = BOOT_CODE_LBA * SASI_SECTOR_SIZE
    image[offset : offset + len(boot_code)] = boot_code

    id_sector = build_id_sector(total_sectors)
    offset = BOOT_ID_LBA * SASI_SECTOR_SIZE
    image[offset : offset + len(id_sector)] = id_sector

    output.write_bytes(bytes(image))

    print(f"{output} を生成しました ({hdd_bytes} バイト / {total_sectors} セクタ)")
    print(
        f"  ブートコード: LBA {BOOT_CODE_LBA} / {len(boot_code)} バイト "
        f"→ ${BOOT_CODE_LOAD_ADDR:06X} で実行"
    )
    print(f"  識別子: LBA {BOOT_ID_LBA}")
    for name, lba, size in placed:
        print(f"  {name}: LBA {lba} / {size} バイト")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Human68k を展開したディレクトリ")
    parser.add_argument("output", type=Path, help="生成する SASI HDD イメージ")
    parser.add_argument(
        "--size",
        type=int,
        default=DEFAULT_HDD_BYTES,
        help=f"HDD の容量 (バイト、既定 {DEFAULT_HDD_BYTES})",
    )
    args = parser.parse_args()

    if not args.source.is_dir():
        print(f"ディレクトリが見つかりません: {args.source}", file=sys.stderr)
        return 1

    try:
        build_image(args.source, args.output, args.size)
    except (FileNotFoundError, ValueError) as e:
        print(f"エラー: {e}", file=sys.stderr)
        return 1

    print()
    print("次の手順:")
    print(f"  just run --hdd {args.output} --ppm /tmp/boot.ppm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
