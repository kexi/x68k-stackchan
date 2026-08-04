// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// gui_demo.h の実装。68000 のコードを組み立てて走らせる。

#include "gui_demo.h"

#include <cstring>

namespace x68k::guidemo
{
namespace
{

// --- 68000 の小さなアセンブラ -----------------------------------------------
//
// バイト列を直書きせずヘルパを通すのは、命令ごとに「何をしているか」を
// 呼び出し側に残すため。生の配列だと後から逆アセンブルしないと読めない。

class Asm
{
public:
    explicit Asm(u32 origin) : origin_(origin) {}

    void word(u16 value)
    {
        code_.push_back(static_cast<u8>(value >> 8));
        code_.push_back(static_cast<u8>(value & 0xFFu));
    }

    void longWord(u32 value)
    {
        word(static_cast<u16>(value >> 16));
        word(static_cast<u16>(value & 0xFFFFu));
    }

    // MOVE.W #imm,(xxx).L — IPL-ROM $FF642E と同じ形。
    void moveWordImmToAbs(u16 imm, u32 addr)
    {
        word(0x33FC);
        word(imm);
        longWord(addr);
    }

    // MOVE.B #imm,(xxx).L
    void moveByteImmToAbs(u8 imm, u32 addr)
    {
        word(0x13FC);
        word(imm);
        longWord(addr);
    }

    // MOVEA.L #imm,An
    void moveaLongImm(u32 imm, u32 reg)
    {
        word(static_cast<u16>(0x207C | (reg << 9)));
        longWord(imm);
    }

    // MOVE.L #imm,Dn
    void moveLongImmToData(u32 imm, u32 reg)
    {
        word(static_cast<u16>(0x203C | (reg << 9)));
        longWord(imm);
    }

    // MOVE.W #imm,Dn
    void moveWordImmToData(u16 imm, u32 reg)
    {
        word(static_cast<u16>(0x303C | (reg << 9)));
        word(imm);
    }

    // MOVE.W Dn,(An)+
    void moveWordDataToPostInc(u32 dataReg, u32 addrReg)
    {
        word(static_cast<u16>(0x30C0 | (addrReg << 9) | dataReg));
    }

    // MOVE.B (xxx).L,Dn
    void moveByteAbsToData(u32 addr, u32 reg)
    {
        word(static_cast<u16>(0x1039 | (reg << 9)));
        longWord(addr);
    }

    // EXT.W Dn — 符号付き 8bit の移動量を 16bit へ広げる。
    void extWord(u32 reg)
    {
        word(static_cast<u16>(0x4880 | reg));
    }

    // ADD.W Dn,Dm
    void addWord(u32 src, u32 dst)
    {
        word(static_cast<u16>(0xD040 | (dst << 9) | src));
    }

    // SUBQ.W #1,Dn
    void subqWord(u32 imm, u32 reg)
    {
        word(static_cast<u16>(0x5140 | ((imm & 7u) << 9) | reg));
    }

    // ORI.B #imm,(xxx).L — テキスト VRAM のビットを立てる。
    //
    // Why not BSET を使わないか: BSET の即値形はビット番号を拡張ワードに
    // 取るので、プレーン内のビット位置を毎回計算し直すことになる。
    // ORI.B ならマスクをそのまま渡せて、既存のビットも壊さない。
    void oriByteImmToAbs(u8 imm, u32 addr)
    {
        word(0x0039);
        // ORI.B の即値は拡張ワードの下位バイト。上位バイトは無視される。
        word(imm);
        longWord(addr);
    }

    [[nodiscard]] std::size_t here() const
    {
        return code_.size();
    }

    // 「今の位置」へ後ろから戻る BNE を吐く。
    void branchNeBack(std::size_t target)
    {
        word(0x6600);
        const std::size_t at = code_.size();
        const auto delta = static_cast<u16>(static_cast<int>(target) - static_cast<int>(at));
        word(delta);
    }

    // STOP #$2700 で止める。割り込みを全て禁止するので、二度と動かない。
    // ホスト側はこれを見て「走り切った」と判断する。
    void stop()
    {
        word(0x4E72);
        word(0x2700);
    }

