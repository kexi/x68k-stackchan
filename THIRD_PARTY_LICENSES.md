# Third-party licenses

本プロジェクトが同梱、または取得して利用する第三者成果物の一覧です。
X68000 の ROM / OS の扱いについては `NOTICE.md` を参照してください。

## 同梱しているもの

| 成果物 | バージョン | ライセンス | 配置 | 用途 |
|---|---|---|---|---|
| [doctest](https://github.com/doctest/doctest) | 2.4.11 | MIT | `test/doctest/doctest.h` | ホスト単体テストのフレームワーク |

Copyright (c) 2016-2023 Viktor Kirilov

MIT ライセンスの全文は `test/doctest/LICENSE.txt` に置いています。
ヘッダ `doctest.h` の冒頭には著作権表示と参照先の案内があるだけで
条文そのものは含まれないため、上流の `LICENSE.txt` を併置しています。
MIT は「著作権表示および許諾表示を複製物の全てに含める」ことを求めるので、
ヘッダだけでは条件を満たしません。

## ビルド時に取得するもの（同梱しない）

| 成果物 | バージョン | ライセンス | 取得経路 |
|---|---|---|---|
| [M5Unified](https://github.com/m5stack/M5Unified) | 0.2.19 | MIT (c) 2021 M5Stack | ESP-IDF Component Manager（`main/idf_component.yml`） |
| [M5GFX](https://github.com/m5stack/M5GFX) | 0.2.26 | MIT (c) 2021 M5Stack | M5Unified の依存として |
| [ESP-IDF](https://github.com/espressif/esp-idf) | 5.5.2 | Apache-2.0 | Nix（`flake.nix` の `nixpkgs-esp-dev`） |

バージョンは `dependencies.lock` と `flake.lock` で固定しています。
リポジトリには含まれず、ビルド時に取得されます。

### M5GFX に含まれる第三者成果物

**M5GFX の `LICENSE` は M5Stack の著作権表示しか持ちませんが、内部には
別ライセンスの成果物が同梱されています。** バイナリを配布するときは、
実際にリンクされるものの表示義務を果たす必要があります。

本プロジェクトのファームに**実際にリンクされるもの**:

| 成果物 | ライセンス | 用途 |
|---|---|---|
| Adafruit GFX の 5x7 フォント (`glcdfont.h`) | BSD 3-Clause (c) 2012 Adafruit Industries | 起動時のメッセージ表示 (`DisplayLcd::showMessage`) |

BSD は**バイナリ配布時に著作権表示・条件・免責事項を配布物に添付する資料へ
含めること**を求めるため、条文を `licenses/adafruit-gfx-BSD.txt` に置いています。

リンクされないもの（参考）: M5GFX には東雲フォント (/efont/)、IPA フォント、
ChaN の FatFs 由来のコードなども含まれますが、本プロジェクトは使っておらず、
未使用シンボルはリンカが落とすためファームには入りません
（`xtensa-esp32s3-elf-nm` で確認済み）。使う場合は表示義務が増えます。

## 利用者が取得するもの（同梱しない）

| 成果物 | ライセンス | 取得経路 |
|---|---|---|
| [東雲フォント](http://openlab.ring.gr.jp/efont/) | Public Domain（実質） | 利用者が取得し `just make-cgrom` で変換 |

CGROM（漢字フォント ROM）は無償公開されていないため、代替として東雲フォントから
CGROM 相当のイメージを生成できます。東雲フォントのライセンスは「自由な改造、
他フォーマットへの変換、組込み、再配布を行うことができます」と明記された実質的な
パブリックドメインで、本用途に制約がありません。詳細は `NOTICE.md` を参照してください。

フォント本体も生成したイメージもリポジトリには含めません。

生成した CGROM を配布する場合、東雲フォントは実質パブリックドメインなので
ライセンス上の表示義務はありませんが、**出所を明記するのが礼儀**です。
作者は AUTHORS に列挙されており、権利を行使しないと宣言する形を採っています
（日本の法制では著作権の放棄が不可能なため）。

## テスト時に取得するもの（同梱しない）

| 成果物 | ライセンス | 取得経路 |
|---|---|---|
| [SingleStepTests/m68000](https://github.com/SingleStepTests/m68000) | MIT | `just fetch-tests` |

MC68000 の命令単位テストベクタ（JSON）です。MAME の 68000 マイクロコード実装から
生成されたもので、本プロジェクトの自作 68000 コアの検証に使います。
数 GB あるためリポジトリには含めず、必要なときに取得します。

## バイナリを配布する場合

**`licenses/` ディレクトリをそのまま配布物に添付してください。** ファームに
リンクされる成果物の条文を集めてあります。

| ファイル | 対象 | 義務の根拠 |
|---|---|---|
| `licenses/m5unified-MIT.txt` | M5Unified | MIT: 著作権表示と許諾表示を複製物に含める |
| `licenses/m5gfx-MIT.txt` | M5GFX | 同上 |
| `licenses/adafruit-gfx-BSD.txt` | Adafruit GFX の 5x7 フォント | BSD: バイナリ配布時に添付資料へ含める |
| `licenses/esp-idf-Apache-2.0.txt` | ESP-IDF | Apache-2.0 第 4 条: ライセンスの写しを頒布先へ渡す |

`licenses/doctest-MIT.txt` はテスト専用でファームには入らないため、
ファームウェア単体の配布では不要です。ソースを配布する場合は必要です。

Apache-2.0 の ESP-IDF は、**改変した場合**は変更点を明示する必要もあります
（第 4 条 b）。本プロジェクトは ESP-IDF を改変していません。

加えて、**X68000 の ROM や Human68k を含む配布物**については `NOTICE.md` に
記載した配布条件（無償配布に限る、改変時はソース公開が必須）に従う必要があります。
そもそも ROM の再配布は避け、利用者が自分で用意する方式を推奨します。

### この一覧の作り方

`licenses/` は手作業で集めたものなので、依存を増やしたときは更新が要ります。
ファームに実際に何がリンクされているかは次で確認できます。

```
xtensa-esp32s3-elf-nm build/x68k-stackchan.elf | grep -i font
```

M5GFX のように「LICENSE ファイルに書かれていない第三者成果物」を内包する
ライブラリがあるため、**トップレベルの LICENSE を見るだけでは足りません**。
`grep -rh "Copyright" managed_components/ | sort -u` で洗い出せます。
