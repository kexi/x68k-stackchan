# Third-party licenses

本プロジェクトが同梱、または取得して利用する第三者成果物の一覧です。
X68000 の ROM / OS の扱いについては `NOTICE.md` を参照してください。

## 同梱しているもの

| 成果物 | ライセンス | 配置 | 用途 |
|---|---|---|---|
| [doctest](https://github.com/doctest/doctest) | MIT | `test/doctest/doctest.h` | ホスト単体テストのフレームワーク |

doctest は MIT ライセンスです。著作権表示とライセンス条文はヘッダ
`test/doctest/doctest.h` の冒頭に含まれています。

## ビルド時に取得するもの（同梱しない）

| 成果物 | ライセンス | 取得経路 |
|---|---|---|
| [M5Unified](https://github.com/m5stack/M5Unified) | MIT | ESP-IDF Component Manager（`main/idf_component.yml`） |
| [M5GFX](https://github.com/m5stack/M5GFX) | MIT | M5Unified の依存として |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Apache-2.0 | Nix（`flake.nix` の `nixpkgs-esp-dev`） |

これらはリポジトリに含まれず、ビルド時に取得されます。各ライブラリに同梱の
`LICENSE` が正となります。

## テスト時に取得するもの（同梱しない）

| 成果物 | ライセンス | 取得経路 |
|---|---|---|
| [SingleStepTests/m68000](https://github.com/SingleStepTests/m68000) | MIT | `just fetch-tests` |

MC68000 の命令単位テストベクタ（JSON）です。MAME の 68000 マイクロコード実装から
生成されたもので、本プロジェクトの自作 68000 コアの検証に使います。
数 GB あるためリポジトリには含めず、必要なときに取得します。

## バイナリを配布する場合

ビルドしたファームウェアを配布する場合は、上記 MIT / Apache-2.0 成果物の
著作権表示とライセンス条文を配布物に添付してください。

加えて、**X68000 の ROM や Human68k を含む配布物**については `NOTICE.md` に
記載した配布条件（無償配布に限る、改変時はソース公開が必須）に従う必要があります。
