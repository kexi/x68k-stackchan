# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
"""Collect a device LCD frame; opening USB may reset the board."""

import atexit
import json
import re
import sys
import time
from pathlib import Path

import serial

from capture_scenario import emit_line, observe

log_path = sys.argv[7] if len(sys.argv) > 7 else ""
# Why not with 文か: このログは実行中ずっと開いたままにし、終了時に atexit で
# 閉じる。with にするとブロックを抜けた時点で閉じてしまい、寿命が合わない。
log_file = open(log_path, "x", encoding="utf-8") if log_path else None  # noqa: SIM115
has_log_file = log_file is not None
if has_log_file:
    atexit.register(log_file.close)


def emit(text, **kwargs):
    emit_line(text, output=sys.stdout, log=log_file, **kwargs)


port = serial.Serial()
port.port = sys.argv[1]
port.baudrate = 115200
port.timeout = 1
port.dtr = False
port.rts = False
port.open()
pending = b""
if len(sys.argv) > 3:
    # USB接続時に再起動する環境ではHuman68kのキーボード初期化を待つ。
    boot_until = time.monotonic() + 20
    while time.monotonic() < boot_until:
        line = port.readline().decode("utf-8", errors="replace")
        if line.strip():
            emit(line.strip(), flush=True)
    port.write(sys.argv[3].encode().replace(b"\\r", b"\r"))
    started_at = time.monotonic()
    start_at = started_at + float(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] else None
    events = json.loads(Path(sys.argv[6]).read_text()) if len(sys.argv) > 6 and sys.argv[6] else []
    pending = observe(port, started_at, float(sys.argv[4]), start_at, events, emit=emit)
port.write(b"?")
rows = {}
width = height = 0
deadline = time.monotonic() + 90
while time.monotonic() < deadline:
    line = (pending + port.readline()).decode("ascii", errors="ignore").strip()
    pending = b""
    if "[remote-key]" in line and "accepted=0" in line:
        raise RuntimeError("Remote key was rejected")
    begin = re.search(r"X68K_FRAME_BEGIN (\d+) (\d+)", line)
    if begin:
        width, height = map(int, begin.groups())
        rows.clear()
    row = re.search(r"X68K_FRAME_ROW (\d+) ([0-9a-f]+)", line)
    if row:
        rows[int(row[1])] = row[2]
    if "X68K_FRAME_END" in line:
        break
port.close()
assert width and height and set(rows) == set(range(height)), "Incomplete capture"
pixels = bytearray()
for y in range(height):
    assert len(rows[y]) == width * 4, "Truncated row"
    for x in range(width):
        value = int(rows[y][x * 4 : x * 4 + 4], 16)
        pixels.extend(
            ((value >> 11) * 255 // 31, ((value >> 5) & 63) * 255 // 63, (value & 31) * 255 // 31)
        )
Path(sys.argv[2]).write_bytes(f"P6\n{width} {height}\n255\n".encode() + pixels)
emit(f"Captured device LCD: {width}x{height} -> {sys.argv[2]}")
