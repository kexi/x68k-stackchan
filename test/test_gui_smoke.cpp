// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: SX-Window が前提とする GUI ハードウェアの経路が、
// 実機のソフトと同じ順序で叩かれたときに一体として機能すること。
//
// 【このテストが証明しないこと】
// SX-Window が起動することは証明しない。SX-Window のディスクイメージは
// 入手できず、リポジトリにも無い。ここで走らせるのは「GUI が行う操作」を
// 抜き出した合成プログラムであって、SX-Window そのものではない。
// 証明できるのは「SX-Window が依存する個々のハードウェア経路が、
// 組み合わせても壊れない」ところまで。実際の起動は別途イメージが要る。
//
// なぜ合成プログラムなのか: 既存のテストは経路ごとに分かれている。
// test_gvram_bus.cpp はバスの折り込みを、test_graphic_raster.cpp は
// ラスタの重ね合わせを、test_scc.cpp はマウスの framing を個別に見る。
// どれも単体では正しくても、「モード設定 → パレット → 描画 → 合成」を
// 通しで行ったときに食い違う余地が残る。実際、バスとラスタがピクセル配置で
// 食い違っていたバグは、VRAM の状態を手で組み立てるテストでは見つからず、
// バスを通して初めて出た。だからここでは 68000 のコードから
// $C00000 の窓へ書く、という実機のソフトと同じ入口だけを使う。
//
// 実機の根拠 (rom/iplrom.dat = EXPERT 用 IPLROM v1.0、$FE0000 がファイル先頭):
//
//   $FF642E: 33fc 0020 00e8 2600 33fc 06e4 00e8 2500
//            = MOVE.W #$0020,$E82600 / MOVE.W #$06E4,$E82500
//            起動時の既定プライオリティ。$06E4 は SP=0 / TX=1 / GR=2、
//            GP3=3 / GP2=2 / GP1=1 / GP0=0。本テストの priority 値は
//            この形式に従って組み立てる。
//
//   $FF147E: 33fc 0005 00e9 8000 33fc 0062 00e9 8000
//            = MOVE.W #$0005,$E98000 / MOVE.W #$0062,$E98000
//            WR5 を選んで $62 (RTS on) を書く = マウス有効化。
//            下の enableMouse() はこの 2 命令をそのまま再現する。
//
//   $FFB0A0: 1 ドットを MOVE.W で書き、値はゼロ拡張された 4bit
//            (16 色モード)。本テストの塗りつぶしも同じ形を採る。

#include <vector>

#include "doctest.h"
#include "machine.h"
#include "video/graphic_raster.h"

namespace
{

// --- 68000 の小さなアセンブラ -----------------------------------------------
//
// バイト配列を直書きせずヘルパを通すのは、命令ごとに「何をしているか」を
// 呼び出し側に残すため。生の配列だと、後からプログラムを読み解くのに
// 逆アセンブルが要る。
//
// Why not test_boot.cpp の RomBuilder を使い回さないか: あちらは IPL-ROM
// 領域 ($FE0000 以降) へ書き、リセットベクタ経由で起動する形に固定されている。
// ここで確かめたいのは「メインメモリに載ったプログラムがバス越しに GUI を
// 描く」経路で、ROM から動かすとバスの経路が ROM 読み出し側に寄ってしまう。
class Asm
{
public:
    explicit Asm(x68k::u32 origin) : origin_(origin) {}

    void emitWord(x68k::u16 value)
    {
        code_.push_back(static_cast<x68k::u8>(value >> 8));
        code_.push_back(static_cast<x68k::u8>(value & 0xFFu));
    }

    void emitLong(x68k::u32 value)
    {
        emitWord(static_cast<x68k::u16>(value >> 16));
        emitWord(static_cast<x68k::u16>(value & 0xFFFFu));
    }

    // MOVE.W #imm,(xxx).L  — I/O レジスタと VRAM への 1 ワード書き込み。
    // IPL-ROM が $E82500 等へ書くときの形と同じ ($FF642E)。
    void moveWordImmToAbs(x68k::u16 imm, x68k::u32 addr)
    {
        emitWord(0x33FC);
        emitWord(imm);
        emitLong(addr);
    }

    // LEA (xxx).L,An — 描画ループの書き込み先を作る。
    void leaAbs(x68k::u32 addr, x68k::u32 areg)
    {
        emitWord(static_cast<x68k::u16>(0x41F9u | (areg << 9)));
        emitLong(addr);
    }

    // MOVEQ #imm,Dn — ループカウンタと色番号を置く。
    void moveq(x68k::s8 imm, x68k::u32 dreg)
    {
        emitWord(static_cast<x68k::u16>(0x7000u | (dreg << 9) |
                                        (static_cast<x68k::u16>(static_cast<x68k::u8>(imm)))));
    }

    // MOVE.W #imm,Dn — MOVEQ で表せない 16bit の値を置く。
    void moveWordImmToDreg(x68k::u16 imm, x68k::u32 dreg)
    {
        emitWord(static_cast<x68k::u16>(0x303Cu | (dreg << 9)));
        emitWord(imm);
    }

    // MOVE.W Dn,(An)+ — 1 ドット書いて次のドットへ進む。
    //
    // 16 色モードの G-VRAM は 1 ドット 1 ワードで、ページの選択は
    // アドレス (どの窓か) が決める。IPL-ROM $FFB0A0 も MOVE.W (A0)+ で
    // 1 ドットずつ書く。
    void moveWordDregToAregPostInc(x68k::u32 dreg, x68k::u32 areg)
    {
        emitWord(static_cast<x68k::u16>(0x30C0u | (areg << 9) | dreg));
    }

