{
  # x68k-stackchan の開発環境。ツールチェーンは「すべて」Nix で固定する。
  #
  # Why 全部 Nix (kexi 配下の他リポジトリとの差分):
  #   stackchan-dapan / oink-chan / stackchan-doom は「ESP-IDF 本体と xtensa
  #   toolchain は Nix の外 (~/esp/esp-idf/export.sh)」という方針だった。
  #   本リポジトリはそれを改め、ESP-IDF・xtensa toolchain・clang/gcc・python まで
  #   devShell に閉じる。理由は 2 つ:
  #     1. CI で実機ファームのビルドを再現するのに docker image や外部 SDK の
  #        セットアップが要らなくなる (nix develop -c just build で完結する)。
  #     2. ~/esp を前提にすると、非対話シェル (Claude の Bash tool 等) や
  #        clone 直後の環境で idf.py が PATH に無く落ちる。この不安定を根絶する。
  #
  # 使い方: `nix develop`、または direnv (.envrc の `use flake`) で自動有効化。
  #   shell 内で `just <task>` (例 `just test-host` / `just build`)。
  description = "x68k-stackchan dev shell (ESP-IDF / xtensa toolchain / host test tools)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # ESP-IDF 本体 + xtensa-esp32s3-elf toolchain を Nix で供給する。
    # nixpkgs 本体には ESP-IDF の完全なツールチェーンが無いため、この overlay に依存する。
    #
    # Why not inputs.nixpkgs.follows = "nixpkgs":
    #   この overlay の pkgs/esp-idf/tools.nix は python310 を引数に取るが、
    #   nixpkgs-unstable では python310 が削除済みで
    #   'Function called without required argument "python310"' で評価が落ちる。
    #   follows を張らず upstream 自身が lock した nixpkgs (python310 が生きている世代)
    #   で ESP-IDF を評価させる。ホスト側のツール (clang/ruff/cmake 等) は
    #   こちらの新しい nixpkgs から取るので、両者を別々の pkgs として使い分ける。
    nixpkgs-esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      nixpkgs-esp-dev,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        # ホスト側のツール (cmake/clang/ruff/just/...) は新しい nixpkgs から取る。
        pkgs = import nixpkgs { inherit system; };

        # ESP-IDF と xtensa toolchain は nixpkgs-esp-dev が lock している nixpkgs で
        # 評価する (上の input コメント参照: python310 の有無で評価が変わるため)。
        # esptool が依存する ecdsa が insecure 指定なので、その pkgs 側で許可する。
        espPkgs = import nixpkgs-esp-dev.inputs.nixpkgs {
          inherit system;
          overlays = [ nixpkgs-esp-dev.overlays.default ];
          config.permittedInsecurePackages = [
            "python3.10-ecdsa-0.19.1"
            "python3.11-ecdsa-0.19.1"
            "python3.12-ecdsa-0.19.1"
            "python3.13-ecdsa-0.19.1"
          ];
        };
      in
      {
        devShells.default = pkgs.mkShell {
          packages = [
            # --- 実機ファーム (ESP32-S3 / CoreS3) ---
            # esp-idf-xtensa は idf.py + xtensa 系 (ESP32/S2/S3) の toolchain + esptool を含む。
            # Why not esp-idf-esp32s3: チップ別派生は upstream で deprecated
            #   (ESP-IDF 6.0 で削除予定)。Why not esp-idf-full: RISC-V 系の
            #   toolchain まで入り closure が肥大する。CoreS3 は ESP32-S3 = xtensa。
            espPkgs.esp-idf-xtensa

            # --- ホストビルド (core/ の doctest と host/ のエミュレータランナー) ---
            # core/ は ESP32 非依存の純粋 C++17 で、Mac 上で 68000 を走らせて
            # Human68k を起動させるのが開発の主戦場になる (M1-M3)。
            pkgs.cmake
            pkgs.ninja
            pkgs.clang
            pkgs.gcc

            # --- lint / format ---
            # clang-format / clang-tidy は clang パッケージではなく clang-tools が供給する。
            pkgs.clang-tools
            pkgs.ruff

            # X68000 の ROM と Human68k は LZH で配布されている
            # (2000 年当時の主流だった圧縮形式)。展開に使う。
            pkgs.lhasa

            # --- tools/ のスクリプト ---
            # uv 自体は Nix で固定し、python の依存は uv が pyproject.toml と
            # uv.lock から解決する。
            # Why not nixpkgs の pythonEnv に直接ライブラリを列挙するか:
            #   Pillow のようなライブラリのバージョンを nixpkgs の世代に縛られず
            #   uv.lock で固定できる。tools/ は開発補助スクリプト置き場で
            #   ファーム本体とは独立しているので、依存解決も分けておく方が素直。
            pkgs.uv
            # uv が使うベースの python。uv 自身は python を持ってこないので必要。
            pkgs.python3

            # --- タスクランナーと開発フロー ---
            pkgs.just
            pkgs.git
            pkgs.jq
            # justfile の fmt / tidy レシピがソースの列挙に使う。
            pkgs.fd
            pkgs.lefthook
            pkgs.gitleaks
            pkgs.pinact
            pkgs.actionlint
          ];

          shellHook = ''
            # lefthook.yml の git hook を登録する。冪等なので毎回実行してよい。
            # tarball checkout など .git が無い場合に落ちないようガードする。
            if [ -d .git ]; then
              # 失敗しても devShell には入れる。フックが無くても作業自体は
              # できるので、ここで止めると不便なだけ。ただし黙って進むと
              # 「pre-commit が動いていないことに誰も気付かない」状態になるので、
              # 出力は捨てずに警告を出す。
              if ! lefthook install; then
                echo "[x68k-stackchan] 警告: git hook を登録できませんでした。" >&2
                echo "[x68k-stackchan] pre-commit の検査が動きません。" >&2
              fi
            fi

            echo "[x68k-stackchan] idf.py $(idf.py --version 2>/dev/null | head -1) / just $(just --version | awk '{print $2}')"
            echo "[x68k-stackchan] ESP-IDF も Nix 供給。~/esp/esp-idf/export.sh は不要。"
          '';
        };

        formatter = pkgs.nixfmt-tree;
      }
    );
}
