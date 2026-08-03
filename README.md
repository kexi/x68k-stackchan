# x68k-stackchan

M5Stack CoreS3 (ESP32-S3) 上で動く **SHARP X68000 エミュレータ**（自作）。
まずは Human68k のコマンドプロンプト到達を目標にしています。

将来的にはスタックチャンの顔表示と相互に切り替えられるようにする予定です。

> **状態**: 開発初期（M0: 開発環境の構築）。まだ何も動きません。

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

CGROM（漢字フォント）は無償公開されていませんが、**CGROM が無くても
IPL-ROM 内の 6×12 ANK フォントで英数字のコンソールを表示できる**設計にしています。

## ライセンス

MIT（[`LICENSE`](LICENSE)）。エミュレータのコードはすべて本プロジェクトで
書き起こしたものです。第三者成果物については
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) を参照してください。