    // ADDA.W #imm,An — 次のラスタへ進むための加算。
    void addaWordImm(x68k::u16 imm, x68k::u32 areg)
    {
        emitWord(static_cast<x68k::u16>(0xD0FCu | (areg << 9)));
        emitWord(imm);
    }

    // DBRA Dn,<label> — ループ。条件は常に偽なので純粋なカウンタ。
    //
    // 分岐先はディスプレースメントワード自身のアドレスが基準になる
    // (68000 の仕様)。ラベルの解決は現在位置から逆算する。
    void dbra(x68k::u32 dreg, std::size_t targetOffset)
    {
        emitWord(static_cast<x68k::u16>(0x51C8u | dreg));
        // ディスプレースメントワードの位置 = 今から書く場所。
        const std::size_t dispPos = code_.size();
        const auto disp = static_cast<x68k::s16>(static_cast<x68k::s32>(targetOffset) -
                                                 static_cast<x68k::s32>(dispPos));
        emitWord(static_cast<x68k::u16>(disp));
    }

    // STOP #imm — 実行の終わりを示す。
    //
    // Why not BRA 自分自身 (無限ループ) にしないか: テスト側は
    // 「プログラムが終わった」ことを検出したい。無限ループだと
    // サイクル数を数えて打ち切るしかなく、命令を足すたびに調整が要る。
    // STOP なら CPU が停止状態に入るので、そこを終了条件にできる。
    // 割り込みは使わないので停止したまま戻らない。
    void stop(x68k::u16 imm)
    {
        emitWord(0x4E72);
        emitWord(imm);
    }

    [[nodiscard]] std::size_t here() const
    {
        return code_.size();
    }

    [[nodiscard]] x68k::u32 addressHere() const
    {
        return origin_ + static_cast<x68k::u32>(code_.size());
    }

    [[nodiscard]] const std::vector<x68k::u8>& code() const
    {
        return code_;
    }

