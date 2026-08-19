// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "bus.h"

#include "cpu/m68k.h"

namespace x68k
{
namespace
{

// 68000 のアドレスバスは 24bit。
constexpr u32 kAddrMask = 0x00FFFFFFu;

// I/O 空間の範囲。$E80000-$EBFFFF (CRTC からスプライトまで)。
constexpr u32 kIoBase = 0xE80000u;
constexpr u32 kIoEnd = 0xEC0000u;

// リセット直後に $000000 へ写像される ROM の位置と大きさ。
//
// 写像元は $FF0000 側 (IPL-ROM 128KB の後半 64KB)。リセットベクタと
// 起動コードはここに置かれており、実行は $FF0010 から始まる。
constexpr u32 kRomAtZeroOffset = 0xFF0000u - kIplromBase;  // ROM 内オフセット
constexpr u32 kRomAtZeroSize = kIplromSize - kRomAtZeroOffset;

// G-VRAM の窓 1 つぶんの大きさ。実 VRAM 全体 (512KB) と同じ。
//
// 窓は $C00000 / $C80000 / $D00000 / $D80000 の 4 つで、$E00000 まで届く。
// 「どの窓か」がページ番号になる。
//
// 根拠: IPL-ROM のグラフィックページ設定 ($FFAEE8 と $FFB268 に同じ計算がある)。
//   MOVE.W D1,D0 / AND.W #$0003,D0 / ASL.W #3,D0 / ADD.W #$00C0,D0
//   / SWAP D0 / CLR.W D0 / MOVE.L D0,$095C
// ページ番号を 8 倍して $C0 を足し上位ワードへ送るので、
// ページ N の先頭は $C00000 + N * $80000。逆変換も $FFB282 にある
//   (MOVE.L $095C,D0 / SWAP D0 / SUB.W #$00C0,D0 / LSR.W #3,D0 / AND.L #3,D0)。
//
// 窓の広さが実 VRAM 全体と同じであることは $FFAAB4 の全消去が示している。
//   LEA $C00000,A0 / LEA $C80000,A1 / BSR $FFABC0
//   $FFABC0: CLR.L (A0)+ / CMPA.L A1,A0 / BNE.S -6
// 512KB ぶんを消すだけで 4 ページすべてが消える。ページごとに VRAM が
// 分かれているなら 1/4 しか消えないので、この 1 ループでは足りない。
constexpr u32 kGvramWindowSize = 0x80000u;
constexpr u32 kGvramWindowMask = kGvramWindowSize - 1u;

}  // namespace

void SystemBus::publishFastRam()
{
    if (fastPathCpu_ == nullptr)
    {
        return;
    }

    // ウォッチが張られている間は直接経路を止める。
    // 素通りされると監視対象への書き込みが notifyWatch を通らない。
    const bool watching = watchCallback_ != nullptr;
    u8* const base = watching ? nullptr : mem_.mainRam;
    fastPathCpu_->setFastRam(base, kMainRamSize);

    // 読み出しを直接経路に流してよいのは ROM 写像が外れてからだけ。
    // 写像中の $000000-$00FFFF は RAM ではなく IPL-ROM の $FF0000 側が見える。
    fastPathCpu_->setFastRamReadable(!romAtZero_);

    // IPL-ROM ($FE0000-) は最頻のアクセス先 (実測で全体の 79%)。
    // 書けないので読み出しだけを通す。ROM 写像の有無に関係なく
    // $FE0000 側の窓は常に正しい (写像が変えるのは $000000 側の見え方だけ)。
    //
    // ウォッチは書き込みだけを見張る仕組みなので、読み出し専用のこの窓には
    // 関係しない。止める必要が無い。
    fastPathCpu_->setFastRom(mem_.iplRom, kIplromBase, kIplromSize);

    // **ROM は書き換わらないので、世代を固定値で返してよい。**
    //
    // 世代配列はメインメモリぶんしか無いため、$FE0000 は範囲外として
    // kAlwaysStale になり、翻訳器の I9 が「常に古い」と見て拒否していた。
    // 実測で**実行の 23% が ROM から**なので、まるごと捨てていた。
    //
    // 窓と同じ場所で教えるので、片方だけ更新して食い違うことがない。
    fastPathCpu_->codeGenMap().setImmutableRange(kIplromBase, kIplromSize);
}

SystemBus::GvramLane SystemBus::gvramLaneOf(u32 addr) const
{
    const u32 offsetInSpace = addr - kGvramBase;
    const u32 window = offsetInSpace / kGvramWindowSize;  // 0-3
    const u32 offsetInWindow = offsetInSpace & kGvramWindowMask;

    // 未設定なら 16 色。VideoController::reset() が $E82400 を 0 にするので、
    // 実機のリセット直後と同じ扱いになる。
    const VideoController::GraphicColorMode mode =
        video_ != nullptr ? video_->graphicColorMode()
                          : VideoController::GraphicColorMode::k16Color;

    // ワード内でのバイト位置。窓のオフセットをそのままワード境界へ丸める。
    //
    // Why not 窓ごとに実 VRAM のオフセットをずらさないか: 4 つの窓は
    // 「同じワードを別の角度から見る」ものなので、実 VRAM 上の位置は
    // どの窓から触っても同じでなければならない。ずらすとページ 1 に書いた絵が
    // ページ 0 と別の座標に出る。
    const u32 wordBase = offsetInWindow & ~1u;

    switch (mode)
    {
        case VideoController::GraphicColorMode::k16Color:
            // 4 ページぶんの 4bit が 1 ワードに同居する。ページ 0 が最下位ニブル。
            // 窓 0-3 がそのままページ 0-3。
            return {wordBase, window * 4u, 0x000Fu};

        case VideoController::GraphicColorMode::k256Color:
            // 2 ページぶんの 8bit が 1 ワードに同居する。ページ 0 が下位バイト。
            //
            // 窓は 4 つあるが使うページは 2 つなので、$D00000 以降は
            // $C00000 側の繰り返しになる (窓番号の bit0 だけが効く)。
            return {wordBase, (window & 1u) * 8u, 0x00FFu};

        case VideoController::GraphicColorMode::kReserved:
        case VideoController::GraphicColorMode::k65536Color:
        default:
            // 1 ワードがそのまま 1 ドットの色。ページの概念が無く、
            // どの窓から触っても同じワード全体に効く。
            return {wordBase, 0u, 0xFFFFu};
    }
}

u16 SystemBus::readGvramDot(u32 addr) const
{
    const GvramLane lane = gvramLaneOf(addr);
    const u8* p = mem_.graphicVram + lane.byteOffset;
    // 実 VRAM はビッグエンディアンのワード列。ホストのエンディアンに依存しない
    // よう明示的に組む (video/graphic_raster.cpp の readWord と同じ理由)。
    const u16 word = static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
    return static_cast<u16>((word >> lane.shift) & lane.mask);
}

void SystemBus::writeGvramDot(u32 addr, u16 value)
{
    const GvramLane lane = gvramLaneOf(addr);
    u8* p = mem_.graphicVram + lane.byteOffset;
    const u16 word = static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);

