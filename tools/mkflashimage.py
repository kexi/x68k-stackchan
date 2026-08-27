#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""ROM とディスクイメージを、flash へ焼ける 1 つのファイルにまとめる。

なぜ要るか:
    既定では IPL-ROM とディスクイメージを microSD から読む。差し替えが
    楽なのが利点だが、「カードを挿さずに動かしたい」「配る相手に
    カードの用意をさせたくない」ときに困る。

    flash には storage パーティション (12MB) が空いている。
    パーティション表のコメントにも「ROM を SD ではなく flash に
    焼きたくなった場合」に備えて確保してある、と書いてある。

ディスクを疎 (sparse) で持つ:
    SASI のイメージは IPL-ROM の要求で最低 10MB 必要だが、中身は
    ほとんどゼロ。Human68k 一式とゲームを入れても非ゼロは 150KB ほど。
    全部焼くと 10MB を無駄にするので、非ゼロのセクタだけ並べて
    「どのセクタが実体を持つか」を索引で持つ。

    読み出しは索引を引くだけ。索引に無いセクタはゼロを返す。
    SASI は 256 バイト/セクタなので、索引は 4 バイト x セクタ数で足りる。

出力の形式 (すべてビッグエンディアン。68000 に合わせる):
    オフセット 0  : "X68F"          マジック
    オフセット 4  : version (u32)   1
    オフセット 8  : ipl_offset      IPL-ROM の位置
    オフセット 12 : ipl_size
    オフセット 16 : cgrom_offset    無ければ 0
    オフセット 20 : cgrom_size
    オフセット 24 : disk_sectors    ディスク全体のセクタ数
    オフセット 28 : index_offset    索引の位置 (u32 x disk_sectors)
    オフセット 32 : data_offset     セクタ実体の位置
    オフセット 36 : data_sectors    実体を持つセクタの数

    索引の各要素は「そのセクタの実体が data の何番目にあるか」。
    0xFFFFFFFF ならゼロ (実体なし)。

使い方:
    uv run tools/mkflashimage.py --iplrom rom/iplrom.dat \\
        --hdd game.hdf --out build/x68kdata.bin
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"X68F"
VERSION = 1
HEADER_SIZE = 64
SECTOR_SIZE = 256
NO_SECTOR = 0xFFFFFFFF


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iplrom", type=Path, required=True)
    ap.add_argument("--cgrom", type=Path)
    ap.add_argument("--hdd", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument(
        "--limit",
        type=int,
        default=12 * 1024 * 1024,
        help="焼ける上限 (既定 12MB = storage パーティション)",
    )
    args = ap.parse_args()

    ipl = args.iplrom.read_bytes()
    cgrom = args.cgrom.read_bytes() if args.cgrom else b""
    disk = args.hdd.read_bytes()

    if len(disk) % SECTOR_SIZE != 0:
        print(f"エラー: ディスクがセクタ長で割り切れません ({len(disk)})", file=sys.stderr)
        return 1

    total_sectors = len(disk) // SECTOR_SIZE

    # 非ゼロのセクタだけ集める。
    index = [NO_SECTOR] * total_sectors
    data = bytearray()
    kept = 0
    for lba in range(total_sectors):
        chunk = disk[lba * SECTOR_SIZE : (lba + 1) * SECTOR_SIZE]
        if not any(chunk):
            continue
        index[lba] = kept
        data += chunk
        kept += 1

    # 配置を決める。
    pos = HEADER_SIZE
    ipl_off = pos
    pos += len(ipl)
    cgrom_off = pos if cgrom else 0
    pos += len(cgrom)
    # 索引はロングワード境界に揃える。68000 は奇数番地からロングを読めない。
    pos = (pos + 3) & ~3
    index_off = pos
    pos += total_sectors * 4
    data_off = pos
    pos += len(data)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into(">I", header, 4, VERSION)
    struct.pack_into(">I", header, 8, ipl_off)
    struct.pack_into(">I", header, 12, len(ipl))
    struct.pack_into(">I", header, 16, cgrom_off)
    struct.pack_into(">I", header, 20, len(cgrom))
    struct.pack_into(">I", header, 24, total_sectors)
    struct.pack_into(">I", header, 28, index_off)
    struct.pack_into(">I", header, 32, data_off)
    struct.pack_into(">I", header, 36, kept)

    out = bytearray(header)
    out += ipl
    out += cgrom
    out += b"\0" * (index_off - len(out))
    for v in index:
        out += struct.pack(">I", v)
    out += data

    if len(out) > args.limit:
        print(f"エラー: {len(out)} バイトで上限 {args.limit} を超えます", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(bytes(out))

    print(f"{args.out} を生成しました ({len(out)} バイト / {len(out) / 1024:.1f} KB)")
    print(f"  IPL-ROM: {len(ipl)} バイト")
    if cgrom:
        print(f"  CGROM:   {len(cgrom)} バイト")
    print(f"  ディスク: {total_sectors} セクタ中 {kept} セクタが実体を持つ")
    print(
        f"           ({kept * SECTOR_SIZE / 1024:.1f} KB。全部焼けば "
        f"{len(disk) / 1024 / 1024:.1f} MB)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