    [[nodiscard]] x68k::u32 origin() const
    {
        return origin_;
    }

private:
    x68k::u32 origin_;
    std::vector<x68k::u8> code_;
};

// --- 画面の定数 -------------------------------------------------------------

// プログラムを置く番地。ベクタ領域 ($0-$3FF) と IOCS ワークを避ける。
constexpr x68k::u32 kProgramBase = 0x010000u;

// G-VRAM のページ窓。ページ N の先頭 = $C00000 + N * $80000。
// 根拠は IPL-ROM $FFAEE8 の計算 (test_gvram_bus.cpp 冒頭に引用がある)。
constexpr x68k::u32 kPageWindow = 0x80000u;

// 16 色モードで使うパレット番号。0 は透明なので 1 から使う。
constexpr x68k::u16 kColorBack = 1;   // 奥のページの矩形 (赤)
constexpr x68k::u16 kColorFront = 2;  // 手前のページの矩形 (青)

// パレットの色 (X68000 形式 GGGGGRRRRRBBBBBI)。
constexpr x68k::u16 kPaletteRed = 0x07C0;
constexpr x68k::u16 kPaletteBlue = 0x003E;
constexpr x68k::u16 kPaletteTextWhite = 0xFFFF;

// 矩形の位置。奥 (ページ 1) と手前 (ページ 0) を一部だけ重ねる。
//
// 重なりと非重なりの両方を作るのが要点。全面が重なっていると
// 「手前だけ描かれている」のか「正しく隠している」のかを区別できない。
constexpr x68k::u32 kBackRectX = 20;
constexpr x68k::u32 kBackRectY = 20;
constexpr x68k::u32 kBackRectW = 40;
constexpr x68k::u32 kBackRectH = 30;

constexpr x68k::u32 kFrontRectX = 40;
constexpr x68k::u32 kFrontRectY = 30;
constexpr x68k::u32 kFrontRectW = 40;
constexpr x68k::u32 kFrontRectH = 30;

// 手前のページに開ける「穴」。1 ドットだけ透明 (パレット 0) のまま残す。
//
// 重なりの内側に置くので、ここには奥のページの色が出るはず。
// 塗り残しではなく意図した透明であることを、周囲との対比で示す。
constexpr x68k::u32 kHoleX = 50;
constexpr x68k::u32 kHoleY = 40;

// G-VRAM の 1 ライン (ワード単位で 512 ドット、バイトで 1024)。
constexpr x68k::u32 kBytesPerLine = x68k::kGvramBytesPerLine;

// $E82500 のプライオリティ値。
//
// 形式は video.h に従う: bit13-12=SP / bit11-10=TX / bit9-8=GR /
// bit7-6=GP3 / bit5-4=GP2 / bit3-2=GP1 / bit1-0=GP0。値が小さいほど手前。
//
// グラフィックを手前 (GR=0)、テキストを奥 (TX=1) にする。
// ページは GP0=0 (手前) / GP1=1 (奥) で、ページ 0 がページ 1 を隠す形。
//   TX=1 → bit11-10 = 01 → $0400
//   GR=0 → bit9-8   = 00
//   GP1=1 → bit3-2  = 01 → $0004
//   GP0=0 → bit1-0  = 00
constexpr x68k::u16 kPriorityGraphicFront = 0x0404;

// テキストを手前にした版。GR=1 / TX=0 で上の逆。
// ページの並び (GP1=1, GP0=0) はそのまま。
constexpr x68k::u16 kPriorityTextFront = 0x0104;

// ページの順位だけを入れ替えた版。GP1=0 (手前) / GP0=1 (奥)。
// 面の順位は kPriorityGraphicFront と同じ (TX=1 / GR=0)。
//   TX=1  → bit11-10 = 01 → $0400
//   GP1=0 → bit3-2   = 00
//   GP0=1 → bit1-0   = 01 → $0001
//
// なぜこの値が要るか: GP0=0 / GP1=1 という「番号順と一致する」並びだけを
// 試すと、$E82500 を一切読まずページ番号順で重ねる実装でも同じ絵になる。
// 実際、graphic_raster.cpp の並べ替えは同順位のとき番号の小さい方を手前に
// する規則を持つので、全ページの順位を 0 に潰しても番号順に落ちて
// 検査を通り抜けてしまう (ミューテーションで確認した)。
// 番号順と逆の並びを 1 つ入れて初めて、レジスタが読まれている証拠になる。
constexpr x68k::u16 kPriorityPage1Front = 0x0401;

// $E82600 の表示制御。
// bit4=GS4 (グラフィック全体)、bit0=ページ 0、bit1=ページ 1、bit5=テキスト。
// 16 色モードなので GS3-GS0 が 1bit = 1 ページとして効く。
constexpr x68k::u16 kDisplayGraphicPagesAndText = 0x0033;

// $E82400 の画面モード。16 色 (bit1-0 = 00)、512x512 (bit2 = 0)。
constexpr x68k::u16 kScreenMode16Color = 0x0000;

// --- 合成 GUI プログラム -----------------------------------------------------

// 塗りつぶし矩形を描くコードを吐く。
//
// 1 ドット 1 ワードで、ページの選択はアドレス (どの窓か) が決める。
// ラスタごとに行頭へ戻して次の行へ進む。
//
//   A0 = 書き込み先、D0 = 色、D1 = 行カウンタ、D2 = 桁カウンタ
//
// Why not 1 ラスタを MOVEM でまとめて書かないか: 実機の描画も 1 ドットずつ
// MOVE.W で書く ($FFB0A0)。まとめ書きにすると「バスがワード書き込みごとに
// 正しいニブルへ折り込めるか」という、ここで確かめたい性質を迂回してしまう。
void emitFillRect(Asm& a, x68k::u32 page, x68k::u32 rx, x68k::u32 ry, x68k::u32 w, x68k::u32 h,
                  x68k::u16 color)
{
    const x68k::u32 base = x68k::kGvramBase + page * kPageWindow + ry * kBytesPerLine + rx * 2u;

    a.leaAbs(base, 0);
    a.moveWordImmToDreg(color, 0);
    // DBRA は -1 で抜けるので、回数 - 1 を入れる。
    a.moveWordImmToDreg(static_cast<x68k::u16>(h - 1), 1);

    // 行ループの先頭。
    const std::size_t rowLoop = a.here();
    a.moveWordImmToDreg(static_cast<x68k::u16>(w - 1), 2);

    // 桁ループの先頭。
    const std::size_t colLoop = a.here();
    a.moveWordDregToAregPostInc(0, 0);  // MOVE.W D0,(A0)+
    a.dbra(2, colLoop);

    // 次のラスタの行頭へ。今 A0 は w ドットぶん進んでいるので、
    // 1 ライン (1024 バイト) から進んだぶんを引いた量を足す。
    a.addaWordImm(static_cast<x68k::u16>(kBytesPerLine - w * 2u), 0);
    a.dbra(1, rowLoop);
}

// テキスト VRAM へ 1 ワードぶんのビットパターンを書く。
//
// テキスト画面はプレーン分割のビットマップで、プレーン 0 の 1bit が
// パレット番号の bit0 になる。文字の形そのものは問わない
// (CGROM の有無に左右されると、確かめたい合成の話から離れる)。
void emitTextPattern(Asm& a, x68k::u32 tx, x68k::u32 ty, x68k::u16 bits)
{
    const x68k::u32 addr = x68k::kTvramBase + ty * x68k::kTvramBytesPerLine + (tx / 8u);
    a.moveWordImmToAbs(bits, addr);
}

// SX-Window が起動時に行う順序で GUI の下地を作るプログラムを組む。
//
//   1. 画面モードを 16 色 512x512 にする   ($E82400)
//   2. グラフィックパレットを設定する       ($E82000)
//   3. プライオリティと表示許可を設定する   ($E82500 / $E82600)
//   4. 奥のページ (1) に矩形を描く          ($C80000 の窓)
//   5. 手前のページ (0) に重なる矩形を描く  ($C00000 の窓)
//   6. テキスト VRAM にパターンを書く       ($E00000)
//
// 順序を実機に合わせるのは、モードを決める前にパレットや VRAM を触ると
// バスの折り込み方が変わるため。SX-Window も同じ順で初期化する。
Asm buildGuiProgram(x68k::u16 priority)
{
    Asm a(kProgramBase);

    // 1. 画面モード。以降のバス書き込みの折り込み方がこれで決まる。
    a.moveWordImmToAbs(kScreenMode16Color, 0xE82400);

    // 2. グラフィックパレット。$E82000 + 番号 * 2。
    //    番号 0 は透明なので色を入れても出ないが、実機と同じく黒にしておく。
    a.moveWordImmToAbs(0x0000, 0xE82000 + 0 * 2);
    a.moveWordImmToAbs(kPaletteRed, 0xE82000 + kColorBack * 2);
    a.moveWordImmToAbs(kPaletteBlue, 0xE82000 + kColorFront * 2);

    // テキストパレット。$E82200 + 番号 * 2。番号 1 を白にする。
    a.moveWordImmToAbs(kPaletteTextWhite, 0xE82200 + 1 * 2);

    // 3. プライオリティと表示許可。IPL-ROM $FF642E と同じ 2 命令の形。
    a.moveWordImmToAbs(priority, 0xE82500);
    a.moveWordImmToAbs(kDisplayGraphicPagesAndText, 0xE82600);

    // 4. 奥のページ (1) の矩形。CPU バス越しに $C80000 の窓へ書く。
    emitFillRect(a, 1, kBackRectX, kBackRectY, kBackRectW, kBackRectH, kColorBack);

    // 5. 手前のページ (0) の矩形。奥と一部が重なる。
    emitFillRect(a, 0, kFrontRectX, kFrontRectY, kFrontRectW, kFrontRectH, kColorFront);

    // 5b. 手前のページに透明の穴を 1 ドット開ける。
    //
    // 塗りつぶした後にパレット 0 を書き戻す。重なりの内側なので、
    // 透明が効いていれば奥のページの色が見えるはず。
    a.moveWordImmToAbs(0x0000,
                       x68k::kGvramBase + 0 * kPageWindow + kHoleY * kBytesPerLine + kHoleX * 2u);

    // 6. テキスト VRAM。プレーン 0 に $FFFF (16 ドット連続) を書く。
    emitTextPattern(a, 0, 0, 0xFFFF);

    a.stop(0x2700);
    return a;
}

// --- 実行環境 ---------------------------------------------------------------

// Machine にメモリを与えてプログラムを走らせる一式。
//
// IPL-ROM は使わない。リセットベクタから起動すると IPL-ROM の初期化が
// 先に走り、そこで書かれる画面モードやパレットが混ざって、
// 何がこのプログラムの結果なのか分からなくなる。
// CPU の状態を直接置いてプログラムの先頭から始める。
struct GuiFixture
{
    std::vector<x68k::u8> mainRam;
    std::vector<x68k::u8> textVram;
    std::vector<x68k::u8> graphicVram;
    x68k::Machine machine;

