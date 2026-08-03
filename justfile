# x68k-stackchan task runner (just)。
#
# 【方針】実行するコマンドはすべてこの justfile に集約する。
# 生の idf.py / cmake / ctest / clang-format を直接叩かない。CI もローカルも
# `just <task>` だけを呼ぶ (= コマンド文字列の単一情報源)。ツールは flake.nix の
# devShell が供給する (ESP-IDF も含めてすべて Nix 固定。~/esp は前提にしない)。
#
# 使い方:
#   just              # 一覧
#   just test-host    # core/ のホストテスト
#   just build        # 実機ファーム

# ホストビルドのディレクトリ。テストとエミュレータランナーを同じツリーで作る。
host_build := "build-host"
san_build  := "build-san"

# 既定ターゲット: 引数なしなら一覧を出す。
default:
    @just --list

# ───── ホスト側 (core/ の開発。M1-M3 の主戦場) ──────────────────────────────
#
# core/ は ESP32 非依存の純粋 C++17。68000 コアもデバイスも Mac 上で動くので、
# 実機に焼かずに Human68k の起動までデバッグできる。これが開発速度を決める。

[doc('core/ のホストテスト (doctest) をビルドして実行する')]
test-host:
    cmake -S test -B {{host_build}} -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build {{host_build}}
    ctest --test-dir {{host_build}} --output-on-failure

# ASan/UBSan は 68000 コアのメモリ破壊 (EA 計算ミスによる配列外アクセス等) を
# 一発で捕まえる。エミュレータ開発では通常テストより価値が高い場面が多い。
[doc('ASan/UBSan 付きでホストテストを実行する')]
test-san:
    cmake -S test -B {{san_build}} -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
    cmake --build {{san_build}}
    ctest --test-dir {{san_build}} --output-on-failure

[doc('ホストのエミュレータランナー x68k-run をビルドする')]
build-host:
    cmake -S test -B {{host_build}} -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build {{host_build}} --target x68k-run

# ROM は同梱していないので rom/ に置く (NOTICE.md 参照)。
# 例: just run --png /tmp/out.png --trace /tmp/trace.txt
[doc('ホストで X68000 を起動する (rom/ に IPLROM とディスクイメージが必要)')]
run *ARGS: build-host
    ./{{host_build}}/x68k-run --iplrom rom/iplrom.dat {{ARGS}}

# ───── 68000 コアの適合性テスト ─────────────────────────────────────────────
#
# SingleStepTests/m68000 (MIT) は MAME のマイクロコード実装から生成された
# 命令ごとの JSON テストベクタ。自作 68000 コアの正しさを機械的に保証する要。
# 数 GB あるのでリポジトリには同梱しない。

[doc('68000 テストベクタ (SingleStepTests/m68000) を取得する')]
fetch-tests:
    ./tools/fetch_processor_tests.sh

[doc('68000 適合性テストをフル実行する (fetch-tests 済みが前提)')]
test-vectors: build-host
    ./{{host_build}}/x68k_tests --test-suite=json_full

# ───── 実機 (M5Stack CoreS3 / ESP32-S3) ────────────────────────────────────

[doc('実機ファームをビルドする')]
build:
    idf.py build

[doc('実機ファームをビルドして CoreS3 に書き込む')]
flash:
    idf.py flash

[doc('CoreS3 のシリアル出力を読む')]
monitor:
    idf.py monitor

[doc('書き込んでそのままシリアルを読む')]
run-device:
    idf.py flash monitor

# .iram0 / .dram0 の overflow はリンク時にしか分からず、しかも「あと少しで溢れる」
# 状態は気付きにくい。68000 のホット命令を IRAM_ATTR に足すたびにこれを見る。
[doc('バイナリサイズの内訳を出す (IRAM/DRAM の残量監視)')]
size:
    idf.py size
    idf.py size-components

[doc('sdkconfig を編集する')]
menuconfig:
    idf.py menuconfig

# ───── lint / format ───────────────────────────────────────────────────────

# fd の -E doctest は vendored な third-party ヘッダを整形対象から外すため。
# upstream との差分が追えなくなるうえ、更新のたびに巨大な diff が出る。
# なお host/ (ホスト用エミュレータランナー) は M2 で作る。作ったら対象に足すこと。
[doc('C/C++ と Python を整形する')]
fmt:
    fd -e c -e h -e cpp -e hpp . src test -E doctest --exec clang-format -i
    uv run ruff format tools

[doc('整形されているか検査する (CI 用。書き換えない)')]
fmt-check:
    fd -e c -e h -e cpp -e hpp . src test -E doctest --exec clang-format --dry-run --Werror
    uv run ruff format --check tools

[doc('Python を lint する')]
lint:
    uv run ruff check tools

# clang-tidy は compile_commands.json (CMake が生成) を必要とする。
# core/ は純粋 C++17 なのでホストビルドの compile_commands で解析できる。
# platform/ と ESP-IDF ビルドは xtensa ヘッダで誤検知が出るため対象外。
[doc('core/ を clang-tidy で静的解析する')]
tidy:
    cmake -S test -B {{host_build}} -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    fd -e c -e cpp . src/x68k/core --exec clang-tidy -p {{host_build}}

# ───── 集約 ────────────────────────────────────────────────────────────────

[doc('CI と同じ検査を一括で回す')]
check: fmt-check lint tidy test-host

# core/ に ESP32 依存が混入していないことを検査する。
# core/ がホストで動くことが本プロジェクトの開発速度の前提なので、CI と
# pre-commit の両方でガードする。
[doc('core/ が ESP32 非依存であることを検査する')]
core-guard:
    #!/usr/bin/env bash
    set -euo pipefail
    if grep -rEn '^[[:space:]]*#[[:space:]]*include.*(Arduino\.h|M5Unified|M5GFX|LovyanGFX|lgfx|esp_|freertos/|driver/|soc/|sdkconfig\.h|hal/)' src/x68k/core/; then
      echo "::error::ESP32-only #include found in src/x68k/core/ (must stay ESP32-independent)"
      exit 1
    fi
    echo "core/ is ESP32-independent: OK"

[doc('GitHub Actions のピン留めを検証する (書き換えない)')]
pinact-verify:
    pinact run --verify --check

[doc('GitHub Actions のピン留めを最新の SHA へ更新する')]
pin:
    pinact run

[doc('シークレットスキャンを全履歴に対して回す')]
gitleaks:
    gitleaks git --no-banner --redact

# ───── ツール ──────────────────────────────────────────────────────────────

# CGROM はシャープの無償公開対象外で入手できないことがある。一方 IPLROM には
# 6x12 の ANK フォントが入っている ($FFCFF6 付近, 254 文字)。これを抽出できれば
# CGROM 無しでも英数字コンソールを出せる。まず目視で確認するためのツール。
[doc('IPLROM 内の 6x12 ANK フォントを抽出して PNG で確認する')]
extract-font IPLROM OUT="/tmp/ank6x12.png":
    uv run tools/extract_font.py {{IPLROM}} --out {{OUT}}

[doc('Human68k の FD イメージから SASI HDD イメージを作る')]
make-hdd FD OUT:
    uv run tools/make_sasi_image.py --fd {{FD}} --out {{OUT}}

[doc('ビルド成果物を消す')]
clean:
    rm -rf {{host_build}} {{san_build}} build