    // STOP #$2000 で止める。割り込みマスクが 0 なので、SCC (レベル 5) の
    // 受信割り込みで起きる。スーパーバイザビットは立てたままにする
    // (STOP は S=0 の値を書くと特権違反になる)。
    void stopEnabled()
    {
        word(0x4E72);
        word(0x2000);
    }

    [[nodiscard]] const std::vector<u8>& code() const
    {
        return code_;
    }

    [[nodiscard]] u32 origin() const
    {
        return origin_;
    }

private:
    u32 origin_;
    std::vector<u8> code_;
};

// G-VRAM 16 色モードのドットアドレス。1 ドット = 1 ワード。
u32 gvramAddr(u32 x, u32 y)
{
    return kGvramBase + y * kGvramBytesPerLine + x * 2u;
}

// 矩形を塗る。1 ライン分をループで回し、ライン送りは外側で展開する。
//
// Why not 全ドットを展開しないか: 背景 512x512 だけで 26 万ドットになり、
// MOVE.W #imm,(xxx).L は 1 ドット 8 バイトなので 2MB のメインメモリに
// 収まらない。ライン内をループにすれば命令数は行数に比例する程度で済む。
void emitFillRect(Asm& a, u32 x, u32 y, u32 w, u32 h, u16 color)
{
    if (w == 0 || h == 0)
    {
        return;
    }

    // D0 = 色、D1 = 残りドット数、A0 = 書き込み先。
    a.moveWordImmToData(color, 0);
    for (u32 line = 0; line < h; ++line)
    {
        a.moveaLongImm(gvramAddr(x, y + line), 0);
        a.moveWordImmToData(static_cast<u16>(w), 1);
        const std::size_t loop = a.here();
        a.moveWordDataToPostInc(0, 0);  // MOVE.W D0,(A0)+
        a.subqWord(1, 1);               // SUBQ.W #1,D1
        a.branchNeBack(loop);
    }
}

// 立体的な枠を描く。左上を明るく、右下を暗くする定番の見せ方。
// これがあると「窓が板として浮いている」ことが絵から分かる。
void emitBevelBox(Asm& a, const WindowRect& r, u16 face, bool raised)
{
    const u16 topLeft = raised ? kColorHighlight : kColorShadow;
    const u16 bottomRight = raised ? kColorShadow : kColorHighlight;

    emitFillRect(a, r.x, r.y, r.w, r.h, face);
    emitFillRect(a, r.x, r.y, r.w, 1, topLeft);                // 上辺
    emitFillRect(a, r.x, r.y, 1, r.h, topLeft);                // 左辺
    emitFillRect(a, r.x, r.y + r.h - 1, r.w, 1, bottomRight);  // 下辺
    emitFillRect(a, r.x + r.w - 1, r.y, 1, r.h, bottomRight);  // 右辺
}

// 窓を 1 枚描く。タイトルバー + タイトル文字の代わりの横線 + 閉じるボタン +
// 本文領域。SX-Window の窓の構成要素をなぞる。
void emitWindow(Asm& a, const WindowRect& r)
{
    // 影を先に落とす。窓の右下に少しずらして暗い矩形を置く。
    emitFillRect(a, r.x + 4, r.y + 4, r.w, r.h, kColorShadow);

    // 窓の枠と地。
    emitFillRect(a, r.x, r.y, r.w, r.h, kColorFrame);
    emitBevelBox(a, {r.x + 1, r.y + 1, r.w - 2, r.h - 2}, kColorWindow, true);

    // タイトルバー。
    constexpr u32 kTitleHeight = 14;
    emitFillRect(a, r.x + 3, r.y + 3, r.w - 6, kTitleHeight, kColorTitleBar);

    // タイトル文字の代わりの横線を 3 本引く。CGROM を使わずに
    // 「ここに文字がある」ことを示す。
    //
    // Why not 実フォントを敷かないか: グラフィック面へ文字を出すには
    // CGROM か自前のビットマップが要り、デモの主眼 (窓の重なりと合成) から
    // 外れる。文字は下のテキスト面 (メニューバー) で本物を出している。
    for (u32 i = 0; i < 3; ++i)
    {
        emitFillRect(a, r.x + 24 + i * 22, r.y + 8, 16, 2, kColorHighlight);
    }

    // 閉じるボタン (タイトルバー左端の小さな箱)。
    emitBevelBox(a, {r.x + 6, r.y + 6, 8, 8}, kColorWindow, true);

    // 本文領域を一段凹ませる。
    emitBevelBox(a, {r.x + 6, r.y + 3 + kTitleHeight + 3, r.w - 12, r.h - kTitleHeight - 15},
                 kColorWindow, false);
}

// 矢印型のマウスカーソルを (x, y) に描く。
//
// カーソルの座標はゲストが D4/D5 に持っている値なので、本来は
// 実行時アドレス計算で描くべきところ。だが 16 色モードのアドレスは
// y*1024 + x*2 で、68000 で乗算を挟むと命令列が読みにくくなる。
// ここではホスト側が最終座標を先に計算し、その定数で描く命令を吐く。
// ゲストが SCC から読んだ移動量は下の emitReadMouse が D4/D5 へ足しており、
// 「絵に出るカーソル位置」と「ゲストが計算した位置」が一致することは
// ホスト側で突き合わせて確かめる (Result::cursorX/Y)。
void emitCursor(Asm& a, u32 x, u32 y)
{
    // 矢印の各行の幅。上ほど細く、下で尾を引く形。
    static constexpr u32 kRowWidth[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 4, 3, 2};
    constexpr u32 kRows = sizeof(kRowWidth) / sizeof(kRowWidth[0]);

    for (u32 row = 0; row < kRows; ++row)
    {
        // 縁取りを 1 ドット付けて、窓の上でも輪郭が分かるようにする。
        emitFillRect(a, x, y + row, kRowWidth[row] + 1, 1, kColorFrame);
        emitFillRect(a, x, y + row, kRowWidth[row], 1, kColorCursor);
    }
}

// テキスト VRAM に 1 ドット打つ。プレーン 0 だけを使う (パレット 1 = 白)。
//
// テキスト面は 4 プレーンのビットマップで、1 ライン 128 バイト。
// x ビット目は (x>>3) バイト目の (7 - (x&7)) ビット。
void emitTextDot(Asm& a, u32 x, u32 y)
{
    const u32 addr = kTvramBase + y * kTvramBytesPerLine + (x >> 3);
    const auto mask = static_cast<u8>(1u << (7u - (x & 7u)));
    a.oriByteImmToAbs(mask, addr);
}

// テキスト面に横帯 (メニューバー) を敷く。
void emitTextBar(Asm& a, u32 x, u32 y, u32 w, u32 h)
{
    for (u32 row = 0; row < h; ++row)
    {
        for (u32 col = 0; col < w; ++col)
        {
            emitTextDot(a, x + col, y + row);
        }
    }
}

// マウスを有効化し、受信割り込みまで通す。
//
// 実機の IOCS がやることを縮めた形。Scc::isMouseEnabled は WR3 の受信有効と
// WR5 の RTS の両方を見るので、片方だけでは moveMouse が断られる。
// さらに割り込みで起こしてもらうには WR1 の受信割り込みモードと
// WR9 のマスタ割り込み許可、WR2 のベクタベースが要る。
void emitEnableMouse(Asm& a)
{
    // WR2 = ベクタベース $50。IPL-ROM $FF0E2C が書く値。
    // チャネル B の受信は +4 されて $54 になる (Scc::acknowledgeInterrupt)。
    a.moveWordImmToAbs(0x0002, kSccBase);
    a.moveWordImmToAbs(0x0050, kSccBase);

    // WR1 = $10 (全文字で受信割り込み)。IPL-ROM $FF0E6C の初期化表と同じ。
    a.moveWordImmToAbs(0x0001, kSccBase);
    a.moveWordImmToAbs(0x0010, kSccBase);

    // WR9 = $08 (MIE: マスタ割り込み許可)。チップ共通のレジスタ。
    a.moveWordImmToAbs(0x0009, kSccBase);
    a.moveWordImmToAbs(0x0008, kSccBase);

    // WR3 を選んで受信有効 ($C1 = Rx enable / 8bit)。
    a.moveWordImmToAbs(0x0003, kSccBase);
    a.moveWordImmToAbs(0x00C1, kSccBase);

    // WR5 を選んで RTS on ($62)。IPL-ROM $FF147E がこの並び。
    a.moveWordImmToAbs(0x0005, kSccBase);
    a.moveWordImmToAbs(0x0062, kSccBase);
}

// SCC のチャネル B データポートから 3 バイトのレポートを読み、
// 移動量を D4 (X) / D5 (Y) へ足す。
//
// 実機の IOCS は割り込みで 1 バイトずつ引き取るが、ここでは
// 「レポートが積まれている」ことが分かっている状態で読むので、
// ポーリングで 3 バイトまとめて取る。読む順は buttons / dx / dy で、
// Scc::moveMouse が積む順と同じ。
void emitReadMouse(Asm& a)
{
    constexpr u32 kSccDataB = kSccBase + 3;  // $E98003 = ch B データ

    a.moveByteAbsToData(kSccDataB, 3);  // D3 = ボタン (今は使わない)

    a.moveByteAbsToData(kSccDataB, 1);  // D1 = dx
    a.extWord(1);                       // 符号付き 8bit → 16bit
    a.addWord(1, 4);                    // D4 += D1

    a.moveByteAbsToData(kSccDataB, 2);  // D2 = dy
    a.extWord(2);
    a.addWord(2, 5);  // D5 += D2
}

}  // namespace

Result run(u16* pixels)
{
    Result result;
    if (pixels == nullptr)
    {
        result.failure = "出力バッファが null";
        return result;
    }

    constexpr u32 kOrigin = 0x010000u;
    Asm a(kOrigin);

    // --- 画面モードとパレット -------------------------------------------
    //
    // 16 色 512x512。
    //
    // Why not SX-Window 本来の 256 色 768x512 にしないか: 256 色モードは
    // 1 ドット 1 バイトで、G-VRAM の折り込み方が 16 色と変わる。
    // デモの主眼は「窓が正しい前後関係で合成されるか」で、そこは色数に
    // 依らない。16 色の方が MOVE.W 1 回 = 1 ドットで素直に書け、
    // 生成する命令列も短い。256 色経路は test_gvram_bus.cpp が別に見ている。
    a.moveWordImmToAbs(0x0000, 0xE82400);

    // グラフィックパレット。0 は透明なので触らない。
    struct PaletteEntry
    {
        u16 index;
        u16 grb;  // X68000 の GRB555 + I
    };
    // G/R/B 各 5bit。値は (G<<11)|(R<<6)|(B<<1)。
    static constexpr PaletteEntry kPalette[] = {
        {kColorDesktop, (2u << 11) | (2u << 6) | (12u << 1)},      // 濃い青
        {kColorWindow, (20u << 11) | (20u << 6) | (20u << 1)},     // 灰
        {kColorTitleBar, (8u << 11) | (6u << 6) | (26u << 1)},     // 青
        {kColorFrame, (1u << 11) | (1u << 6) | (1u << 1)},         // ほぼ黒
        {kColorHighlight, (31u << 11) | (31u << 6) | (31u << 1)},  // 白
        {kColorShadow, (9u << 11) | (9u << 6) | (9u << 1)},        // 暗い灰
        {kColorCursor, (31u << 11) | (31u << 6) | (31u << 1)},     // 白
    };
    for (const PaletteEntry& e : kPalette)
    {
        a.moveWordImmToAbs(e.grb, 0xE82000 + e.index * 2u);
    }

    // テキストパレット 1 = 白。メニューバーの色。
    a.moveWordImmToAbs(0xFFFF, 0xE82202);

    // プライオリティ: TX=0 (手前) / GR=1 (奥) / GP1=1 / GP0=0。
    // $E82500 の組み立て方は $FF642E の $06E4 と同じ (SP/TX/GR と GP3..GP0)。
    //
    // Why not IPL-ROM の既定 (TX が奥) のままにしないか: この値だと
    // グラフィックが手前になり、最後に塗る不透明なデスクトップ背景が
    // テキスト面を丸ごと覆う。テキストのメニューバーが 1 ドットも見えず、
    // 「テキスト面が合成されているか」を絵から確かめられなくなる。
    // SX-Window もメニューやテキストはテキスト面に置いてグラフィックの
    // 手前に出すので、GUI としてはこちらが自然。
    a.moveWordImmToAbs(0x0104, 0xE82500);

    // 表示 ON: テキスト + グラフィックページ 0/1。
    a.moveWordImmToAbs(0x0033, 0xE82600);

    // --- マウス ----------------------------------------------------------
    //
    // 描画より先に有効化する。ホスト側は「プログラムが走り切ってから」
    // moveMouse を呼ぶのではなく、途中で止めて呼ぶ必要があるが、
    // ここでは有効化 → ホストが割り込み待ちで止まる、という形にせず、
    // 有効化 → 描画 → STOP → ホストが moveMouse → 再開 → 読み取り、
    // の 2 段構えにする (下の kResumePc)。
    a.moveaLongImm(0, 1);  // A1 は使わないが初期化しておく
    emitEnableMouse(a);

    // カーソル座標の初期値。D4 = X、D5 = Y。
    a.moveLongImmToData(kCursorStartX, 4);
    a.moveLongImmToData(kCursorStartY, 5);

    // --- 背景と窓 --------------------------------------------------------
    emitFillRect(a, 0, 0, kScreenWidth, kScreenHeight, kColorDesktop);

    // メニューバー (グラフィック面側の帯)。
    emitBevelBox(a, {0, 0, kScreenWidth, 18}, kColorWindow, true);

    // 奥の窓 → 手前の窓の順に描く。後から描いた方が上に乗る。
    emitWindow(a, kBackWindow);
    emitWindow(a, kFrontWindow);

    // ここで割り込み待ちに入る。STOP #$2000 は割り込みを許可した状態で
    // 止まるので、ホストが moveMouse でレポートを積むと SCC が
    // 受信割り込みを上げ、下のハンドラが呼ばれて動き出す。
    //
    // Why not STOP #$2700 で止めて PC を書き換えて再開しないか:
    // 68000 コアはプリフェッチキューを持つので、st_.pc だけ変えても
    // キューに残った古い命令語が実行され、書き換えた先へ移らない
    // (refillPrefetch は private で外から呼べない)。それ以前に、
    // PC を外から差し替えるのは実機に無い操作で、確かめたい経路
    // 「マウスのレポートが割り込みとして CPU に届く」を迂回してしまう。
    // 割り込みで起こす形なら SX-Window が実際に通る経路と同じになる。
    a.stopEnabled();

    // --- 割り込みハンドラ: マウスを読んでカーソルを描く -----------------
    const std::size_t handlerOffset = a.here();
    emitReadMouse(a);

    // ゲストが計算する最終座標。ホストも同じ式で先に求めておき、
    // カーソルを描く命令はその定数で吐く (emitCursor のコメント参照)。
    const u32 finalX = kCursorStartX + static_cast<u32>(kMouseDx);
    const u32 finalY = kCursorStartY + static_cast<u32>(kMouseDy);
    emitCursor(a, finalX, finalY);

    // テキスト面のメニューバー。グラフィックの窓より奥に出るはず
    // (TX=1 / GR=0)。テキストが合成されている印。
    emitTextBar(a, 8, 4, 160, 10);

    a.stop();

    // --- 実行 ------------------------------------------------------------
    std::vector<u8> mainRam(kMainRamSize, 0);
    std::vector<u8> textVram(kTvramSize, 0);
    std::vector<u8> graphicVram(kTvramSize, 0);

    const std::vector<u8>& code = a.code();
    if (kOrigin + code.size() >= kMainRamSize)
    {
        result.failure = "生成したコードがメインメモリに収まらない";
        return result;
    }
    std::memcpy(mainRam.data() + kOrigin, code.data(), code.size());

    // ベクタを置く小さなヘルパ。ベクタ番号 n は $000000 + n*4。
    const auto setVector = [&mainRam](u32 vectorNumber, u32 value)
    {
        const std::size_t at = static_cast<std::size_t>(vectorNumber) * 4u;
        for (int b = 0; b < 4; ++b)
        {
            mainRam[at + static_cast<std::size_t>(b)] = static_cast<u8>(value >> ((3 - b) * 8));
        }
    };

    // リセットベクタ。ベクタ 0 = SSP、ベクタ 1 (= $000004) = 初期 PC。
    setVector(0, kResetSsp);
    setVector(1, kOrigin);

    // SCC チャネル B 受信のベクタ。WR2 に書いた $50 に、Z8530 が
    // 受信要因として +4 した $54 が使われる (Scc::acknowledgeInterrupt の
    // コメントに IPL-ROM $FF0E2C / $FF0DA2 からの裏取りがある)。
    // ここを外すと STOP から起きても未初期化のベクタへ飛ぶ。
    constexpr u32 kSccRxBVector = 0x54;
    setVector(kSccRxBVector, kOrigin + static_cast<u32>(handlerOffset));

    Machine machine;
    MemoryMap memory;
    memory.mainRam = mainRam.data();
    memory.textVram = textVram.data();
    memory.graphicVram = graphicVram.data();
    machine.setMemory(memory);
    machine.bus().setRomMappedAtZero(false);
    machine.reset();

    // 1 段目: 最初の STOP まで。
    constexpr u32 kMaxInstructions = 20000000;
    u32 executed = 0;
    while (executed < kMaxInstructions && !machine.cpu().state().stopped && !machine.isHalted())
    {
        machine.step();
        ++executed;
    }

    if (machine.isHalted())
    {
        result.haltedOpcode = machine.haltedOpcode();
        result.failure = "未実装命令で停止した";
        result.instructions = executed;
        return result;
    }
    if (!machine.cpu().state().stopped)
    {
        result.failure = "描画後の STOP に到達しなかった";
        result.instructions = executed;
        return result;
    }

    // --- ホストがマウスを動かす -----------------------------------------
    //
    // ゲストは STOP #$2000 で割り込み待ちに入っている。ここでレポートを
    // 積むと SCC が受信割り込みを上げ、ベクタ $54 のハンドラが走る。
    result.mouseEnabled = machine.scc().isMouseEnabled();
    result.mouseAccepted = machine.moveMouse(kMouseDx, kMouseDy, true, false);

    // 2 段目: 割り込みで起きたハンドラを最後の STOP まで走らせる。
    while (executed < kMaxInstructions && !machine.isHalted())
    {
        machine.step();
        ++executed;

        // ハンドラの末尾の STOP #$2700 まで来たら終わり。
        // 1 つ目の STOP と区別するため、割り込みマスクが 7 (= 全禁止) に
        // なっていることで見分ける。
        const M68kState& s = machine.cpu().state();
        const bool isFinalStop = s.stopped && s.interruptMask() == 7;
        if (isFinalStop)
        {
            break;
        }
    }

    if (machine.isHalted())
    {
        result.haltedOpcode = machine.haltedOpcode();
        result.failure = "割り込みハンドラで未実装命令に当たった";
        result.instructions = executed;
        return result;
    }
    if (!machine.cpu().state().stopped)
    {
        result.failure = "割り込みハンドラの STOP に到達しなかった";
        result.instructions = executed;
        return result;
    }

    // ゲストが計算したカーソル座標。ホストが描画に使った定数と
    // 一致するはず。ずれていれば SCC の読み出しか EXT/ADD が壊れている。
    result.cursorX = machine.cpu().state().d[4] & 0xFFFFu;
    result.cursorY = machine.cpu().state().d[5] & 0xFFFFu;

    result.instructions = executed;

    // --- 合成 ------------------------------------------------------------
    GraphicRaster::composite(graphicVram.data(), textVram.data(), machine.video(), 0, 0,
                             kScreenWidth, kScreenHeight, pixels, kScreenWidth);

    result.ok = true;
    return result;
}

}  // namespace x68k::guidemo
