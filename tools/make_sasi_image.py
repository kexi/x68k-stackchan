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

# --- FAT12 のパラメータ -----------------------------------------------------
#
# HUMAN.SYS はブートコードが直接読み込むので FAT を経由しないが、
# 起動後の Human68k は CONFIG.SYS や COMMAND.X を FAT から探す。
# ファイルシステムが無いと、Human68k は何も読めずプロンプトを出せない。

# FAT の開始位置。Human68k の領域はここから始まる。
FAT_START_LBA = 32

FAT_BYTES_PER_SECTOR = SASI_SECTOR_SIZE
FAT_SECTORS_PER_CLUSTER = 4
FAT_RESERVED_SECTORS = 1
FAT_COPIES = 2
FAT_ROOT_ENTRIES = 128
FAT_MEDIA_DESCRIPTOR = 0xF8  # 固定ディスク

# ディレクトリエントリ 1 件の大きさ。
DIR_ENTRY_SIZE = 32


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

    # 読み出しは 2 段階。IPL-ROM 自身も $FF02AA でこの順に呼んでいる。
    #
    #   $47 — ドライブの準備。REZERO を送って位置決めするだけで、
    #         データは転送しない。ここだけ呼んで転送先を見ると 0 のまま。
    #   $46 — 実際の読み出し (B_READ)。
    #
    # $47 が「B_READ」という名前で紹介されている資料もあるが、
    # 実物の ROM ではこの 2 段構えになっている。
    #
    # レジスタは両方とも共通:
    #   D1.L = デバイス番号 ($8000 + SASI の ID)
    #   D2.L = 開始レコード番号
    #   D3.L = 読み込むバイト数 ($FF9676 で 256 で割ってセクタ数にされる)
    #   A1   = 読み込み先アドレス
    emit(b"\x22\x3c" + struct.pack(">I", 0x00008000), "MOVE.L #$8000,D1  ; SASI 0")
    emit(b"\x24\x3c" + struct.pack(">I", human_lba), "MOVE.L #human_lba,D2")
    emit(
        b"\x26\x3c" + struct.pack(">I", human_sectors * SASI_SECTOR_SIZE),
        "MOVE.L #bytes,D3",
    )
    emit(b"\x22\x7c" + struct.pack(">I", HUMAN_LOAD_ADDR), "MOVEA.L #$6800,A1")
    emit(b"\x70\x47", "MOVEQ #$47,D0     ; ドライブの準備")
    emit(b"\x4e\x4f", "TRAP #15")
    emit(b"\x70\x46", "MOVEQ #$46,D0     ; B_READ")
    emit(b"\x4e\x4f", "TRAP #15")

    # 読み込んだ HUMAN.SYS へ飛ぶ。イメージ生成時にヘッダを外して
    # メモリ上の姿にしてあるので、ヘッダぶんの補正は要らない。
    emit(b"\x4e\xf9" + struct.pack(">I", entry), "JMP entry")

    return bytes(code)


def build_id_sector(total_sectors: int) -> bytes:
    """IPL-ROM が最初に読む識別セクタを組む。

    総セクタ数はオフセット 8。IPL-ROM は $FF920A で 8(SP) を読み、
    $9FD9 / $13D1D と比べてディスクの諸元テーブル ($FF99B2 から $14 刻み)
    を選ぶ。位置を間違えると容量 0 とみなされ、小容量向けの諸元が
    使われる。
    """
    sector = bytearray(SASI_SECTOR_SIZE)
    sector[0:4] = BOOT_MAGIC
    struct.pack_into(">I", sector, 8, total_sectors)
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


