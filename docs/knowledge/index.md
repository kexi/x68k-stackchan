---
title: x68k-stackchan ナレッジ索引
description: X68000 の仕様と、エミュレータ実装で得た知見の入口。
type: index
tags: [index, x68000, emulator]
verified: confirmed
sources:
  - docs/knowledge/x68000-emulator-pitfalls.md
  - docs/knowledge/x68000-sasi-boot.md
  - docs/knowledge/x68000-boot-sequence.md
  - 実測（IPL-ROM EXPERT 用 v1.0、MD5 7fd4caabac1d9169e289f0f7bbf71d8e）
  - 実測（Human68k 3.02 の HUMAN.SYS、58496 バイト）
updated: 2026-08-03
---

# ナレッジ索引

X68000 の仕様と、このプロジェクトで実際に手を動かして分かったことを置く。

形式は OKF（Markdown + YAML frontmatter）。`verified` フィールドで
「実測で確認した」か「資料を読んだだけ」かを区別する。

## 実装の知見（実測）

| ドキュメント | `verified` | 内容 |
|---|---|---|
| [X68000 エミュレータ実装の落とし穴](x68000-emulator-pitfalls.md) | `confirmed` | 実物の IPL-ROM を走らせて初めて露見したバグ。症状 → 原因 → 見分け方。ROM の読み違えで一度誤った結論に達した例も含む |
| [X68000 の SASI 起動経路](x68000-sasi-boot.md) | `confirmed` | SASI から Human68k が起動するまでのレジスタ・プロトコル・ディスク構造、IOCS とキーボードのワーク。実物の ROM と HUMAN.SYS から読み取った値 |

## X68000 の仕様（資料ベース + 一部実測）

| ドキュメント | `verified` | 内容 |
|---|---|---|
| [ブートシーケンス](x68000-boot-sequence.md) | `partial` | リセットから Human68k のプロンプトまで。Phase 1〜4 は実測、Phase 5〜6 は資料ベース |

## 検証に使った現物

ここに書いた「実測」はすべて次の 2 つを直接読んだ結果。
アドレスを裏取りするときはこの対応で引く。

| 対象 | ファイル | 対応 |
|---|---|---|
| IPL-ROM (EXPERT 用 v1.0) | `rom/iplrom.dat` (131072 B, MD5 `7fd4caabac1d9169e289f0f7bbf71d8e`) | メモリ `$FE0000` = オフセット 0 |
| Human68k 3.02 の HUMAN.SYS | `/tmp/h302/human.sys` (58496 B) | メモリ `$6800` = オフセット 64（先頭 64 B はヘッダ） |

```python
d = open('rom/iplrom.dat', 'rb').read()
def w(a): o = a - 0xFE0000; return (d[o] << 8) | d[o+1]
print(f'{w(0xFF42BC):04X}')   # -> 0C39  (CMPI.B)
```

## 一度書き間違えて訂正した項目

同じ落とし穴に二度落ちないよう、訂正の履歴を残す。詳細は各ドキュメントに。

| 項目 | 誤り | 正しくは |
|---|---|---|
| IOCS `$FF` | 「システム終了」→「無害なワーククリア」 | **エラー停止** (`$FF058E`)。`$FF059A` は IOCS `$FD` で別物 |
| `$000BC2` の比較 | `$FF42BA` にあり、外れると `$FF430C` で `RTS` | 比較は `$FF42BC`、飛び先は `$FF430E`（`RTS` ではない）。ただし関門でない点は変わらない |
| IOCS `$47` | B_READ（読み出し） | ドライブの準備。読み出しは **`$46`**。ROM は両方を順に呼ぶ |
| `$00081C` | キーバッファの残数 / 書き込み位置 | バッファの**先頭**。残数は `$000812`、書き込み位置は `$000814` |
| `$000BCA` | キーバッファの残数 | キー**文字列**の残数（`$000BCC` が読み出し位置） |
| `$001C62` | HUMAN.SYS に書き込む命令が無い | HUMAN.SYS の `$00A534` が書いている |
| 起動デバイス走査 | `ADDI.W #1` / `CMPI.W #4` | `ADDI.W #$0100` / `CMPI.W #$9470`、判定は `BTST #$1D` |

## 書き方の約束

- **確定情報と仮説を分ける**。`verified: confirmed` は実測で確かめたもの、
  `partial` は一部が資料頼み、`unverified` は未確認。
  本文でも推測は「推測」「未検証」と明記する。
- **症状から引けるようにする**。「何が起きたか」から原因へ辿れる構成にする。
  仕様を並べるだけの文書は、実際に詰まったときに役に立たない。
- **出典を書く**。実測なら「実測」と明記し、条件（ROM のバージョンや MD5）も残す。
- **訂正は消さずに残す**。誤った結論に至った理由を書いておくと、
  同じ読み違えを防げる。上の表がその索引。
