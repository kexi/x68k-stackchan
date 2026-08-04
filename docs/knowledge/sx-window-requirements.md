---
title: SX-Window を動かすために足りないもの
description: Human68k 純正 GUI (SX-Window) の前提を、現在のエミュレータ実装と 1 項目ずつ突き合わせた結果。
type: gap-analysis
tags: [x68000, emulator, sx-window, gui, gap-analysis]
verified: partial
sources:
  - 実測（rom/iplrom.dat = EXPERT 用 IPLROM v1.0、MD5 7fd4caabac1d9169e289f0f7bbf71d8e）
  - 実測（host/gui_demo.cpp を --gui-demo で実行、512x512 16 色の合成結果）
  - 実測（Human68k 3.02 を SASI から起動し `A>` に到達、`dir` が動作。2026-08-04）
  - コード読解（src/x68k/core/ 現行実装）
  - docs/knowledge/x68000-sasi-boot.md
  - docs/knowledge/x68000-boot-sequence.md
updated: 2026-08-04
---

# SX-Window を動かすために足りないもの

## この文書の位置づけ

**Human68k 3.02 は SASI から起動し、`A>` のプロンプトまで到達した (実測)。
一方 SX-Window は依然として起動していない。SX-Window そのものが
この環境に存在しないため、起動を試すことすらできていない。**

この 2 つを混同しない。**Human68k が起動したことは SX-Window が
動くことを一切意味しない。** Human68k は OS の土台で、SX-Window は
その上に乗る GUI であり、別個の (そして著作物として別に入手が要る)
ソフトウェアである。

### 2026-08-04 の訂正: 「メディアが無い」は誤りだった

この文書は以前「ディスクイメージはこの作業環境のどこにも存在しない」と
書いていた。**これは誤りだった。** 探索が `.hdf` `.hds` `.xdf` `.dim`
`.d88` `.2hd` `.hdm` `.img` という**拡張子**に限られていたためで、
`/tmp/h302/` に Human68k 3.02 の配布ファイル一式 (80 ファイル、1.2MB)
が**展開済みのディレクトリとして**置かれていた。ディスクイメージでは
なく生のファイル群だったので、拡張子の網に掛からなかった。

教訓: **「イメージが無い」と「起動できる材料が無い」は別**。
`tools/make_sasi_image.py` は元々「LZH を展開した生ファイル群から
イメージを組む」ために書かれたツールで、材料はその形で存在していた。
次に同じ判断をするときは、拡張子ではなく `human.sys` / `command.x`
というファイル名で探す。

### 何が起動して、何が起動していないか

| 対象 | 状態 | 根拠 |
|---|---|---|
| Human68k 3.02 | **起動した**。`A>` まで到達し `dir` が動く | 下記「Human68k の起動」 |
| SX-Window | **未起動**。現物がこの環境に無い | `/tmp/h302/` は Human68k の基本セットのみ。`sx` 系のファイルは 0 件 |

`verified` を `partial` のままにしているのは、SX-Window に関する
G-1〜G-5 が依然として**予測**だからである。Human68k の起動は
`confirmed` に格上げしてよいが、この文書の主題は SX-Window なので
全体としては `partial` を維持する。

## 結論の要約

| 区分 | 状態 |
|---|---|
| 画面まわり (G-VRAM / パレット / プライオリティ合成) | **足りている見込み**。デモで通しで動いた |
| マウス (SCC + 割り込み) | **足りている見込み**。デモで割り込み経由まで動いた |
| メインメモリ 2MB | 足りている |
| **フロッピー (FDC)** | **決定的に足りない**。下記 G-1 |
| **SASI からの起動** | **実測で確認**。Human68k 3.02 が `A>` まで起動した |
| **Human68k の FAT 読み出し** | **実測で確認**。`dir` がルートディレクトリを列挙した |
| **SX-Window 本体** | **入手できていない**。これが現在の最大の欠落 |
| CPU 命令 | Human68k の起動に要る範囲は通った。SX-Window の範囲は未知。下記 G-4 |
| FM 音源 / ADPCM | 未実装。SX-Window の起動自体は妨げない見込み |