    // 読んで、自分のページのビットだけ差し替えて、書き戻す。
    //
    // Why not ワードをそのまま上書きしないか: 16 色モードでは 1 ワードに
    // 4 ページぶんのニブルが同居する。$C80000 (ページ 1) への書き込みで
    // ワード全体を潰すと、同じ座標のページ 0/2/3 のドットが道連れになる。
    // IPL-ROM は 4bit のドットを MOVE.W で書く ($FFB0AA 付近: LSR.B #4 で
    // 取り出したニブルを AND.W #$000F してから MOVE.W (A0)+ する) ので、
    // 上書きにすると 16 色の描画が毎回 3 ページを消して回ることになる。
    const u16 field = static_cast<u16>(lane.mask << lane.shift);
    const u16 next = static_cast<u16>((word & ~field) | ((value & lane.mask) << lane.shift));

    p[0] = static_cast<u8>(next >> 8);
    p[1] = static_cast<u8>(next & 0xFFu);
}

void SystemBus::markTextDirty(u32 offsetInPlane)
{
    // テキスト VRAM は 1 ライン 128 バイト。オフセットから行番号を求め、
    // タイル行 (16 ライン単位) の印を立てる。
    const u32 line = offsetInPlane / kTvramBytesPerLine;
    const u32 tileRow = line / kDirtyTileHeight;
    if (tileRow < kDirtyTileRows)
    {
        textDirty_[tileRow] = true;
    }
}

