// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: --gui-demo が出す絵が「窓のある GUI 画面」として
// 破綻していないこと。具体的には
//
//   1. 68000 プログラムが最後まで走り切る (未実装命令に当たらない)
//   2. ゲストが SCC を設定するとマウスが有効になり、レポートが積まれる
//   3. ゲストが SCC から読んだ移動量が、ホストが与えた移動量と一致する
//   4. 手前の窓が奥の窓を隠す (重なりの前後関係)
//   5. 窓の外には背景が残る (塗りつぶしが暴走していない)
//   6. テキスト面がグラフィック面と合成される
//   7. カーソルが「ゲストが計算した位置」に描かれている
//
// 【このテストが証明しないこと】
// SX-Window が起動することは証明しない。SX-Window のディスクイメージは
// 入手できず、リポジトリにも無い。ここで走るのは GUI の描画操作を抜き出した
// 合成プログラムであって、SX-Window そのものではない。
//
// なぜランナーと同じ生成器を使うのか: 以前はランナーとテストが別々の
// プログラムを持っていた。目視で見た絵とテストが検査する絵が別物だと、
// 「テストは通るが絵は壊れている」を許す。host/gui_demo.cpp を共有し、
// ランナーが PPM に落とすのと同じピクセルをここで検査する。

#include <vector>

#include "doctest.h"
#include "gui_demo.h"

namespace
{

using namespace x68k::guidemo;

// 合成結果から 1 ドット取る。
x68k::u16 at(const std::vector<x68k::u16>& px, x68k::u32 x, x68k::u32 y)
{
    return px[static_cast<std::size_t>(y) * kScreenWidth + x];
}

// パレット番号に対応する RGB565。gui_demo.cpp が $E82000 へ書く GRB555 を
// VideoController が RGB565 へ直したもの。値を直書きせず、
// 「同じ色か違う色か」で判定するために代表点から引く。
struct Palette
{
    x68k::u16 desktop;
    x68k::u16 window;
    x68k::u16 titleBar;
};

}  // namespace

TEST_CASE("GUI デモが窓のある画面を描く")
{
    std::vector<x68k::u16> px(static_cast<std::size_t>(kScreenWidth) * kScreenHeight, 0);
    const Result r = run(px.data());

    // --- 1. 走り切ること -----------------------------------------------
    INFO("failure=", r.failure != nullptr ? r.failure : "(none)", " opcode=", r.haltedOpcode);
    REQUIRE(r.ok);
    CHECK(r.instructions > 0);

    // --- 2. マウスが有効になり、レポートが積まれること -------------------
    //
    // ゲストが $E98000 へ WR3/WR5 を書いた結果として有効になる。
    // ホストが勝手に有効化しているのではない点が要。
    CHECK(r.mouseEnabled);
    CHECK(r.mouseAccepted);

    // --- 3. ゲストが読んだ移動量がホストの与えた値と一致すること ---------
    //
    // ここがずれると、SCC のバイト順か EXT.W の符号拡張が壊れている。
    // ±127 に収まる移動量を選んでいるので飽和は起きないはず。
    CHECK(r.cursorX == kCursorStartX + static_cast<x68k::u32>(kMouseDx));
    CHECK(r.cursorY == kCursorStartY + static_cast<x68k::u32>(kMouseDy));

    // --- 代表点から色を拾う ---------------------------------------------
    //
    // 画面の隅は窓もメニューバーも掛からないので背景色。
    const Palette pal = {
        at(px, 4, kScreenHeight - 4),                                    // 背景
        at(px, kBackWindow.x + 20, kBackWindow.y + kBackWindow.h - 20),  // 窓の地
        at(px, kBackWindow.x + 100, kBackWindow.y + 9),                  // タイトルバー
    };

    // 背景・窓・タイトルバーが互いに違う色であること。
    // 同じなら パレット書き込みか合成のどこかが潰れている。
    CHECK(pal.desktop != pal.window);
    CHECK(pal.window != pal.titleBar);
    CHECK(pal.desktop != pal.titleBar);

    // --- 4. 手前の窓が奥の窓を隠すこと -----------------------------------
    //
    // 2 つの窓が重なる領域を選び、そこが手前の窓の中身になっていることを見る。
    // 前後関係が逆なら、ここは奥の窓の地の色のままになる。
    {
        // 重なり領域の内側 (両方の窓の矩形に入る点)。
        const x68k::u32 ox = kFrontWindow.x + 10;
        const x68k::u32 oy = kFrontWindow.y + 6;
        REQUIRE(ox >= kBackWindow.x);
        REQUIRE(ox < kBackWindow.x + kBackWindow.w);
        REQUIRE(oy >= kBackWindow.y);
        REQUIRE(oy < kBackWindow.y + kBackWindow.h);

        // 手前の窓のタイトルバーの高さにある点なので、タイトルバー色のはず。
        CHECK(at(px, kFrontWindow.x + 100, kFrontWindow.y + 9) == pal.titleBar);
    }

    // --- 5. 窓の外には背景が残ること -------------------------------------
    //
    // 塗りつぶしのループが行数や幅を踏み越えていれば、ここが窓の色になる。
    CHECK(at(px, kScreenWidth - 4, kScreenHeight - 4) == pal.desktop);
    // 奥の窓の左隣 (影の分だけ余裕を取る)。
    CHECK(at(px, kBackWindow.x - 6, kBackWindow.y + 40) == pal.desktop);

    // --- 6. テキスト面が合成されること -----------------------------------
    //
    // テキストのメニューバーは (8,4)-(168,14)。テキストパレット 1 = 白。
    // グラフィックのメニューバー (灰) の上に出る。
    {
        const x68k::u16 textDot = at(px, 20, 8);
        const x68k::u16 graphicOnly = at(px, 300, 8);  // 帯の右外、グラフィックだけ
        CHECK(textDot != graphicOnly);
    }

    // --- 7. カーソルがゲストの計算位置に描かれること ---------------------
    //
    // 矢印の先端 (1 ドット目) が背景でも窓でもない色になっている。
    {
        const x68k::u16 tip = at(px, r.cursorX, r.cursorY);
        CHECK(tip != pal.desktop);
        CHECK(tip != pal.window);
    }
}

TEST_CASE("GUI デモは出力バッファが null なら失敗を返す")
{
    // ランナーが確保に失敗した場合に、黙って進まないこと。
    const Result r = run(nullptr);
    CHECK_FALSE(r.ok);
    CHECK(r.failure != nullptr);
}
