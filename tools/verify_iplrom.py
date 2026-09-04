#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""IPL-ROM を根拠にした主張を、機械的に ROM と突き合わせる。

このリポジトリのコメントと OKF には「IPL-ROM を逆アセンブルして裏取りした」
と書かれた主張が多数ある。2026-08-26 に手作業で 5 件を再検証したところ、
1 件は「値は正しいが根拠の番地とビット番号が違う」形で誤っていた。
動作は何も壊れないので、テストでも実機でも永遠に露見しない種類の誤りである。

そこで、根拠に生ワード列 (ROM の実バイト列) が併記されている主張だけは
機械的に照合できるようにする。逆アセンブルの表記には書き手の解釈が入るが、
バイト列には入らない。

サブコマンド:
    check     生ワード列を抽出して ROM と比較する (ROM が要る)
    lint      生ワード列を伴わない番地の言及を数える (ROM 不要)
    selftest  抽出・比較ロジックを合成 ROM で確かめる (ROM 不要)

Why not 逆アセンブルして比較しないか:
    逆アセンブラを持ち込むと「ツールの解釈」という新しい信用点が増える。
    生ワード列との突き合わせは解釈を挟まないので、ツール自体が誤る余地が
    バイト比較 1 つに閉じる。

Why not 未知の ROM でも照合を続けないか:
    ROM のバージョンが違えばアドレスは正当にずれる。それを MISMATCH として
    報告すると「主張が誤っている」と読めてしまい、正しい主張を消しに
    かかることになる。MD5 が一致しない ROM は UNKNOWN_ROM で別扱いにする。
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from pathlib import Path

# メモリ $FE0000 が ROM ファイルのオフセット 0 に対応する。
ROM_BASE = 0xFE0000
ROM_SIZE = 0x20000
ROM_END = ROM_BASE + ROM_SIZE

# 検証済みの IPL-ROM (EXPERT 用 v1.0)。docs/knowledge/index.md と同じ値。
KNOWN_ROM_MD5 = "7fd4caabac1d9169e289f0f7bbf71d8e"

DEFAULT_ROM_PATH = "rom/iplrom.dat"

# 走査対象。src/x68k/core/ のヘッダと OKF ドキュメント。
DEFAULT_GLOBS = ("src/x68k/core/**/*.h", "docs/knowledge/*.md")

# ワード数の許容範囲。3 語未満は偶然一致しうるので根拠にならず、
# 16 語を超えるものは 1 つの主張ではなく写経になっているとみなす。
MIN_WORDS = 3
MAX_WORDS = 16

# 終了コード。MISMATCH と「ROM が無い」「ROM が違う」を区別する。
EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_NO_ROM = 3
EXIT_UNKNOWN_ROM = 4

MARKER = "生ワード列"

# 主張の開始形。マーカーの直後 (同じ行) に `$番地:` が続くものだけを
# 生ワード列の主張とみなす。
#
# Why not マーカーの出現すべてを主張として扱わないか: 「生ワード列を残していた」
# のような散文にも同じ語が出る。散文まで主張として読むと、書式違反が
# 常時 2 件出る状態になり、本物の書式違反がその中に埋もれる。逆にツール側を
# 寛容にして黙って読み飛ばすと、今度は本物の書式違反まで消える。
# 「マーカー + 番地」を主張の形と決めるのが、どちらにも倒れない線引き。
_CLAIM_START_RE = re.compile(MARKER + r"\s*\$")

# 生ワード列の本文。マーカーの直後から、閉じ括弧か行末までを 1 つの塊として
# 取り出す。改行と行頭の `//` をまたいで続いてよいので、抽出は行単位ではなく
# 「マーカー以降を連結した文字列」に対して行う。
_SEGMENT_RE = re.compile(
    r"\$(?P<addr>[0-9A-Fa-f]{1,8})\s*:\s*(?P<words>[0-9A-Fa-f\s]*[0-9A-Fa-f])",
)

# 番地の言及。$FE____ / $FF____ の 6 桁だけを対象にする。
_ADDRESS_RE = re.compile(r"\$(?:FE|FF)[0-9A-Fa-f]{4}")