    GuiFixture()
        : mainRam(x68k::kMainRamSize, 0),
          textVram(x68k::kTvramSize, 0),
          // G-VRAM の実体は 512KB。アドレス空間は 2MB だが、窓が同じ
          // 実 VRAM を覆う (test_gvram_bus.cpp の $FFAAB4 の根拠を参照)。
          graphicVram(x68k::kTvramSize, 0)
    {
    }

    // プログラムを RAM へ載せ、リセットベクタ経由でそこから実行を始める。
    //
    // Why not PC を直接書き換えないか: M68k::reset() は $000000 の SSP と
    // $000004 の PC を読んでプリフェッチを張る。PC だけを差し替えると
    // プリフェッチキューが元のままになり、最初の 2 ワードが別の命令として
    // 実行される。ベクタを置いて正規の経路を通すほうが状態の作り方が
    // 実機に近く、崩れる余地も無い。
    void load(const Asm& program)
    {
        const auto& code = program.code();
        for (std::size_t i = 0; i < code.size(); ++i)
        {
            mainRam[program.origin() + i] = code[i];
        }

        // リセットベクタ。$000000 = SSP、$000004 = 初期 PC。
        writeLong(0x000000, x68k::kResetSsp);
        writeLong(0x000004, program.origin());

        x68k::MemoryMap memory;
        memory.mainRam = mainRam.data();
        memory.textVram = textVram.data();
        memory.graphicVram = graphicVram.data();
        // IPL-ROM は与えない。与えると $000000 が ROM の写像になり、
        // ここで置いたベクタではなく ROM 側のベクタが読まれる。
        machine.setMemory(memory);

        // ROM の写像を外してから reset する。実機ではエリアセット
        // ($E86001) への書き込みがこれを行うが、ここは IPL-ROM を
        // 積んでいないので写像そのものが意味を持たない。
        machine.bus().setRomMappedAtZero(false);
        machine.reset();
    }

    void writeLong(x68k::u32 addr, x68k::u32 value)
    {
        mainRam[addr + 0] = static_cast<x68k::u8>(value >> 24);
        mainRam[addr + 1] = static_cast<x68k::u8>(value >> 16);
        mainRam[addr + 2] = static_cast<x68k::u8>(value >> 8);
        mainRam[addr + 3] = static_cast<x68k::u8>(value);
    }

    // STOP に達するか、上限に達するまで走らせる。
    // STOP で止まったら true。
    bool runToStop(x68k::u32 maxInstructions = 2000000)
    {
        for (x68k::u32 i = 0; i < maxInstructions; ++i)
        {
            if (machine.isHalted())
            {
                return false;  // 未実装命令。プログラム側の問題
            }
            if (machine.cpu().state().stopped)
            {
                return true;
            }
            machine.step();
        }
        return false;
    }

