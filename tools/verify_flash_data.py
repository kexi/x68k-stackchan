#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""X68F退避データのROMと疎ディスクを元ファイルと全バイト比較する（読取専用）。"""

import argparse
import hashlib
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flash", type=Path)
    parser.add_argument("iplrom", type=Path)
    parser.add_argument("hdd", type=Path)
    args = parser.parse_args()
    flash = args.flash.read_bytes()
    ipl = args.iplrom.read_bytes()
    disk = args.hdd.read_bytes()
    assert len(flash) >= 64 and flash[:4] == b"X68F", "invalid header"
    version, rom_at, rom_size, _cg_at, cg_size, sectors, index_at, data_at, kept = (
        struct.unpack_from(">9I", flash, 4)
    )
    assert version == 1 and cg_size == 0, "unsupported version or unexpected CGROM"
    assert rom_at >= 64 and rom_size == len(ipl)
    assert flash[rom_at : rom_at + rom_size] == ipl, "IPL differs"
    assert sectors * 256 == len(disk), "disk size differs"
    assert index_at >= rom_at + rom_size and data_at >= index_at + sectors * 4
    assert data_at + kept * 256 <= len(flash), "truncated sector data"
    for sector in range(sectors):
        slot = struct.unpack_from(">I", flash, index_at + sector * 4)[0]
        assert slot == 0xFFFFFFFF or slot < kept, "invalid sector index"
        content = (
            bytes(256)
            if slot == 0xFFFFFFFF
            else flash[data_at + slot * 256 : data_at + (slot + 1) * 256]
        )
        assert content == disk[sector * 256 : (sector + 1) * 256], f"disk sector {sector} differs"
    print(f"verified IPL sha256={hashlib.sha256(ipl).hexdigest()}")
    print(f"verified HDD sha256={hashlib.sha256(disk).hexdigest()} sectors={sectors}")
    print(f"flash sha256={hashlib.sha256(flash).hexdigest()} bytes={len(flash)}")


if __name__ == "__main__":
    main()