@dataclass(frozen=True)
class Segment:
    """生ワード列 1 区間。`$FFC1F6: 70f8 0242 ...` の 1 つぶん。"""

    path: Path
    line: int
    address: int
    words: tuple[int, ...]
    # 書式が壊れている場合の理由。None なら書式は妥当。
    defect: str | None = None


@dataclass(frozen=True)
class Finding:
    """1 区間の照合結果。"""

    segment: Segment
    status: str
    detail: str = ""

    def render(self) -> str:
        where = f"{self.segment.path}:{self.segment.line}"
        head = f"{where}: {self.status} ${self.segment.address:06X}"
        return f"{head} {self.detail}".rstrip()


# ── 抽出 ────────────────────────────────────────────────────────────────────


def strip_comment_prefix(line: str) -> str:
    """行頭の `//` や Markdown の飾りを外し、続きの本文だけを返す。

    生ワード列は `//` をまたいで折り返してよい (video.h:275 に実例がある)。
    継続行の `//` を数字の一部と誤読しないよう、連結する前に落とす。
    """
    return re.sub(r"^\s*(?://+|\*|#+|\|)\s*", "", line)


def _iter_marker_blocks(text: str) -> Iterator[tuple[int, str]]:
    """マーカー出現ごとに (1 起点の行番号, マーカー以降の連結本文) を返す。

    連結は「閉じ括弧が出るまで」か「最大 4 行」で打ち切る。無制限に連結すると
    後続の無関係な 16 進数を拾って、書式違反を書式違反として報告できなくなる。
    """
    lines = text.splitlines()
    for index, line in enumerate(lines):
        start = _CLAIM_START_RE.search(line)
        if start is None:
            continue
        chunk = line[start.end() - 1 :]
        # 同じ行で閉じていなければ、後続行を `//` を外して連結する。
        consumed = 0
        while ")" not in chunk and "\n" not in chunk and consumed < 3:
            following = index + 1 + consumed
            if following >= len(lines):
                break
            chunk += " " + strip_comment_prefix(lines[following])
            consumed += 1
        # 閉じ括弧以降は本文ではない。行末までしか無い書式 (Markdown の
        # コードブロック) もあるので、括弧が無ければそのまま使う。
        close_at = chunk.find(")")
        if close_at >= 0:
            chunk = chunk[:close_at]
        yield index + 1, chunk


def parse_segments(text: str, path: Path) -> list[Segment]:
    """テキストから生ワード列を抽出する。書式違反も Segment として返す。

    書式違反を黙って捨てると「1 件も見つからなかった」と「全件通った」が
    区別できなくなる。defect を持つ Segment として必ず表に出す。
    """
    segments: list[Segment] = []
    for line_no, chunk in _iter_marker_blocks(text):
        matches = list(_SEGMENT_RE.finditer(chunk))
        if not matches:
            segments.append(Segment(path, line_no, 0, (), defect="生ワード列の書式で読めない"))
            continue
        for match in matches:
            segments.append(_build_segment(path, line_no, match))
    return segments


def _build_segment(path: Path, line_no: int, match: re.Match[str]) -> Segment:
    """1 区間の一致から Segment を作る。桁数・語数・範囲をここで検査する。"""
    raw_addr = match.group("addr")
    tokens = match.group("words").split()

    if len(raw_addr) != 6:
        return Segment(
            path,
            line_no,
            int(raw_addr, 16),
            (),
            defect=f"アドレスが 6 桁ではない (${raw_addr})",
        )

    address = int(raw_addr, 16)
    bad_token = next((t for t in tokens if len(t) != 4), None)
    if bad_token is not None:
        return Segment(
            path,
            line_no,
            address,
            (),
            defect=f"ワードが 4 桁ではない ({bad_token})",
        )

    is_too_short = len(tokens) < MIN_WORDS
    is_too_long = len(tokens) > MAX_WORDS
    if is_too_short or is_too_long:
        return Segment(
            path,
            line_no,
            address,
            (),
            defect=f"ワード数が {MIN_WORDS}-{MAX_WORDS} の範囲外 ({len(tokens)} 語)",
        )

    return Segment(path, line_no, address, tuple(int(t, 16) for t in tokens))


# ── 照合 ────────────────────────────────────────────────────────────────────