    // 合成した画面を作り、指定座標の色を返す。
    //
    // 実機の表示経路と同じ composite() を通す。ここを render() だけに
    // すると、テキストとの重ね合わせが検査対象から外れる。
    [[nodiscard]] std::vector<x68k::u16> composite(x68k::u32 width, x68k::u32 height)
    {
        std::vector<x68k::u16> pixels(static_cast<std::size_t>(width) * height, 0);
        x68k::GraphicRaster::composite(graphicVram.data(), textVram.data(), machine.video(), 0, 0,
                                       width, height, pixels.data(), width);
        return pixels;
    }
};

// 合成画像から 1 ドットを取る。
x68k::u16 pixelAt(const std::vector<x68k::u16>& pixels, x68k::u32 width, x68k::u32 x, x68k::u32 y)
{
    return pixels[static_cast<std::size_t>(y) * width + x];
}

// 描画する矩形が収まる大きさ。全画面 (512x512) を毎回作る必要はない。
constexpr x68k::u32 kViewWidth = 128;
constexpr x68k::u32 kViewHeight = 96;

}  // namespace

// --- グラフィック経路 -------------------------------------------------------

TEST_CASE("合成 GUI プログラムが最後まで実行される")
{
    // 保証すること: モード設定・パレット・描画ループ・テキスト書き込みが
    // すべて実装済みの命令で構成され、未実装命令に当たらずに完走すること。
    //
    // 壊れると: 以降のすべての検査が「描かれていない」を「壊れている」と
    // 誤って報告する。まず完走を単独で確かめておく。
    GuiFixture f;
    f.load(buildGuiProgram(kPriorityGraphicFront));

    const bool reachedStop = f.runToStop();

    CHECK(reachedStop);
    CHECK_FALSE(f.machine.isHalted());
}

TEST_CASE("CPU バス越しの描画が G-VRAM の正しいページへ入る")
{
    // 保証すること: 68000 のコードが $C00000 / $C80000 の窓へ書いたドットが、
    // 実 VRAM の共有ワードの正しいニブルへ入ること。
    //
    // 壊れると: バスの折り込みとラスタの読み出しが食い違い、ページ 1 に
    // 描いた絵がページ 0 として読まれる。手で VRAM を組み立てるテストでは
    // 構造的に検出できない種類の食い違い。
    GuiFixture f;
    f.load(buildGuiProgram(kPriorityGraphicFront));
    REQUIRE(f.runToStop());

    // 奥のページ (1) だけが塗られている位置。
    CHECK(x68k::GraphicRaster::pixelIndex(f.graphicVram.data(),
                                          x68k::VideoController::GraphicColorMode::k16Color, 1,
                                          kBackRectX, kBackRectY) == kColorBack);
    // 同じ座標のページ 0 は透明のまま。書き込みが隣のページを侵していない。
    CHECK(x68k::GraphicRaster::pixelIndex(f.graphicVram.data(),
                                          x68k::VideoController::GraphicColorMode::k16Color, 0,
                                          kBackRectX, kBackRectY) == 0);

    // 手前のページ (0) だけが塗られている位置 (奥の矩形の外)。
    const x68k::u32 frontOnlyX = kFrontRectX + kFrontRectW - 1;
    const x68k::u32 frontOnlyY = kFrontRectY + kFrontRectH - 1;
    CHECK(x68k::GraphicRaster::pixelIndex(f.graphicVram.data(),
                                          x68k::VideoController::GraphicColorMode::k16Color, 0,
                                          frontOnlyX, frontOnlyY) == kColorFront);
    CHECK(x68k::GraphicRaster::pixelIndex(f.graphicVram.data(),
                                          x68k::VideoController::GraphicColorMode::k16Color, 1,
                                          frontOnlyX, frontOnlyY) == 0);

    // 重なった位置は、両方のページにそれぞれの色が入っている。
    // 同じワードに同居しても互いを潰していない。
    CHECK(x68k::GraphicRaster::pixelIndex(f.graphicVram.data(),
                                          x68k::VideoController::GraphicColorMode::k16Color, 0,
                                          kHoleX + 1, kHoleY) == kColorFront);
    CHECK(x68k::GraphicRaster::pixelIndex(f.graphicVram.data(),
                                          x68k::VideoController::GraphicColorMode::k16Color, 1,
                                          kHoleX + 1, kHoleY) == kColorBack);
}

TEST_CASE("手前のページの矩形が奥のページを隠す")
{
    // 保証すること: $E82500 の GP1-GP0 が示す順序どおりに、
    // 手前のページの不透明ドットが奥のページを覆うこと。
    //
    // 壊れると: SX-Window でウィンドウの重なりが表現できない。
    // 手前のウィンドウの下から背後のウィンドウが透けて見える。
    GuiFixture f;
    f.load(buildGuiProgram(kPriorityGraphicFront));
    REQUIRE(f.runToStop());

    const auto pixels = f.composite(kViewWidth, kViewHeight);
    const x68k::u16 red = x68k::VideoController::toRgb565(kPaletteRed);
    const x68k::u16 blue = x68k::VideoController::toRgb565(kPaletteBlue);

    // 重なった領域は手前 (ページ 0 = 青) が勝つ。
    CHECK(pixelAt(pixels, kViewWidth, kHoleX + 1, kHoleY) == blue);

    // 奥だけの領域は奥の色 (赤) が出る。
    CHECK(pixelAt(pixels, kViewWidth, kBackRectX, kBackRectY) == red);

    // 手前だけの領域は手前の色 (青)。
    CHECK(pixelAt(pixels, kViewWidth, kFrontRectX + kFrontRectW - 1,
                  kFrontRectY + kFrontRectH - 1) == blue);

    // どちらの矩形にも属さない位置は黒 (どのページも透明)。
    CHECK(pixelAt(pixels, kViewWidth, 5, 5) == 0);
}

