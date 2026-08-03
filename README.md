# x68k-stackchan

M5Stack CoreS3 (ESP32-S3) 上で動く **SHARP X68000 エミュレータ**（自作）。
まずは Human68k のコマンドプロンプト到達を目標にしています。

将来的にはスタックチャンの顔表示と相互に切り替えられるようにする予定です。

> **状態**: 68000 コアと周辺デバイス、実機バックエンドまで実装済み。
> 実際の起動確認には利用者側で IPL-ROM と Human68k を用意する必要があります（後述）。

| 実装 | 状態 |
|---|---|
| MC68000 コア | 実装済み（[SingleStepTests](https://github.com/SingleStepTests/m68000) の全 127 命令スイートで検証） |
| バス / SRAM / MFP / CRTC / ビデオコントローラ | 実装済み |
| SASI（起動デバイス） | 実装済み |
| テキスト画面（4 プレーン合成） | 実装済み |
| 実機バックエンド（SD / LCD / タッチ入力） | 実装済み |
| グラフィック画面・FM 音源・スプライト | 未実装 |
| スタックチャン統合（顔表示・サーボ） | 未実装 |

## ハードウェア

- M5Stack CoreS3（ESP32-S3, 240MHz dual-core Xtensa LX7, 8MB PSRAM, 16MB Flash,
  320×240 タッチスクリーン, microSD）

## 開発環境

**ツールチェーンはすべて [Nix](https://nixos.org/) flake で固定**しています。
ESP-IDF と xtensa toolchain も含むので、`~/esp/esp-idf/export.sh` は不要です。
依存は **nix と（任意で）direnv だけ**。

```sh
nix develop          # devShell に入る
# direnv 利用時は cd するだけで自動有効化（初回のみ direnv allow）
```

タスクは [just](https://github.com/casey/just) に集約しています。生の
`idf.py` や `cmake` を直接叩く必要はありません。

```sh
just                 # タスク一覧
just test-host       # core/ のホストテスト（Mac 上で動く）
just build           # 実機ファームのビルド
just flash           # CoreS3 へ書き込み
just check           # CI と同じ検査（fmt / lint / tidy / test）
```

## 構成

エミュレータ本体は **ESP32 非依存の純粋 C++17** として書き、ホスト（Mac）上でも
同じコードが動くようにしています。実機に焼かずに 68000 と Human68k をデバッグ
できることが開発速度を決めるためで、この分離は CI でも検査しています。

```
src/x68k/core/       ESP32 非依存。68000 コア、バス、各デバイス、ラスタライザ
src/x68k/platform/   ESP32 専用。M5Unified による LCD / タッチ / SD
test/                ホスト単体テスト（doctest）
tools/               開発補助スクリプト
```

## ROM とディスクイメージ

**このリポジトリには X68000 の ROM も OS も含まれていません。** 動かすには
利用者自身が IPL-ROM と Human68k を用意する必要があります。配布条件を含む
詳細は [`NOTICE.md`](NOTICE.md) を参照してください。

シャープが 2000 年に無償公開したもので、
[X68000 LIBRARY](http://retropc.net/x68000/software/sharp/) から取得できます。

### 実機で動かす場合（microSD に配置）

```
SD:/x68k/
├── iplrom.dat   IPL-ROM（128KB、必須）
├── cgrom.dat    CGROM（768KB、任意）
├── hdd0.hdf     SASI HDD イメージ（Human68k 入り）
└── sram.dat     自動生成
```

### ホストで動かす場合

```sh
just run --hdd rom/hdd0.hdf --ppm /tmp/boot.ppm
```

`--iplrom` は `rom/iplrom.dat` を既定で読みます。実機に焼かずに 68000 と
Human68k の起動をデバッグできるので、開発中はこちらが主戦場です。
未実装の命令に当たると命令語を表示して停止します。

CGROM（漢字フォント）は無償公開されていませんが、**CGROM が無くても
IPL-ROM 内の 6×12 ANK フォントで英数字のコンソールを表示できる**設計にしています。

## ライセンス

MIT（[`LICENSE`](LICENSE)）。エミュレータのコードはすべて本プロジェクトで
書き起こしたものです。第三者成果物については
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) を参照してください。