u8 SystemBus::read8(u32 addr)
{
    const u32 a = addr & kAddrMask;
    faulted_ = false;

    // --- メインメモリ (最頻) ---
    if (a < kMainRamSize)
    {
        // リセット直後は IPL-ROM が $000000 に写像されている。
        // IPL-ROM がエリアセットに書き込むまでこの状態が続く。
        //
        // 写像されるのは ROM の先頭 ($FE0000) ではなく $FF0000 の側。
        // リセットベクタ (SSP と PC) はそこに置かれており、実機の
        // 起動は PC=$FF0010 から始まる。ここを取り違えると
        // ベクタが読めず即座に暴走する。
        if (romAtZero_ && a < kRomAtZeroSize && mem_.iplRom != nullptr)
        {
            return mem_.iplRom[kRomAtZeroOffset + a];
        }
        return mem_.mainRam != nullptr ? mem_.mainRam[a] : 0u;
    }

    // --- IPL-ROM ---
    if (a >= kIplromBase)
    {
        return mem_.iplRom != nullptr ? mem_.iplRom[a - kIplromBase] : 0u;
    }

    // --- CGROM ---
    if (a >= kCgromBase && a < kCgromEnd)
    {
        return mem_.cgRom != nullptr ? mem_.cgRom[a - kCgromBase] : 0u;
    }

    // --- SRAM ---
    if (a >= kSramBase && a < kSramEnd)
    {
        return sram_.read8(a - kSramBase);
    }

    // --- テキスト VRAM ---
    if (a >= kTvramBase && a < kTvramEnd)
    {
        return mem_.textVram != nullptr ? mem_.textVram[a - kTvramBase] : 0u;
    }

    // --- I/O ---
    if (a >= kIoBase && a < kIoEnd)
    {
        return io_.ioRead8(a);
    }

    // --- グラフィック VRAM ---
    if (a >= kGvramBase && a < kGvramEnd)
    {
        if (mem_.graphicVram == nullptr)
        {
            return 0u;
        }
        // 窓が選んだページのぶんだけを取り出し、残りは 0 で埋めたワードを作る。
        // バイトアクセスはそのワードの上位/下位を切り出したもの。
        //
        // Why not 共有ワードのバイトをそのまま返さないか: 16 色モードの
        // $C80000 を読むとページ 1 とページ 0 のニブルが混ざった値になる。
        // 実機はページの外を読ませないので、混ざった値を返すと
        // 「読んで加工して書き戻す」描画が他ページの絵を自分のページへ焼き付ける。
        return static_cast<u8>(readGvramDot(a) >> (((a & 1u) == 0u) ? 8u : 0u));
    }

    // ここまでのどれにも当たらない領域。
    //
    // X68000 の IPL-ROM は「バスエラーベクタを差し替えてから読みに行き、
    // エラーが起きれば装置が無い」という方法で SCSI ROM ($FC0000) の有無を
    // 調べる ($FF0236)。0 を返してしまうと「ROM がある」ことになり、
    // その先頭を JSR で呼んで暴走する。
    //
    // I/O 空間 ($E80000-$EBFFFF) は上で処理済みなので、ここへ来るのは
    // 本当に何も繋がっていないアドレス。バスエラーにしてよい。
    faulted_ = true;
    return 0u;
}

u16 SystemBus::read16(u32 addr)
{
    const u32 a = addr & kAddrMask;
    faulted_ = false;

    // ワード単位でまとめて読める領域は 2 回の read8 を避ける。
    // 命令フェッチが必ずここを通るので効果が大きい。
    // 高速路は「2 バイトとも同じ領域に収まる」ときだけ通す。
    //
    // a だけを見て通すと、領域の最後のバイトへのワードアクセスで
    // 配列の 1 バイト外を読む。収まらないものは下の read8 を 2 回呼ぶ
    // 経路へ落とせば、そちらが境界を正しく判定する。
    const bool fitsInMainRam = a + 1 < kMainRamSize;
    if (fitsInMainRam)
    {
        if (romAtZero_ && a + 1 < kRomAtZeroSize && mem_.iplRom != nullptr)
        {
            const u8* rom = mem_.iplRom + kRomAtZeroOffset;
            return static_cast<u16>((rom[a] << 8) | rom[a + 1]);
        }
        if (mem_.mainRam != nullptr)
        {
            return static_cast<u16>((mem_.mainRam[a] << 8) | mem_.mainRam[a + 1]);
        }
        return 0u;
    }

    const bool fitsInIplrom = a >= kIplromBase && a + 1 < kIplromBase + kIplromSize;
    if (fitsInIplrom && mem_.iplRom != nullptr)
    {
        const u32 off = a - kIplromBase;
        return static_cast<u16>((mem_.iplRom[off] << 8) | mem_.iplRom[off + 1]);
    }

    if (a >= kIoBase && a < kIoEnd)
    {
        return io_.ioRead16(a);
    }

    // グラフィック VRAM はワードが最小単位なので、read8 2 回に分けない。
    //
    // Why not read8 を 2 回でよくないか: 窓のアドレスはワード境界へ丸められる
    // ので、上位バイトと下位バイトが同じワードを指す。分けて読むと同じドットを
    // 2 度切り出すことになり、65536 色モードで上位バイトが下位バイトの値に
    // 化ける。ここは 1 回で組み立てる。
    const bool fitsInGvram = a >= kGvramBase && a + 1 < kGvramEnd;
    if (fitsInGvram)
    {
        return mem_.graphicVram != nullptr ? readGvramDot(a) : 0u;
    }

    // 上のどれにも当たらない領域は read8 を 2 回に分ける。
    // read8 が faulted_ を書き換えるので、どちらかが失敗したら
    // ワード全体を失敗として扱う。
    const u8 hi = read8(a);
    const bool hiFaulted = faulted_;
    const u8 lo = read8(a + 1);
    faulted_ = faulted_ || hiFaulted;
    return static_cast<u16>((hi << 8) | lo);
}