TEST_CASE("ページの順位を逆にすると重なりが入れ替わる")
{
    // 保証すること: $E82500 の GP1-GP0 が実際に読まれていること。
    // VRAM の内容も表示許可も変えず、プライオリティレジスタだけを
    // 「ページ 1 が手前」に変えると、重なった領域の色が入れ替わる。
    //
    // なぜ番号順と逆の並びが要るか: GP0=0 / GP1=1 の場合だけを見ると、
    // レジスタを無視してページ番号順に重ねる実装でも同じ絵になってしまう。
    // graphic_raster.cpp の並べ替えは同順位なら番号の小さい方を手前にするので、
    // 全ページの順位を 0 に潰すミューテーションが番号順に落ちて素通りした。
    // 逆順を要求して初めて、レジスタを読まない実装を落とせる。
    //
    // 壊れると: SX-Window がウィンドウの前後関係を切り替えられない。
    // クリックしたウィンドウが最前面に来ない形で出る。
    GuiFixture front;
    front.load(buildGuiProgram(kPriorityGraphicFront));
    REQUIRE(front.runToStop());

    GuiFixture swapped;
    swapped.load(buildGuiProgram(kPriorityPage1Front));
    REQUIRE(swapped.runToStop());

    const x68k::u16 red = x68k::VideoController::toRgb565(kPaletteRed);
    const x68k::u16 blue = x68k::VideoController::toRgb565(kPaletteBlue);

    // VRAM の中身は 2 つの実行で同一。違いはプライオリティだけ。
    REQUIRE(front.graphicVram == swapped.graphicVram);

    const auto frontPixels = front.composite(kViewWidth, kViewHeight);
    const auto swappedPixels = swapped.composite(kViewWidth, kViewHeight);

    // 重なった領域: GP0 が手前なら青、GP1 が手前なら赤。
    const x68k::u32 overlapX = kHoleX + 1;
    const x68k::u32 overlapY = kHoleY;
    CHECK(pixelAt(frontPixels, kViewWidth, overlapX, overlapY) == blue);
    CHECK(pixelAt(swappedPixels, kViewWidth, overlapX, overlapY) == red);

    // 重なっていない領域はどちらの設定でも変わらない。
    // 入れ替わったのが「重なり順」であって「描いた絵」ではない証拠。
    CHECK(pixelAt(swappedPixels, kViewWidth, kBackRectX, kBackRectY) == red);
    CHECK(pixelAt(swappedPixels, kViewWidth, kFrontRectX + kFrontRectW - 1,
                  kFrontRectY + kFrontRectH - 1) == blue);
}

TEST_CASE("手前のページの透明ドットから奥のページが見える")
{
    // 保証すること: パレット番号 0 が透明として扱われ、背後のページの色が
    // 出ること。手前のページが「塗られていない」のではなく「透明」である
    // ことを、周囲が不透明であることと対にして示す。
    //
    // 壊れると: SX-Window のウィンドウの角丸や、アイコンの抜き部分が
    // 背景ではなく黒で塗り潰される。
    GuiFixture f;
    f.load(buildGuiProgram(kPriorityGraphicFront));
    REQUIRE(f.runToStop());

    const auto pixels = f.composite(kViewWidth, kViewHeight);
    const x68k::u16 red = x68k::VideoController::toRgb565(kPaletteRed);
    const x68k::u16 blue = x68k::VideoController::toRgb565(kPaletteBlue);

    // 穴の位置は手前が透明なので、奥のページの赤が出る。
    CHECK(pixelAt(pixels, kViewWidth, kHoleX, kHoleY) == red);
    // 穴の隣は手前の青。穴が塗り残しではなく 1 ドットの意図した透明である証拠。
    CHECK(pixelAt(pixels, kViewWidth, kHoleX + 1, kHoleY) == blue);
    CHECK(pixelAt(pixels, kViewWidth, kHoleX - 1, kHoleY) == blue);
}

// --- テキストとの合成 -------------------------------------------------------

TEST_CASE("グラフィックが手前ならテキストはグラフィックの下に隠れる")
{
    // 保証すること: $E82500 の GR/TX の順位が、面どうしの重ね合わせに
    // 反映されること。ここでは GR=0 (手前) / TX=1 (奥)。
    //
    // 壊れると: SX-Window の画面にテキスト画面の残骸が常に浮く。
    GuiFixture f;
    f.load(buildGuiProgram(kPriorityGraphicFront));
    REQUIRE(f.runToStop());

    // テキストは (0,0) から 16 ドット書いてある。グラフィックの矩形は
    // (20,20) 以降なので、テキストの位置はグラフィックが透明。
    // つまりテキストが見える。
    const auto pixels = f.composite(kViewWidth, kViewHeight);
    const x68k::u16 white = x68k::VideoController::toRgb565(kPaletteTextWhite);
    CHECK(pixelAt(pixels, kViewWidth, 0, 0) == white);

    // グラフィックが不透明な位置では、グラフィックが勝つ。
    const x68k::u16 blue = x68k::VideoController::toRgb565(kPaletteBlue);
    CHECK(pixelAt(pixels, kViewWidth, kFrontRectX, kFrontRectY) == blue);
}