---

## Human68k の起動 (実測)

`tools/make_sasi_image.py` に `/tmp/h302/` を食わせて 20MB の SASI
イメージを作り、起動した。

```sh
just make-hdd /tmp/h302 /tmp/human.hdf
just run --hdd /tmp/human.hdf --cycles 400000000 --dump-text
```

結果 (テキスト画面の抜粋):

```
######## ### ###### version 3.02
Copyright 1987,88,89,90,91,92,93 SHARP/Hudson

Command version 3.00

A>
```

`--keys 'dir\n'` を足すと COMMAND.X が応答する:

```
A>dir
                        A:\
     3                 87K Byte            20168K Byte
HUMAN              SYS      58496
COMMAND            X        28382
CONFIG             SYS         20
```

**これは「プロンプトの絵が出た」のではない。** `dir` は COMMAND.X が
FAT を辿ってルートディレクトリを列挙した結果で、ファイル名も
サイズも配布物の実際の値と一致する (`human.sys` 58496 バイト、
`command.x` 28382 バイト)。つまり Human68k のファイルシステム層が
生きて動いている。

### 通ったディスク経路 (`--trace-disk`)

```
[disk] read lba=4 count=1      # IPL-ROM が識別子 "X68K" を検査
[disk] read lba=0 count=4      # ブートコード 1024B を $002000 へ
[disk] read lba=8 count=221    # HUMAN.SYS を $006800 へ (ブートコードが直接読む)
[disk] read lba=4 count=1      # Human68k がパーティションテーブルを読む
[disk] read lba=836 ...        # 以降 FAT / ルートディレクトリ / COMMAND.X
```

[x68000-sasi-boot.md](x68000-sasi-boot.md) が予測していた順序
(LBA 4 の識別が先、実行されるのは LBA 0) が、そのとおりに観測された。

### 検証に使った現物

| 対象 | MD5 | 備考 |
|---|---|---|
| `/tmp/h302/human.sys` (58496 B) | `023fc17478ecb6a00a41da4bb04de1a8` | Human68k 3.02 |
| `/tmp/h302/command.x` (28382 B) | `1c2bf1153d84eaeb941a1385257d8d81` | Command 3.00 |

**`/tmp/h302/` は永続しない。** `/tmp` は OS の再起動で消えるので、
この結果を再現するには配布物を再取得する必要がある。イメージ自体も
著作物を含むためリポジトリには置けない。

### まだ確かめていないこと

- `--keys` で送れるのは COMMAND.X の内部コマンドまでしか試していない。
  `bin/` 以下の外部コマンド (`chkdsk.x` など) の実行は未確認
- CONFIG.SYS は `make_sasi_image.py` が生成する最小構成
  (`SHELL = \COMMAND.X` の 1 行) を使っている。配布の CONFIG.SYS が
  読み込むデバイスドライバ群は**イメージに入っていない**。
  したがって「Human68k が完全な構成で起動する」ことは示せていない

---

---

## 実際に走らせたもの

イメージが無いので、SX-Window が使うのと同じハードウェア経路を
自前の 68000 プログラムで叩くデモを書いた
(`host/gui_demo.cpp`、`--gui-demo out.ppm` で起動)。

**これは SX-Window ではない。** 描いているのは自作の窓であって、
SX-Window のコードは一行も動いていない。示せるのは
「SX-Window が依存する経路が、通しで叩いても絵になる」ところまで。

走った内容と結果:

```
[gui-demo] 1880418 命令を実行しました
[gui-demo] マウス: SCC 有効=1 レポート受理=1
[gui-demo] ゲストが計算したカーソル位置: (296, 260)
```

