// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// GUI デモ: SX-Window が踏むのと同じ手順で「窓のある画面」を組み立てる
// 68000 プログラムを生成し、その実行結果を合成して返す。
//
// 【このデモが示さないこと】
// SX-Window の起動は示さない。SX-Window のディスクイメージは入手できず、
// リポジトリにも無い (rom/ にあるのは iplrom.dat だけ)。ここで走るのは
// 「GUI が行う描画操作」を抜き出した合成プログラムであって、
// SX-Window そのものではない。示せるのは
// 「SX-Window が依存するハードウェア経路が、通しで叩いても絵になる」ところまで。
//
// なぜ矩形ではなく窓を描くのか: 単色矩形を 2 枚重ねるだけなら、
// 合成の優先順位が逆でも「色が違う板が 2 枚」に見えてしまい、
// 目視では破綻に気付けない。タイトルバー・枠・影・重なりを持つ窓なら、
// 前後関係が狂った瞬間に「奥の窓のタイトルバーが手前の窓を突き抜ける」
// といった形で目に見える。目視用の入口としてはこちらの方が働く。
//
// なぜランナーとテストで共有するのか: 以前はランナー側とテスト側で
// 別々のプログラムを持っていた。目視で確かめた絵と、テストが数値で
// 検査する絵が別物だと、「テストは通るが絵は壊れている」を許してしまう。
// 生成を一箇所に寄せて、テストは同じ絵のピクセルを検査する。
//
// 実機の根拠 (rom/iplrom.dat = EXPERT 用 IPLROM v1.0、$FE0000 がファイル先頭):
//
//   $FF642E: 33fc 0020 00e8 2600 33fc 06e4 00e8 2500
//            = MOVE.W #$0020,$E82600 / MOVE.W #$06E4,$E82500
//            起動時の既定プライオリティ。$E82500 の値はこの形式で組む。
//
//   $FF147E: 33fc 0005 00e9 8000 33fc 0062 00e9 8000
//            = MOVE.W #$0005,$E98000 / MOVE.W #$0062,$E98000
//            WR5 を選んで $62 (RTS on) を書く = マウス有効化。
//            下の emitEnableMouse() はこの 2 命令をそのまま再現する。

#ifndef X68K_HOST_GUI_DEMO_H
#define X68K_HOST_GUI_DEMO_H

#include <cstddef>
#include <vector>

#include "machine.h"
#include "memmap.h"
#include "video/graphic_raster.h"

namespace x68k::guidemo
{

// 画面の大きさ。16 色 512x512 (SX-Window の 256 色 768x512 ではない。
// 理由は下の kCrtcMode のコメント)。
inline constexpr u32 kScreenWidth = 512;
inline constexpr u32 kScreenHeight = 512;

// パレット番号。0 は透明なので窓の色には使えない。
inline constexpr u16 kColorDesktop = 1;    // 背景 (濃い青)
inline constexpr u16 kColorWindow = 2;     // 窓の地 (灰)
inline constexpr u16 kColorTitleBar = 3;   // タイトルバー (青)
inline constexpr u16 kColorFrame = 4;      // 枠 (黒に近い)
inline constexpr u16 kColorHighlight = 5;  // 立体の明るい辺 (白)
inline constexpr u16 kColorShadow = 6;     // 影 (暗い灰)
inline constexpr u16 kColorCursor = 7;     // マウスカーソル (白)

// 窓の位置。手前の窓が奥の窓に一部かぶるように置く。
// 重なりが無いと「前後関係が正しいか」を絵から読めない。
struct WindowRect
{
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

inline constexpr WindowRect kBackWindow = {48, 96, 240, 170};
inline constexpr WindowRect kFrontWindow = {160, 190, 260, 190};

// マウスカーソルの初期位置と、moveMouse で与える移動量。
// ゲスト側が SCC から読んだ移動量でカーソルを描き直すので、
// この値がそのまま絵に出る。
//
// 移動量が ±127 に収まる値なのは Scc::saturateDelta の飽和を避けるため。
// 1 レポートの移動量は符号付き 8bit なので、128 を渡すと 127 に丸められ、
// ホストが描いた位置とゲストが計算した位置が 1 ドットずれる。
// そのずれは「カーソルが出ている」だけ見ていると気付けないので、
// 最初から飽和しない値を選ぶ。
inline constexpr u32 kCursorStartX = 200;
inline constexpr u32 kCursorStartY = 140;
inline constexpr int kMouseDx = 96;
inline constexpr int kMouseDy = 120;

// デモの実行結果。
struct Result
{
    bool ok = false;
    u32 instructions = 0;        // 実行した命令数
    bool mouseEnabled = false;   // ゲストが SCC を設定できたか
    bool mouseAccepted = false;  // moveMouse がレポートを積めたか
    u32 cursorX = 0;             // ゲストが計算した最終カーソル位置
    u32 cursorY = 0;
    u16 haltedOpcode = 0;           // isHalted のときの未実装命令
    const char* failure = nullptr;  // 失敗理由 (成功なら nullptr)
};

// デモを走らせ、合成済みのピクセル (RGB565) を pixels へ書く。
// pixels は kScreenWidth * kScreenHeight 要素を持つこと。
Result run(u16* pixels);

}  // namespace x68k::guidemo

#endif  // X68K_HOST_GUI_DEMO_H
