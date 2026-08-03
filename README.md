# x68k-stackchan

M5Stack CoreS3 (ESP32-S3) の上で動く **SHARP X68000 エミュレータ**（自作）。
Human68k を起動させることを目標にしています。

将来的にはスタックチャンの顔表示と相互に切り替えられるようにする予定です。

## いまどこまで動くのか

**ホスト（Mac）では Human68k が起動し、`A>` プロンプトで `dir` が動いています。** 実機（M5Stack CoreS3）ではまだ焼いていません。

### ホストでの動作確認

```
Human68k for X680x0 version 3.02
Copyright 1987,88,89,90,91,92,93 SHARP/Hudson

Command version 3.00

A>dir

                        A:\
     3                 87K Byte            20168K Byte
                       87K Byte
HUMAN              SYS      58496
COMMAND            X        28382
CONFIG             SYS         20
A>
```

プロンプトが出ているのは、実物の IPL-ROM が起動し、初期化を最後まで通ることが
確認できた証です。起動シーケンスは次のとおり:

- 実物の IPL-ROM が起動し、初期化を最後まで通る
- IPL-ROM が SASI のディスクを起動可能と認識し、ブートコードを `$002000` へ
  読み込んで実行する
- ブートコードが **HUMAN.SYS をロードして実行を開始する**
- Human68k が初期化を完了し、コマンドプロンプト (`A>`) を表示する
- `dir` などの基本的なコマンドが動く

### フォントについて

CGROM（漢字フォント）は無くても英数字は読めます。IPL-ROM 内蔵の 6×12 ANK フォントで代替する実装が入っているためです。漢字は出ません。

### 次の目標

実機（M5Stack CoreS3）への焼き込みと動作確認です。

検証の状況:

- 68000 コアは [SingleStepTests/m68000](https://github.com/SingleStepTests/m68000)
  の命令単位テストベクタで検証済み（`just test-vectors`）
- ホストの単体テストは 148 test cases / 764 assertions が通る（`just test-host`）
- 実機ファーム（ESP-IDF）のビルドが通る。バイナリは約 481KB

| 実装 | 状態 |
|---|---|
| MC68000 コア | 実装済み（SingleStepTests のテストベクタで検証） |
| バス / メモリマップ / SRAM / MFP / RTC | 実装済み |
| SASI（起動デバイス）+ DMAC | 実装済み。ここから Human68k がロードされる |
| FDC | 実装済み。Human68k の初期化が通る |
| テキスト画面（4 プレーン合成） | 実装済み |
| 実機バックエンド（SD / LCD / タッチ入力） | 実装済み |
| グラフィック画面・FM 音源・スプライト | 未実装 |
| スタックチャン統合（顔表示・サーボ） | 未実装 |

## なぜ自作するのか

px68k を移植すれば早いのは確かです。それでも自分で書いているのは、
X68000 が「どの順番で何を触ると起動するのか」を自分で確かめたいからです。
既存実装（px68k / XEiJ / MAME）は**挙動の答え合わせにのみ参照**しており、
コードは一行も持ち込んでいません。

その過程で分かったことは `docs/knowledge/` に OKF 形式で残しています。
IPL-ROM を逆アセンブルして確かめたアドレスや、一度誤った結論に達して
訂正した記録も含みます。

- [ナレッジ索引](docs/knowledge/index.md)
  - [X68000 エミュレータ実装の落とし穴](docs/knowledge/x68000-emulator-pitfalls.md) — 実物の ROM を走らせて初めて露見したバグ
  - [X68000 の SASI 起動経路](docs/knowledge/x68000-sasi-boot.md) — レジスタ・プロトコル・ディスク構造
  - [ブートシーケンス](docs/knowledge/x68000-boot-sequence.md) — リセットからプロンプトまでの全ステップ

## ハードウェア

- M5Stack CoreS3（ESP32-S3, 240MHz dual-core Xtensa LX7, 8MB PSRAM, 16MB Flash,
  320×240 タッチスクリーン, microSD）

## 開発の始め方

**ツールチェーンはすべて [Nix](https://nixos.org/) flake で固定**しています。
ESP-IDF・xtensa toolchain・clang・Python・ruff・just まで含むので、
`~/esp/esp-idf/export.sh` は要りません。依存は **nix と（任意で）direnv だけ**。

エミュレータ開発では「ESP-IDF のバージョンが違うと再現しない」類の問題が
起きやすく、そこに時間を使いたくないので全部固定しています。

```sh
nix develop          # devShell に入る
just --list          # 使えるタスクの一覧
```

direnv を使っている場合は `cd` するだけで自動的に有効になります（初回のみ
`direnv allow`）。

タスクは [just](https://github.com/casey/just) に集約しています。生の
`idf.py` や `cmake` を直接叩く必要はありません。

```sh
just test-host       # ホストの単体テスト
just test-san        # ASan/UBSan 付きで単体テスト
just run             # ホストで X68000 を起動する
just build           # 実機ファームのビルド
just flash           # CoreS3 へ書き込み
just check           # CI と同じ検査（fmt / lint / tidy / test）
```

## 構成

エミュレータ本体は **ESP32 非依存の純粋 C++17** として書き、ホスト（Mac）でも
同じコードが動くようにしています。実機はシリアルログしか見えず 1 サイクル
30 秒かかるのに対し、ホストなら 1 秒で回せてデバッガも使えます。この差が
開発速度を決めるので、分離は CI と pre-commit の両方で検査しています
（`just core-guard`）。

```
src/x68k/core/       ESP32 非依存。68000 コア、バス、各デバイス、ラスタライザ
src/x68k/platform/   ESP32 専用。M5Unified による LCD / タッチ / SD
host/                ホスト用フロントエンド (x68k-run)
main/                実機のエントリポイント (ESP-IDF)
test/                ホスト単体テスト（doctest）
tools/               開発補助スクリプト
docs/knowledge/      調査で分かったこと（OKF 形式）
```

## ROM とディスクイメージを用意する

**このリポジトリには X68000 の ROM も OS も含まれていません。**
シャープが 2000 年 4 月 3 日に無償公開したものですが、**無償配布に限る**という
条件が付いているため同梱していません。詳細と配布条件は
[`NOTICE.md`](NOTICE.md) を参照してください。

### 1. IPL-ROM を用意する

実機から吸い出すか、[X68000 LIBRARY](http://retropc.net/x68000/software/sharp/)
から取得して `rom/iplrom.dat`（128KB）に置きます。

開発時に使っているのは EXPERT 用 v1.0（MD5 `7fd4caabac1d9169e289f0f7bbf71d8e`）
です。`docs/knowledge/` に書いたアドレスはこの ROM のものなので、別バージョンだと
一致しないことがあります。

### 2. Human68k を取得して HDD イメージを作る

Human68k 3.02 を取得します。

```
http://retropc.net/x68000/software/sharp/human302/HUMAN302.LZH
```

LZH を展開すると `human.sys` / `command.x` などのファイルがそのまま出てきます。
フロッピーのイメージではないので、起動可能なディスクを自分で組み立てる必要が
あります。そのためのツールが入っています。

```sh
just make-hdd /path/to/human302 rom/hdd0.hdf
```

**なぜ FDC ではなく SASI から起動するのか**: FDC (uPD72065) はデータ転送が
DMAC 経由になるうえ、ドライブの状態機械も要ります。SASI なら `$C2` / READ 程度で
足ります。起動を通すのに必要な実装量が小さいので、まず SASI を選びました。

イメージの中身と、IPL-ROM がディスクに何を求めるかは
[`tools/make_sasi_image.py`](tools/make_sasi_image.py) の docstring に
書いてあります。

### CGROM について

CGROM（漢字フォント）は無償公開の対象外です。**CGROM が無くても、IPL-ROM 内の
6×12 ANK フォントで英数字のコンソールを表示できる**設計にしています。
漢字の表示には CGROM が必要です。

## ホストで動かす

```sh
just run --hdd rom/hdd0.hdf --ppm /tmp/boot.ppm
```

`--iplrom` は `rom/iplrom.dat` を既定で読みます。実機に焼かずに 68000 と
Human68k の起動をデバッグできるので、開発中はこちらが主戦場です。
未実装の命令に当たると命令語を表示して停止します。

起動を追うためのオプションが一通りあります。

| オプション | 用途 |
|---|---|
| `--ppm FILE` | テキスト画面を PPM に書き出す |
| `--trace` | 実行した命令をトレースする |
| `--trace-from ADDR` / `--trace-last N` | トレースの範囲を絞る |
| `--trace-disk` | ディスクアクセスを記録する |
| `--watch ADDR` | 特定アドレスへの書き込みの出所を追う |
| `--keys STR` | キー入力を打ち込む |
| `--stats` | 実行統計（ポーリングループの発見に使う） |
| `--cycles N` | 実行サイクル数の上限 |

## 実機で動かす

microSD に次のように配置します。

```
SD:/x68k/
├── iplrom.dat   IPL-ROM（128KB、必須）
├── cgrom.dat    CGROM（768KB、任意）
├── hdd0.hdf     SASI HDD イメージ（Human68k 入り）
└── sram.dat     自動生成
```

```sh
just build           # ビルド
just flash           # 書き込み
just monitor         # シリアル出力を読む
just run-device      # 書き込んでそのままシリアルを読む
```

`just size` でバイナリサイズの内訳が出ます。`.iram0` / `.dram0` の overflow は
リンク時にしか分からないので、68000 のホット命令を `IRAM_ATTR` に足したときは
これを見ています。

## 68000 コアの適合性テスト

[SingleStepTests/m68000](https://github.com/SingleStepTests/m68000)（MIT）は
MAME のマイクロコード実装から生成された命令単位の JSON テストベクタです。
自作 68000 コアの正しさを機械的に保証する要になっています。数 GB あるので
リポジトリには同梱していません。

```sh
just fetch-tests     # テストベクタを取得する
just test-vectors    # フル実行する
```

## ライセンス

MIT（[`LICENSE`](LICENSE)）。エミュレータのコードはすべて本プロジェクトで
書き起こしたものです。第三者成果物については
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md)、X68000 の ROM と OS の
扱いについては [`NOTICE.md`](NOTICE.md) を参照してください。