- 16 色 512x512 モード設定 → パレット 7 色 → プライオリティ → `$C00000` 経由で描画
- 重なった 2 枚の窓 (タイトルバー・閉じるボタン・立体枠・影)
- テキスト面のメニューバーがグラフィック面の手前に合成される
- **マウスが割り込み経由で届く**: ゲストが SCC の WR1/WR2/WR3/WR5/WR9 を
  設定 → `STOP #$2000` で待つ → ホストが `Machine::moveMouse(96, 120)` →
  SCC がベクタ `$54` で受信割り込み → ハンドラが 3 バイトを読んで
  カーソル座標を更新。ゲストが計算した `(296, 260)` は
  ホストが与えた移動量と一致する

検査は `test/test_gui_demo.cpp` が同じ生成器を使って行う
(目視した絵とテストが見る絵をずらさないため、生成を共有している)。

### この過程で分かったこと

**IPL-ROM 既定のプライオリティ (`$FF642E` の `$06E4` = TX が奥) のままだと、
不透明な背景を塗るグラフィック面がテキスト面を丸ごと覆う。**
`GraphicRaster::composite` は
`graphicPriority() >= textPriority()` でどちらを後に描くか決めており
(`src/x68k/core/video/graphic_raster.cpp`)、グラフィックが手前だと
テキストは 1 ドットも見えない。SX-Window はメニューやテキストを
テキスト面に置くはずなので、`$E82500` を TX が手前になる値にする
必要がある。デモは `$0104` (TX=0 / GR=1) にした。

---

## 足りないもの (詳細)

### G-1. FDC が「ドライブ未接続」しか返さない — フロッピー起動・インストールは不可能

**これが最大の関門。** SX-Window の配布形態はフロッピー
(システムディスク + アプリケーションディスク) で、通常は
フロッピーから起動するかインストーラでハードディスクへ導入する。
どちらもフロッピーの読み出しが要る。

現行の `src/x68k/core/dev/fdc.cpp` (276 行) は:

- `kCmdReadData` (`$06`) / `kCmdReadTrack` / `kCmdReadId` /
  `kCmdWriteData` などの読み書き系コマンドを受け取ると、
  **実行フェーズにも結果フェーズにも入らず、即コマンド待ちへ戻る**
  (`executeCommand()` の該当 case にそう書いてある)
- `kCmdSenseDriveStatus` は ST3 のレディビット (`$20`) を**立てない** =
  ドライブ未接続の表明
- `kCmdRecalibrate` / `kCmdSeek` は割り込みを立て、
  `SENSE INTERRUPT STATUS` が `kSt0AbnormalTermination | kSt0EquipmentCheck`
  (異常終了 + 装置チェック) を返す
- ディスクイメージを受け取る口が無い。`Fdc` にイメージを渡す API は存在せず、
  `--hdd` に相当するフロッピー用のオプションも無い

さらに **FDC の割り込み線が MFP に配線されていない**。
`machine.cpp` が `fdc_` に触るのは `readStatus` / `readData` /
`writeData` / `writeDriveControl` の 4 箇所だけで、
MFP へ割り込みを上げる経路が無い。fdc.cpp のコメント自身が
「本エミュレータは FDC の割り込み線を配線していないため、結果を積むと
誰も読まず、次のコマンド送出が止まる」と書いている。

**必要な作業**: FDC にイメージバッキング (`.xdf` / `.dim` = 2HD 1232KB or
1440KB) を足し、READ DATA の実行フェーズ (DIO を立てて 1 セクタ分を
データポートから流す) と 7 バイトの結果フェーズを実装し、
FDC 割り込みを MFP に配線する。DMA 転送 (DMAC ch0 が FDC 用) も要る。

### G-2. SASI 経由なら起動しうるが、SX-Window 導入済みのイメージが要る

SASI からの Human68k 起動は実績がある
([x68000-sasi-boot.md](x68000-sasi-boot.md) が
プロトコルとディスク構造を実測で押さえている)。
したがって **SX-Window がインストール済みの `.hdf` があれば、
フロッピーを迂回して試せる可能性がある**。これが現状で最も現実的な経路。

ただし前提として:

- ~~Human68k が起動して `A>` に到達すること~~ →
  **2026-08-04 に達成した**。上記「Human68k の起動」を見よ。
  この前提はもう関門ではない
