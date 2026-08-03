#!/usr/bin/env bash
# SingleStepTests/m68000 (MIT) の 68000 命令テストベクタを取得する。
#
# MAME の 68000 マイクロコード実装から生成された、命令ごとの JSON テストベクタ。
# 初期状態・最終状態に加えてバスアクティビティと UDS/LDS まで含む。
# 自作 68000 コアの正しさを機械的に検証する手段としてはこれが最も確実。
#
# 数 GB あるためリポジトリには同梱せず、必要になったときに取得する
# (.gitignore 済み)。CI ではサブセットだけを回す想定。
#
# 既知の制限 (upstream の README より):
#   - TAS: read-modify-write の 5 サイクルのタイミングが未実装
#   - TRAPV: S ビットの解釈に不明な問題がある
#   この 2 命令のテストは検証済みではないので、突き合わせから除外してよい。
set -euo pipefail

REPO_URL="https://github.com/SingleStepTests/m68000.git"
DEST="third_party/ProcessorTests"

if [ -d "$DEST/.git" ]; then
    echo "[fetch-tests] $DEST は取得済み。更新します。"
    git -C "$DEST" pull --ff-only
    exit 0
fi

mkdir -p "$(dirname "$DEST")"

# 履歴は要らないので depth=1。それでも数 GB あるので時間がかかる。
echo "[fetch-tests] $REPO_URL を $DEST に取得します (数 GB あります)..."
git clone --depth 1 "$REPO_URL" "$DEST"

echo "[fetch-tests] 完了: $DEST"