TEST_CASE("テキストを手前にするとグラフィックの上に出る")
{
    // 保証すること: プライオリティレジスタだけを変えると、VRAM の内容は
    // 同じまま重なり順が入れ替わること。
    //
    // 壊れると: 面の順位が固定になり、$E82500 を書き換えても絵が変わらない。
    // SX-Window はメニューやカーソルの表示にこの切り替えを使う。
    //
    // テキストパターンを矩形の上に重ねる必要があるので、テキストの位置を
    // グラフィックの矩形内へずらしたプログラムを別に組む。
    Asm program(kProgramBase);
    {
        // 上の buildGuiProgram と同じ手順だが、テキストの書き込み位置だけ
        // 手前の矩形の内側にする。
        program.moveWordImmToAbs(kScreenMode16Color, 0xE82400);
        program.moveWordImmToAbs(0x0000, 0xE82000 + 0 * 2);
        program.moveWordImmToAbs(kPaletteRed, 0xE82000 + kColorBack * 2);
        program.moveWordImmToAbs(kPaletteBlue, 0xE82000 + kColorFront * 2);
        program.moveWordImmToAbs(kPaletteTextWhite, 0xE82200 + 1 * 2);
        // テキストを手前にする。
        program.moveWordImmToAbs(kPriorityTextFront, 0xE82500);
        program.moveWordImmToAbs(kDisplayGraphicPagesAndText, 0xE82600);
        emitFillRect(program, 1, kBackRectX, kBackRectY, kBackRectW, kBackRectH, kColorBack);
        emitFillRect(program, 0, kFrontRectX, kFrontRectY, kFrontRectW, kFrontRectH, kColorFront);
        // 手前の矩形の内側 (48,40) から 16 ドットぶんテキストを書く。
        // x=48 はバイト境界 (48/8=6) に乗るので 1 ワードがちょうど 48-63 を覆う。
        emitTextPattern(program, 48, 40, 0xFFFF);
        program.stop(0x2700);
    }

    GuiFixture f;
    f.load(program);
    REQUIRE(f.runToStop());

    const auto pixels = f.composite(kViewWidth, kViewHeight);
    const x68k::u16 white = x68k::VideoController::toRgb565(kPaletteTextWhite);
    const x68k::u16 blue = x68k::VideoController::toRgb565(kPaletteBlue);

    // グラフィックの矩形の内側でも、テキストのビットが立っている位置は白。
    CHECK(pixelAt(pixels, kViewWidth, 48, 40) == white);
    CHECK(pixelAt(pixels, kViewWidth, 63, 40) == white);

    // テキストのビットが無い位置 (同じ矩形内、ワードの外) はグラフィックの青。
    CHECK(pixelAt(pixels, kViewWidth, 64, 40) == blue);
    // テキストを書いていないラスタも青のまま。
    CHECK(pixelAt(pixels, kViewWidth, 48, 41) == blue);
}

TEST_CASE("表示を許可されていないページは合成に現れない")
{
    // 保証すること: $E82600 のページ許可ビットが実際に効くこと。
    //
    // 壊れると: SX-Window が裏画面として使っているページが表に出て、
    // 描画途中の絵が見える。
    //
    // ページ 0 だけを許可する ($E82600 = $0031: bit5=テキスト, bit4=GS4,
    // bit0=ページ 0)。ページ 1 の矩形は VRAM にあるが出ないはず。
    Asm program(kProgramBase);
    {
        program.moveWordImmToAbs(kScreenMode16Color, 0xE82400);
        program.moveWordImmToAbs(0x0000, 0xE82000 + 0 * 2);
        program.moveWordImmToAbs(kPaletteRed, 0xE82000 + kColorBack * 2);
        program.moveWordImmToAbs(kPaletteBlue, 0xE82000 + kColorFront * 2);
        program.moveWordImmToAbs(kPriorityGraphicFront, 0xE82500);
        // ページ 0 のみ表示。
        program.moveWordImmToAbs(0x0031, 0xE82600);
        emitFillRect(program, 1, kBackRectX, kBackRectY, kBackRectW, kBackRectH, kColorBack);
        emitFillRect(program, 0, kFrontRectX, kFrontRectY, kFrontRectW, kFrontRectH, kColorFront);
        program.stop(0x2700);
    }

    GuiFixture f;
    f.load(program);
    REQUIRE(f.runToStop());

    // VRAM にはページ 1 の絵がある。
    REQUIRE(x68k::GraphicRaster::pixelIndex(f.graphicVram.data(),
                                            x68k::VideoController::GraphicColorMode::k16Color, 1,
                                            kBackRectX, kBackRectY) == kColorBack);

    // だが合成結果には出ない。
    const auto pixels = f.composite(kViewWidth, kViewHeight);
    CHECK(pixelAt(pixels, kViewWidth, kBackRectX, kBackRectY) == 0);
    // ページ 0 の矩形は出る。
    const x68k::u16 blue = x68k::VideoController::toRgb565(kPaletteBlue);
    CHECK(pixelAt(pixels, kViewWidth, kFrontRectX, kFrontRectY) == blue);
}

// --- マウス経路 -------------------------------------------------------------