- SX-Window は Human68k 2.0 以降を要求する。起動したのは 3.02 なので
  この条件は満たす
- 導入済みイメージは 20MB 級になる。`Machine::kSasiMaxSectorsPerCommand`
  は 256 セクタ / コマンドで、転送バッファは 64KB。容量そのものの上限は
  `DiskImage` 実装側に依存する

### G-3. 画面モードは 16 色しか通しで確かめていない

SX-Window は **256 色 768x512** を標準的な画面モードとして使う。

`VideoController` は `k16Color` / `k256Color` / `k65536Color` を持ち、
`test_gvram_bus.cpp` が 256 色経路のバス折り込みを見ている。
だが **今回のデモは 16 色 512x512 でしか通しの動作を確かめていない**
(1 ドット = 1 ワードで命令列が素直になるため)。

256 色モードは 1 ドット 1 バイトで G-VRAM の折り込み方が 16 色と変わる。
768x512 は 512x512 を超えるので、CRTC のパラメータ設定
(`$E80000` 系の水平/垂直タイミング) も別経路になる。
**ここは未検証で、詰まる可能性がある。**

### G-4. 未実装命令に当たれば即停止する

`M68k::unimplemented()` はエミュレータを停止させる
(`Machine::isHalted()` / `haltedOpcode()`)。
SingleStepTests のベクタで 68000 の適合性は取っているが、
**SX-Window が実際に使う命令の網羅性は未知**。
デモの範囲 (MOVE / MOVEA / ORI / EXT / ADD / SUBQ / BNE / STOP) は動いた。

Line-A / Line-F は例外ベクタへ落ちる実装があるので
(`m68k.cpp` の `vector::kLineA` / `kLineF`)、そこは停止しない。

SX-Window は 68000 用に書かれているので、
68020 固有命令や MMU は要らないはず (X68000 Expert は 68000)。

### G-5. 未実装だが起動は妨げないと見ているもの

- **FM 音源 (OPM) / ADPCM**: `opm.cpp` / `adpcm.cpp` は存在するが
  README は「未実装」としている。SX-Window は起動時に音を鳴らさないので、
  レジスタ書き込みが無視されても止まらない見込み
- **スプライト / BG**: `sprite.cpp` と `sprite_raster.cpp` がある。
  SX-Window のマウスカーソルは**スプライトではなくソフトウェア描画**が
  基本なので、必須ではない見込み

---

## イメージを入手したときの試し方

```sh
# 1. まず Human68k が A> まで来るか (これは 3.02 で達成済み)
just run --hdd <image>.hdf --cycles 400000000 --dump-text

# 2. 来たら SX-Window を起動 (キー入力を送る)
just run --hdd <image>.hdf --cycles 2000000000 --keys "SX\n" --ppm /tmp/sx.ppm

# 3. 停止したら未実装命令の可能性。命令語が表示される
```

止まった位置の切り分けは
[x68000-emulator-pitfalls.md](x68000-emulator-pitfalls.md) の
症状別の表が使える。

## 参考: デモとの対応

| SX-Window が要るもの | デモで通したか | 実装の場所 |
|---|---|---|
| G-VRAM への描画 (`$C00000`) | 通した (16 色のみ) | `bus.cpp` / `graphic_raster.cpp` |
| グラフィックパレット (`$E82000`) | 通した | `video.h` |
| テキストパレット (`$E82200`) | 通した | `video.h` |
| プライオリティ合成 (`$E82500`) | 通した | `graphic_raster.cpp` |
| 表示制御 (`$E82600`) | 通した | `video.h` |
| SCC マウス (`$E98000`) | 通した (割り込み経由) | `scc.cpp` |
| メインメモリ 2MB | 使った | `memmap.h` |
| 256 色 768x512 | **未検証** | — |
| フロッピー読み出し | **不可** | `fdc.cpp` (G-1) |
| SASI からの起動 | **通した (デモではなく実物の Human68k 3.02)** | `sasi.cpp` / `make_sasi_image.py` |
| FAT の読み出し | **通した (`dir` が動いた)** | Human68k 側が実施 |