void SystemBus::write8(u32 addr, u8 value)
{
    const u32 a = addr & kAddrMask;
    notifyWatch(a, value, 1);

    if (a < kMainRamSize)
    {
        // ROM が写像されている間の書き込みは RAM 側へ行く (ROM は書けない)。
        if (mem_.mainRam != nullptr)
        {
            // ここは CPU の遅い経路と **DMA** の両方が通る。デコード済み
            // ブロックの前提が崩れたことを世代で伝える (code_gen_map.h)。
            if (codeGen_ != nullptr)
            {
                codeGen_->touch(a);
            }
            mem_.mainRam[a] = value;
        }
        return;
    }

    if (a >= kSramBase && a < kSramEnd)
    {
        sram_.write8(a - kSramBase, value);
        return;
    }

    if (a >= kTvramBase && a < kTvramEnd)
    {
        if (mem_.textVram != nullptr)
        {
            const u32 off = a - kTvramBase;
            mem_.textVram[off] = value;
            markTextDirty(off % kTvramPlaneSize);
        }
        return;
    }

    if (a >= kIoBase && a < kIoEnd)
    {
        io_.ioWrite8(a, value);
        return;
    }

    if (a >= kGvramBase && a < kGvramEnd)
    {
        if (mem_.graphicVram != nullptr)
        {
            // バイト書き込みはワードの片側だけを差し替える。
            // 既に載っているドットを読んでから、対象のバイトを入れ替える。
            //
            // 16 色 / 256 色ではドットが下位バイトに収まるので、
            // 偶数番地 (上位バイト) への書き込みは何にも当たらない。
            const bool isHighByte = (a & 1u) == 0u;
            const u16 dot = readGvramDot(a);
            const u16 next = isHighByte
                                 ? static_cast<u16>((static_cast<u16>(value) << 8) | (dot & 0xFFu))
                                 : static_cast<u16>((dot & 0xFF00u) | value);
            writeGvramDot(a, next);
        }
        return;
    }

    // ROM 領域への書き込みは黙って捨てる。
}

void SystemBus::write16(u32 addr, u16 value)
{
    const u32 a = addr & kAddrMask;
    notifyWatch(a, value, 2);

    // read16 と同じく、2 バイトとも収まるときだけ高速路を通す。
    const bool fitsInMainRam = a + 1 < kMainRamSize;
    if (fitsInMainRam && mem_.mainRam != nullptr)
    {
        if (codeGen_ != nullptr)
        {
            codeGen_->touch(a);
        }
        mem_.mainRam[a] = static_cast<u8>(value >> 8);
        mem_.mainRam[a + 1] = static_cast<u8>(value & 0xFFu);
        return;
    }

    if (a >= kIoBase && a < kIoEnd)
    {
        io_.ioWrite16(a, value);
        return;
    }

    // グラフィック VRAM は read16 と同じ理由でワードのまま扱う。
    // 分けて書くと 2 回目の read-modify-write が 1 回目の結果を消す。
    const bool fitsInGvram = a >= kGvramBase && a + 1 < kGvramEnd;
    if (fitsInGvram)
    {
        if (mem_.graphicVram != nullptr)
        {
            writeGvramDot(a, value);
        }
        return;
    }

    write8(a, static_cast<u8>(value >> 8));
    write8(a + 1, static_cast<u8>(value & 0xFFu));
}

}  // namespace x68k