def read_words(rom: bytes, address: int, count: int) -> tuple[int, ...] | None:
    """ROM からビッグエンディアンで count ワード読む。範囲外なら None。"""
    offset = address - ROM_BASE
    is_out_of_range = offset < 0 or offset + count * 2 > len(rom)
    if is_out_of_range:
        return None
    return tuple((rom[offset + 2 * i] << 8) | rom[offset + 2 * i + 1] for i in range(count))


def _format_words(words: Iterable[int]) -> str:
    return " ".join(f"{w:04x}" for w in words)


def compare(segment: Segment, rom: bytes) -> Finding:
    """1 区間を ROM と比較する。"""
    if segment.defect is not None:
        return Finding(segment, "MALFORMED", segment.defect)

    is_outside_rom = not (ROM_BASE <= segment.address < ROM_END)
    if is_outside_rom:
        return Finding(
            segment,
            "OUT_OF_RANGE",
            f"(${ROM_BASE:06X}-${ROM_END - 1:06X} の外)",
        )

    actual = read_words(rom, segment.address, len(segment.words))
    if actual is None:
        return Finding(segment, "OUT_OF_RANGE", "(末尾を超えて読もうとした)")

    if actual == segment.words:
        return Finding(segment, "MATCH", f"({len(segment.words)} 語)")

    return Finding(
        segment,
        "MISMATCH",
        f"(expected {_format_words(segment.words)} actual {_format_words(actual)})",
    )


# ── ファイル収集 ────────────────────────────────────────────────────────────


def collect_files(root: Path, globs: Iterable[str]) -> list[Path]:
    """走査対象を集める。重複を除き、パス順で安定させる。"""
    found: set[Path] = set()
    for pattern in globs:
        found.update(p for p in root.glob(pattern) if p.is_file())
    return sorted(found)


def load_rom(path: Path) -> tuple[bytes | None, str]:
    """ROM を読む。存在しなければ (None, "") を返す。"""
    if not path.is_file():
        return None, ""
    data = path.read_bytes()
    # MD5 は ROM の同定にだけ使う (改竄検知ではないので強度は要らない)。
    return data, hashlib.md5(data).hexdigest()


# ── サブコマンド ────────────────────────────────────────────────────────────


def cmd_check(args: argparse.Namespace) -> int:
    """生ワード列を ROM と突き合わせる。"""
    root = Path(args.root)
    rom_path = root / args.rom

    rom, digest = load_rom(rom_path)
    if rom is None:
        print(f"SKIPPED: {args.rom} not found")
        print("  ROM はライセンス上リポジトリに含められない。手元に置くと照合できる。")
        return EXIT_NO_ROM

    if digest != KNOWN_ROM_MD5:
        print(f"UNKNOWN_ROM: {args.rom} md5={digest}")
        print(f"  expected {KNOWN_ROM_MD5} (EXPERT 用 v1.0)")
        print("  別バージョンではアドレスが正当にずれるので照合しない。")
        return EXIT_UNKNOWN_ROM

    findings = [
        compare(segment, rom)
        for path in collect_files(root, args.globs)
        for segment in parse_segments(path.read_text(encoding="utf-8"), path.relative_to(root))
    ]

    for finding in findings:
        print(finding.render())

    counts: dict[str, int] = {}
    for finding in findings:
        counts[finding.status] = counts.get(finding.status, 0) + 1

    total = len(findings)
    summary = ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    print(f"\n{total} 区間を照合した ({summary or 'なし'})。ROM md5={digest}")

    is_clean = counts.get("MISMATCH", 0) == 0 and counts.get("MALFORMED", 0) == 0
    is_clean = is_clean and counts.get("OUT_OF_RANGE", 0) == 0
    return EXIT_OK if is_clean else EXIT_MISMATCH


def _anchored_lines(text: str) -> set[int]:
    """生ワード列を含むコメントブロックの行番号を集める。

    「同一コメントブロック」は、空行と非コメント行で区切られた連続領域とする。
    ブロック内のどこかに生ワード列があれば、そのブロック全体を anchored とみなす。
    根拠の番地と生ワード列が別の行に書かれるのが普通の形だからである。
    """
    lines = text.splitlines()
    anchored: set[int] = set()

    block_start = 0
    block_has_marker = False
    for index, line in enumerate(lines):
        is_block_break = line.strip() == ""
        if is_block_break:
            if block_has_marker:
                anchored.update(range(block_start + 1, index + 1))
            block_start = index + 1
            block_has_marker = False
            continue
        if _CLAIM_START_RE.search(line):
            block_has_marker = True
    if block_has_marker:
        anchored.update(range(block_start + 1, len(lines) + 1))
    return anchored


