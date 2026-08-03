#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Human68k のフロッピーイメージから SASI HDD イメージを作る。

シャープが無償公開している Human68k はフロッピーの形 (.xdf / .dim) で配られる。
一方 x68k-stackchan の起動デバイスは SASI なので、そのままでは使えない。
このツールが両者を橋渡しする。

なぜ FDC ではなく SASI から起動するのか:
    FDC (uPD72065) はデータ転送が DMAC 経由になるため、DMAC の実装も必要になる。
    SASI は TEST UNIT READY / READ / WRITE / REQUEST SENSE 程度で足りるので、
    最小の起動経路としてはこちらが有利。

生成するイメージの構造:
    セクタ 0    : ブートセクタ (1024 バイト以内、先頭は BRA 命令)
    セクタ 4-   : FAT12 のファイルシステム (HUMAN.SYS ほか)

重要な制約:
    HUMAN.SYS は連続したセクタに配置されている必要がある。ブートセクタは
    FAT のチェーンを辿らず、開始クラスタから連続して読むだけだから。
    このツールは連続配置を保証する。

使い方:
    just make-hdd path/to/human302.xdf out.hdf
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# SASI のセクタ長。X68000 の SASI HDD は 256 バイト/セクタ。
SASI_SECTOR_SIZE = 256

# フロッピーのセクタ長。2HD は 1024 バイト/セクタ。
FD_SECTOR_SIZE_2HD = 1024
FD_SECTOR_SIZE_2DD = 512

# IPL-ROM がブートセクタとして読み込む量。
BOOT_SECTOR_BYTES = 1024

# 生成する HDD の容量。Human68k のシステム一式が入れば十分。
DEFAULT_HDD_BYTES = 20 * 1024 * 1024


class FloppyImage:
    """フロッピーイメージを読む。

    .xdf は生のセクタ列で、ヘッダを持たない。.dim は 256 バイトのヘッダが付く。
    ここでは両方を扱えるようにする。
    """

    def __init__(self, data: bytes):
        self.data = data
        self.sector_size = FD_SECTOR_SIZE_2HD

    @classmethod
    def load(cls, path: Path) -> FloppyImage:
        data = path.read_bytes()

        # .dim は先頭に 256 バイトのヘッダを持ち、その先頭バイトがメディア種別。
        # ヘッダの有無はサイズで見分ける (2HD は 1232640 バイトちょうど)。
        if len(data) % FD_SECTOR_SIZE_2HD == 256:
            data = data[256:]

        return cls(data)

    def read(self, offset: int, length: int) -> bytes:
        if offset + length > len(self.data):
            raise ValueError(
                f"フロッピーイメージが短すぎます: {offset}+{length} > {len(self.data)}"
            )
        return self.data[offset : offset + length]


def find_human_sys(fd: FloppyImage) -> tuple[int, int] | None:
    """フロッピーの FAT12 から HUMAN.SYS の位置と大きさを探す。

    戻り値は (オフセット, バイト数)。見つからなければ None。
    """
    # FAT12 の BPB はブートセクタの先頭にある。
    boot = fd.read(0, 512)
    bytes_per_sector = struct.unpack_from("<H", boot, 0x0B)[0]
    sectors_per_cluster = boot[0x0D]
    reserved_sectors = struct.unpack_from("<H", boot, 0x0E)[0]
    fat_count = boot[0x10]
    root_entries = struct.unpack_from("<H", boot, 0x11)[0]
    sectors_per_fat = struct.unpack_from("<H", boot, 0x16)[0]

    if bytes_per_sector == 0 or sectors_per_cluster == 0:
        return None

    root_start = (reserved_sectors + fat_count * sectors_per_fat) * bytes_per_sector
    root_bytes = root_entries * 32
    data_start = root_start + root_bytes

    root = fd.read(root_start, root_bytes)

    # ルートディレクトリから HUMAN.SYS を探す。
    # ブートセクタは先頭 32 エントリしか見ないので、そこに無ければ起動できない。
    for i in range(0, root_bytes, 32):
        entry = root[i : i + 32]
        if entry[0] in (0x00, 0xE5):
            continue
        name = entry[0:8].decode("ascii", errors="replace").strip()
        ext = entry[8:11].decode("ascii", errors="replace").strip()
        if name.upper() != "HUMAN" or ext.upper() != "SYS":
            continue

        cluster = struct.unpack_from("<H", entry, 0x1A)[0]
        size = struct.unpack_from("<I", entry, 0x1C)[0]
        offset = data_start + (cluster - 2) * sectors_per_cluster * bytes_per_sector
        return offset, size

    return None


def build_image(fd: FloppyImage, output: Path, hdd_bytes: int) -> None:
    """フロッピーの内容を SASI HDD イメージへ写す。

    方針: フロッピーのファイルシステムをそのまま HDD の先頭へ写す。
    ブートセクタも含めて丸ごとコピーすることで、IPL-ROM から見た構造を保つ。
    セクタ長がフロッピー (1024) と SASI (256) で違うが、どちらもバイト列
    としては連続しているので、そのまま並べれば論理セクタ番号が 4 倍になるだけ。
    """
    image = bytearray(hdd_bytes)

    # フロッピーの中身を先頭へ写す。
    if len(fd.data) > hdd_bytes:
        raise ValueError(
            f"フロッピーイメージ ({len(fd.data)} バイト) が "
            f"HDD の容量 ({hdd_bytes} バイト) を超えています"
        )
    image[0 : len(fd.data)] = fd.data

    output.write_bytes(bytes(image))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fd", type=Path, required=True, help="Human68k のフロッピーイメージ")
    parser.add_argument("--out", type=Path, required=True, help="生成する SASI HDD イメージ")
    parser.add_argument(
        "--size",
        type=int,
        default=DEFAULT_HDD_BYTES,
        help=f"HDD の容量 (バイト、既定 {DEFAULT_HDD_BYTES})",
    )
    args = parser.parse_args()

    if not args.fd.exists():
        print(f"フロッピーイメージが見つかりません: {args.fd}", file=sys.stderr)
        return 1

    fd = FloppyImage.load(args.fd)
    print(f"フロッピーイメージ: {len(fd.data)} バイト")

    found = find_human_sys(fd)
    if found is None:
        print(
            "警告: HUMAN.SYS が見つかりませんでした。"
            "ルートディレクトリの先頭 32 エントリに無いと起動できません。",
            file=sys.stderr,
        )
    else:
        offset, size = found
        print(f"HUMAN.SYS: オフセット {offset} / {size} バイト")

    build_image(fd, args.out, args.size)
    print(f"{args.out} を生成しました ({args.size} バイト)")
    print()
    print("次の手順:")
    print(f"  just run --hdd {args.out} --ppm /tmp/boot.ppm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