namespace
{

// IPL-ROM $FF147E と同じ手順でマウスを有効化する。
//
//   MOVE.W #$0005,$E98000   WR5 を選ぶ
//   MOVE.W #$0062,$E98000   $62 = RTS on
//
// 受信も有効にする必要がある (WR3 bit0)。ROM の初期化表は WR3 に $C1 を
// 書く ($FF0E4C の 10 番目)。ここでは最小限の 2 レジスタだけを設定する。
//
// Why not Scc を直接叩かないか: Machine::moveMouse() の経路を確かめたいので、
// ゲスト (68000 のコード) がバス越しに書いた設定でマウスが有効になることが
// 前提として要る。SCC を直接触ると、バスの I/O 経路が検査から外れる。
void emitEnableMouse(Asm& a)
{
    // WR3 = $C1 (受信 8bit + 受信有効)。ROM の初期化表と同じ値。
    a.moveWordImmToAbs(0x0003, 0xE98000);
    a.moveWordImmToAbs(0x00C1, 0xE98000);
    // WR5 = $62 (RTS on)。IPL-ROM $FF147E の値。
    a.moveWordImmToAbs(0x0005, 0xE98000);
    a.moveWordImmToAbs(0x0062, 0xE98000);
}

// SCC のチャネル B から 1 バイト読む。ゲストのハンドラと同じ経路
// ($FF1512 の MOVE.W $E98002,D0)。
x68k::u8 readMouseByte(x68k::Machine& machine)
{
    return static_cast<x68k::u8>(machine.bus().read8(0xE98003));
}

}  // namespace

TEST_CASE("ゲストが有効化したマウスのレポートが 3 バイトで届く")
{
    // 保証すること: 68000 のコードがバス越しに SCC を有効化すると
    // Machine::moveMouse() が受理され、ゲストから見える受信 FIFO に
    // (ボタン, X, Y) の 3 バイトが順に並ぶこと。
    //
    // 壊れると: SX-Window でカーソルが動かない。原因が SCC のレジスタか
    // バスの経路か framing かは症状から切り分けられない。
    Asm program(kProgramBase);
    emitEnableMouse(program);
    program.stop(0x2700);

    GuiFixture f;
    f.load(program);
    REQUIRE(f.runToStop());

    // ゲストの設定でマウスが有効になっている。
    REQUIRE(f.machine.scc().isMouseEnabled());

    // 右へ 10、下へ 5 動かす。ボタンは押していない。
    REQUIRE(f.machine.moveMouse(10, 5, false, false));

    // 3 バイト揃っている。
    CHECK(f.machine.scc().pendingBytes(x68k::Scc::kChannelB) == 3);

    // ボタンなし → $00、X = 10、Y = 5。
    CHECK(readMouseByte(f.machine) == 0x00);
    CHECK(readMouseByte(f.machine) == 10);
    CHECK(readMouseByte(f.machine) == 5);

    // 引き取ったら空になる。
    CHECK(f.machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);
}

TEST_CASE("ボタンの押下と解放が別々のレポートとして届く")
{
    // 保証すること: 押下 → 解放の順で送ったとき、ゲストが読む 1 バイト目が
    // 押下時は左ボタンのビット、解放時は 0 になること。
    //
    // 壊れると: SX-Window のドラッグが終わらない。解放が届かないと
    // ゲストはボタンを押しっぱなしと見なし続ける。scc.h が戻り値を持つ
    // 理由そのもの。
    Asm program(kProgramBase);
    emitEnableMouse(program);
    program.stop(0x2700);

    GuiFixture f;
    f.load(program);
    REQUIRE(f.runToStop());
    REQUIRE(f.machine.scc().isMouseEnabled());

    // 左ボタンを押しながら右へ 3。
    REQUIRE(f.machine.moveMouse(3, 0, true, false));
    // 左ボタンのビットは bit1 ($02)。右が bit0 なのは scc.cpp の
    // kMouseButtonLeft / kMouseButtonRight の根拠を参照。
    CHECK(readMouseByte(f.machine) == 0x02);
    CHECK(readMouseByte(f.machine) == 3);
    CHECK(readMouseByte(f.machine) == 0);

    // 解放。移動なし。
    REQUIRE(f.machine.moveMouse(0, 0, false, false));
    CHECK(readMouseByte(f.machine) == 0x00);
    CHECK(readMouseByte(f.machine) == 0);
    CHECK(readMouseByte(f.machine) == 0);

    CHECK(f.machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);
}

TEST_CASE("マウスの移動量が負でも符号付き 1 バイトで届く")
{
    // 保証すること: 左上へ動かしたときに 2 の補数として届くこと。
    //
    // 壊れると: カーソルが片方向にしか動かない、または逆へ飛ぶ。
    Asm program(kProgramBase);
    emitEnableMouse(program);
    program.stop(0x2700);

    GuiFixture f;
    f.load(program);
    REQUIRE(f.runToStop());
    REQUIRE(f.machine.scc().isMouseEnabled());

    REQUIRE(f.machine.moveMouse(-1, -2, false, false));
    CHECK(readMouseByte(f.machine) == 0x00);
    CHECK(readMouseByte(f.machine) == 0xFF);  // -1
    CHECK(readMouseByte(f.machine) == 0xFE);  // -2
}

TEST_CASE("ゲストが有効化する前のマウス操作は積まれない")
{
    // 保証すること: SCC を設定していない状態では moveMouse が断られること。
    //
    // 壊れると: 有効化した瞬間に、それまでの動きが一度に流れ込む。
    // 起動直後にカーソルが画面外へ飛ぶ形で出る。
    Asm program(kProgramBase);
    // 有効化せずに終わるだけのプログラム。
    program.stop(0x2700);

    GuiFixture f;
    f.load(program);
    REQUIRE(f.runToStop());

    CHECK_FALSE(f.machine.scc().isMouseEnabled());
    CHECK_FALSE(f.machine.moveMouse(10, 10, false, false));
    CHECK(f.machine.scc().pendingBytes(x68k::Scc::kChannelB) == 0);
}