@dataclass(frozen=True)
class Unanchored:
    path: Path
    line: int
    addresses: tuple[str, ...]


def find_unanchored(text: str, path: Path) -> list[Unanchored]:
    """生ワード列を伴わない番地の言及を列挙する。"""
    anchored = _anchored_lines(text)
    results: list[Unanchored] = []
    for index, line in enumerate(text.splitlines(), 1):
        if index in anchored:
            continue
        hits = _ADDRESS_RE.findall(line)
        if hits:
            results.append(Unanchored(path, index, tuple(hits)))
    return results


def cmd_lint(args: argparse.Namespace) -> int:
    """番地の言及のうち、機械照合できないものを数える。"""
    root = Path(args.root)
    files = collect_files(root, args.globs)

    unanchored: list[Unanchored] = []
    malformed: list[Segment] = []
    anchored_count = 0

    for path in files:
        text = path.read_text(encoding="utf-8")
        relative = path.relative_to(root)
        unanchored.extend(find_unanchored(text, relative))
        for segment in parse_segments(text, relative):
            if segment.defect is None:
                anchored_count += 1
            else:
                malformed.append(segment)

    if not args.quiet:
        for item in unanchored:
            print(f"{item.path}:{item.line}: unanchored {' '.join(item.addresses)}")

    for segment in malformed:
        print(f"{segment.path}:{segment.line}: MALFORMED {segment.defect}", file=sys.stderr)

    mentions = sum(len(item.addresses) for item in unanchored)
    print(f"\nunanchored: {len(unanchored)} 行 ({mentions} 件の言及)")
    print(f"anchored (生ワード列あり): {anchored_count} 区間")
    if malformed:
        print(f"malformed: {len(malformed)} 件")

    if args.baseline is not None:
        return _check_baseline(root / args.baseline, len(unanchored), bool(malformed))

    return EXIT_MISMATCH if malformed else EXIT_OK


def _check_baseline(baseline_path: Path, current: int, has_malformed: bool) -> int:
    """ラチェット。unanchored が baseline より増えていたら落とす。

    Why not 0 を目標に落とすか: 211 件の主張すべてに生ワード列を足すのは
    一度には終わらない。増やさないことだけを機械で守り、減らすのは手作業に
    任せる。減ったときは baseline を下げるよう促す。
    """
    if not baseline_path.is_file():
        print(f"BASELINE_MISSING: {baseline_path}", file=sys.stderr)
        return EXIT_MISMATCH

    baseline = int(baseline_path.read_text(encoding="utf-8").split("#")[0].strip())
    print(f"baseline: {baseline} 行")

    if current > baseline:
        print(
            f"FAIL: unanchored が {current - baseline} 行増えた "
            f"({baseline} -> {current})。番地を足したら生ワード列も足す。",
            file=sys.stderr,
        )
        return EXIT_MISMATCH

    if current < baseline:
        print(f"OK: unanchored が {baseline - current} 行減った。baseline を {current} へ下げる。")

    return EXIT_MISMATCH if has_malformed else EXIT_OK


# ── selftest ────────────────────────────────────────────────────────────────

# 合成 ROM の置き場所 (--write-fixture 用)。既定では書き出さない。
SYNTHETIC_ROM_PATH = "tools/testdata/synthetic_iplrom.bin"


def synthetic_rom() -> bytes:
    """selftest 用の合成 ROM。実物の IPL-ROM のバイト列は 1 バイトも含まない。

    中身は「オフセット i のバイト = (i * 7 + 13) & 0xFF」という自作の式。
    実機の命令列ではないが、抽出と比較のロジックを試すのが目的なので、
    意味のある機械語である必要が無い。実 ROM と同じ 128KB にしてあるので、
    末尾をまたぐ OUT_OF_RANGE の境界も本物と同じ位置で試せる。

    Why not 生成した 128KB のバイナリをリポジトリに置くか:
        置くと「実物の ROM が紛れ込んでいないか」をレビューで確かめられない。
        バイナリの差分は読めないので、次に誰かが差し替えても気付けない。
        生成式をソースに置けば、合成であることがコードレビューで確認できる。
        実際、この式で作った 128KB は実 ROM と 8 バイト以上一致する箇所が
        1 つも無い (確認済み)。ファイルが要るときは --write-fixture で出す。
    """
    return bytes((i * 7 + 13) & 0xFF for i in range(ROM_SIZE))