class Fat12:
    """FAT12 のファイルシステムを組み立てる。

    Human68k の FAT は PC の FAT12 とほぼ同じ。違うのはセクタ長が 256 バイト
    であることと、ディレクトリエントリの日付形式くらいで、起動に要る範囲では
    同じ構造で通る。
    """

    def __init__(self, total_sectors: int):
        self.total_sectors = total_sectors
        # FAT のエントリ数はクラスタ数で決まる。12bit なので 1.5 バイト/個。
        self.cluster_count = total_sectors // FAT_SECTORS_PER_CLUSTER
        fat_bytes = (self.cluster_count + 2) * 3 // 2
        self.sectors_per_fat = (fat_bytes + FAT_BYTES_PER_SECTOR - 1) // FAT_BYTES_PER_SECTOR

        root_bytes = FAT_ROOT_ENTRIES * DIR_ENTRY_SIZE
        self.root_sectors = (root_bytes + FAT_BYTES_PER_SECTOR - 1) // FAT_BYTES_PER_SECTOR

        self.fat_start = FAT_RESERVED_SECTORS
        self.root_start = self.fat_start + FAT_COPIES * self.sectors_per_fat
        self.data_start = self.root_start + self.root_sectors

        self.fat = bytearray(self.sectors_per_fat * FAT_BYTES_PER_SECTOR)
        self.root = bytearray(root_bytes)
        self.data = bytearray()
        self.next_cluster = 2
        self.entry_count = 0

        # FAT の先頭 2 エントリは予約。メディア記述子と終端を入れる。
        self.set_fat_entry(0, 0xF00 | FAT_MEDIA_DESCRIPTOR)
        self.set_fat_entry(1, 0xFFF)

    def set_fat_entry(self, index: int, value: int) -> None:
        """12bit のエントリを書く。2 個で 3 バイトを分け合う。"""
        offset = index * 3 // 2
        if index % 2 == 0:
            self.fat[offset] = value & 0xFF
            self.fat[offset + 1] = (self.fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)
        else:
            self.fat[offset] = (self.fat[offset] & 0x0F) | ((value << 4) & 0xF0)
            self.fat[offset + 1] = (value >> 4) & 0xFF

    def add_file(self, name: str, data: bytes) -> None:
        """ルートディレクトリへファイルを 1 つ足す。"""
        if self.entry_count >= FAT_ROOT_ENTRIES:
            raise ValueError("ルートディレクトリが一杯です")

        cluster_bytes = FAT_SECTORS_PER_CLUSTER * FAT_BYTES_PER_SECTOR
        cluster_count = max(1, (len(data) + cluster_bytes - 1) // cluster_bytes)
        first_cluster = self.next_cluster

        # クラスタを連続で確保し、チェーンを繋ぐ。
        for i in range(cluster_count):
            current = first_cluster + i
            is_last = i == cluster_count - 1
            self.set_fat_entry(current, 0xFFF if is_last else current + 1)
        self.next_cluster += cluster_count

        # データ領域へ書く。クラスタ境界まで 0 で埋める。
        padded = data + bytes(cluster_count * cluster_bytes - len(data))
        self.data.extend(padded)

        # ディレクトリエントリ。名前は 8.3 の固定長で、空きは空白。
        stem, _, ext = name.partition(".")
        entry = bytearray(DIR_ENTRY_SIZE)
        entry[0:8] = stem.upper().ljust(8)[:8].encode("ascii")
        entry[8:11] = ext.upper().ljust(3)[:3].encode("ascii")
        entry[11] = 0x20  # アーカイブ属性
        struct.pack_into("<H", entry, 0x1A, first_cluster)
        struct.pack_into("<I", entry, 0x1C, len(data))

        offset = self.entry_count * DIR_ENTRY_SIZE
        self.root[offset : offset + DIR_ENTRY_SIZE] = entry
        self.entry_count += 1

    def build_boot_sector(self) -> bytes:
        """BPB を持つセクタを組む。Human68k はここを見て構造を知る。"""
        sector = bytearray(FAT_BYTES_PER_SECTOR)
        # 先頭 3 バイトはジャンプ命令の場所。Human68k は実行しないが、
        # 「ここから BPB」の目印として慣習どおり置いておく。
        sector[0:3] = b"\x60\x1e\x00"
        sector[3:11] = b"X68IPL30"
        struct.pack_into("<H", sector, 0x0B, FAT_BYTES_PER_SECTOR)
        sector[0x0D] = FAT_SECTORS_PER_CLUSTER
        struct.pack_into("<H", sector, 0x0E, FAT_RESERVED_SECTORS)
        sector[0x10] = FAT_COPIES
        struct.pack_into("<H", sector, 0x11, FAT_ROOT_ENTRIES)
        # 総セクタ数。65536 以上なら 0 にして $20 の 32bit 側へ入れる。
        if self.total_sectors < 0x10000:
            struct.pack_into("<H", sector, 0x13, self.total_sectors)
        else:
            struct.pack_into("<H", sector, 0x13, 0)
            struct.pack_into("<I", sector, 0x20, self.total_sectors)
        sector[0x15] = FAT_MEDIA_DESCRIPTOR
        struct.pack_into("<H", sector, 0x16, self.sectors_per_fat)
        return bytes(sector)

    def to_bytes(self) -> bytes:
        """パーティション全体のバイト列を返す。"""
        image = bytearray()
        image.extend(self.build_boot_sector())
        for _ in range(FAT_COPIES):
            image.extend(self.fat)
        image.extend(self.root)
        image.extend(bytes(self.root_sectors * FAT_BYTES_PER_SECTOR - len(self.root)))
        image.extend(self.data)
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
    placed = [(human_name, human_lba, len(human_data))]

    # FAT のファイルシステムを別に作る。
    #
    # HUMAN.SYS はブートコードが直接読むので FAT を経由しないが、
    # 起動後の Human68k は CONFIG.SYS や COMMAND.X を FAT から探す。
    # ここが無いと Human68k は何も読めず、プロンプトを出せない。
    #
    # HUMAN.SYS も FAT 側へ入れておく。ブートコードが読む生のコピーとは
    # 別に、ファイルとしても見えている必要がある。
    fat = Fat12(total_sectors - FAT_START_LBA)
    fat.add_file("HUMAN.SYS", human_raw)
    for name, data in files[1:]:
        fat.add_file(name, data)

    fat_image = fat.to_bytes()
    offset = FAT_START_LBA * SASI_SECTOR_SIZE
    if offset + len(fat_image) > hdd_bytes:
        raise ValueError("ファイルシステムが HDD の容量を超えています")
    image[offset : offset + len(fat_image)] = fat_image

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
        print(f"  {name}: LBA {lba} / {size} バイト (ブートコードが直接読む)")
    print(f"  ファイルシステム: LBA {FAT_START_LBA} / {len(fat_image)} バイト")
    print(f"    FAT {fat.sectors_per_fat} セクタ x {FAT_COPIES}")
    print(f"    ルートディレクトリ {fat.entry_count} エントリ")


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