def _synthetic_words(rom: bytes, address: int, count: int) -> str:
    words = read_words(rom, address, count)
    assert words is not None
    return _format_words(words)


def cmd_selftest(args: argparse.Namespace) -> int:
    """抽出と比較のロジックを、合成 ROM と既知のサンプルで確かめる。

    What: 以下を保証する。
      1. 1 行の生ワード列を抽出し、正しい ROM 内容に MATCH を返す
      2. `/` 区切りの複数区間を、区間ごとに独立して抽出する
      3. 改行と `//` をまたいだ折り返しを 1 つの区間として読む
      4. 1 ワードだけ違う列に MISMATCH を返し、期待値と実値の両方を出す
      5. ROM 範囲外の番地に OUT_OF_RANGE を返す
      6. 語数・桁数の書式違反を MALFORMED として報告する (黙って捨てない)
      7. 生ワード列を伴わない番地を unanchored として数え、
         伴うブロックは数えない
    """
    rom = synthetic_rom()
    failures: list[str] = []

    def expect(name: str, actual: object, wanted: object) -> None:
        if actual != wanted:
            failures.append(f"{name}: expected {wanted!r}, got {actual!r}")

    fake = Path("fake.h")

    # 1. 単一区間が MATCH する。
    single = _synthetic_words(rom, 0xFF1000, 5)
    text = f"// (生ワード列 $FF1000: {single})\n"
    segments = parse_segments(text, fake)
    expect("single/count", len(segments), 1)
    expect("single/address", segments[0].address, 0xFF1000)
    expect("single/words", len(segments[0].words), 5)
    expect("single/status", compare(segments[0], rom).status, "MATCH")

    # 2. `/` 区切りの複数区間が独立して取れる (sprite.h:258 と同じ形)。
    first = _synthetic_words(rom, 0xFFC1F6, 4)
    second = _synthetic_words(rom, 0xFFC208, 3)
    text = f"// (生ワード列 $FFC1F6: {first} / $FFC208: {second})\n"
    segments = parse_segments(text, fake)
    expect("multi/count", len(segments), 2)
    expect("multi/addresses", [s.address for s in segments], [0xFFC1F6, 0xFFC208])
    expect("multi/statuses", [compare(s, rom).status for s in segments], ["MATCH", "MATCH"])

    # 3. 改行と `//` をまたいだ折り返し (video.h:275 と同じ形)。
    wrapped = _synthetic_words(rom, 0xFFB2D2, 12).split()
    text = (
        f"    // という手順を踏む (生ワード列 $FFB2D2: {' '.join(wrapped[:6])}\n"
        f"    // {' '.join(wrapped[6:])})。マスクが\n"
    )
    segments = parse_segments(text, fake)
    expect("wrapped/count", len(segments), 1)
    expect("wrapped/words", len(segments[0].words), 12)
    expect("wrapped/status", compare(segments[0], rom).status, "MATCH")

    # 4. 1 ワードだけ違えば MISMATCH。期待値と実値の両方が出る。
    broken = single.split()
    broken[2] = "dead"
    text = f"// (生ワード列 $FF1000: {' '.join(broken)})\n"
    finding = compare(parse_segments(text, fake)[0], rom)
    expect("mismatch/status", finding.status, "MISMATCH")
    expect("mismatch/has_expected", "expected" in finding.detail, True)
    expect("mismatch/has_actual", "actual" in finding.detail, True)

    # 5. ROM 範囲外。
    text = "// (生ワード列 $FDFFFE: 1234 5678 9abc)\n"
    expect(
        "out_of_range/status",
        compare(parse_segments(text, fake)[0], rom).status,
        "OUT_OF_RANGE",
    )
    # 末尾ぎりぎりから 3 語読もうとする形も範囲外になる。
    text = f"// (生ワード列 ${ROM_END - 2:06X}: 1234 5678 9abc)\n"
    expect(
        "out_of_range/tail",
        compare(parse_segments(text, fake)[0], rom).status,
        "OUT_OF_RANGE",
    )

    # 6. 書式違反は MALFORMED として表に出る (寛容に読み替えない)。
    cases = {
        "// (生ワード列 $FF1000: 1234 5678)\n": "ワード数",
        "// (生ワード列 $FF1000: " + " ".join(["1234"] * 17) + ")\n": "ワード数",
        "// (生ワード列 $FF100: 1234 5678 9abc)\n": "アドレスが 6 桁ではない",
        "// (生ワード列 $FF1000: 123 5678 9abc)\n": "ワードが 4 桁ではない",
        "// (生ワード列 $: 数字が無い)\n": "書式で読めない",
    }
    for source, wanted in cases.items():
        segment = parse_segments(source, fake)[0]
        finding = compare(segment, rom)
        expect(f"malformed/{wanted}/status", finding.status, "MALFORMED")
        expect(f"malformed/{wanted}/reason", wanted in (segment.defect or ""), True)

    # 6b. マーカーが散文に出るだけの行は主張ではない (書式違反でもない)。
    #     ここを主張として扱うと、散文を書くたびに MALFORMED が増える。
    prose = "`dev/sprite.h:25` は主張と一緒に生ワード列を残していた:\n"
    expect("prose/not_a_claim", parse_segments(prose, fake), [])

    # 7. unanchored の数え方。生ワード列を含むブロックは数えない。
    text = (
        "// 根拠: $FF6436 が値を書く\n"
        f"// (生ワード列 $FF1000: {single})\n"
        "\n"
        "// 別ブロック: $FF8042 は根拠が無い\n"
        "// $FFC1F6 も同じブロックなので同じ扱い\n"
    )
    unanchored = find_unanchored(text, fake)
    expect("unanchored/lines", [u.line for u in unanchored], [4, 5])
    expect("unanchored/mentions", sum(len(u.addresses) for u in unanchored), 2)

    # 8. 合成 ROM が実 ROM ではないことを、MD5 ゲートで確かめる。
    digest = hashlib.md5(rom).hexdigest()
    expect("synthetic/is_not_real_rom", digest != KNOWN_ROM_MD5, True)
    expect("synthetic/size", len(rom), ROM_SIZE)

    if args.write_fixture:
        fixture = Path(args.root) / SYNTHETIC_ROM_PATH
        fixture.parent.mkdir(parents=True, exist_ok=True)
        fixture.write_bytes(rom)
        print(f"wrote {SYNTHETIC_ROM_PATH} ({len(rom)} bytes, md5 {digest})")

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)

    if failures:
        print(f"\nselftest: {len(failures)} 件失敗")
        return EXIT_MISMATCH

    print("selftest: 8 群すべて通った (抽出 / 複数区間 / 折り返し / 不一致 / 範囲外 /")
    print("          書式違反 / unanchored の数え方 / 合成 ROM が実物でないこと)")
    return EXIT_OK


# ── CLI ─────────────────────────────────────────────────────────────────────


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="verify_iplrom.py",
        description="IPL-ROM を根拠にした主張を機械的に検証する",
    )
    parser.add_argument(
        "--root",
        default=".",
        help="リポジトリのルート (既定: カレントディレクトリ)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    check = subparsers.add_parser("check", help="生ワード列を ROM と比較する")
    check.add_argument("--rom", default=DEFAULT_ROM_PATH, help="IPL-ROM のパス")
    check.add_argument("--globs", nargs="*", default=list(DEFAULT_GLOBS))
    check.set_defaults(func=cmd_check)

    lint = subparsers.add_parser("lint", help="生ワード列を伴わない番地を数える")
    lint.add_argument("--globs", nargs="*", default=list(DEFAULT_GLOBS))
    lint.add_argument("--baseline", default=None, help="ラチェット用の baseline ファイル")
    lint.add_argument("--quiet", action="store_true", help="件数だけ出す")
    lint.set_defaults(func=cmd_lint)

    selftest = subparsers.add_parser("selftest", help="抽出・比較ロジックを確かめる")
    selftest.add_argument(
        "--write-fixture",
        action="store_true",
        help="合成 ROM をファイルへ書き出す",
    )
    selftest.set_defaults(func=cmd_selftest)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
