// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// I/O 割り込みコントローラ ($E9C000)。
//
// X68000 は MFP にも SCC にも繋がっていない 4 つの周辺 (FDC・FDD・SASI HDD・
// プリンタ) の割り込みを 1 本にまとめる小さなコントローラを持つ。IRQ1 に
// 繋がっていて、ベクタ番号は自分で返す (オートベクタではない)。
//
// --- レジスタ配置の出典 -----------------------------------------------------
// rom/iplrom.dat (EXPERT 用 v1.0) の逆アセンブルと、MAME の x68k ドライバの
// 実装が一致する。ROM 側の実測は以下 (バイトパターンで全件検索した)。
//
//   $FF0CC2: MOVE.B #$60,$E9C003   ← ベクタレジスタ (ROM 全体でここ 1 箇所)
//   $FF0D04: MOVE.B #$06,$E9C001   ← 割り込み許可
//
// $E9C001 は read-modify-write される (許可レジスタ)、$E9C003 は即値を 1 度
// 書くだけ (ベクタレジスタ)、という非対称がそのまま区別の根拠になる。
//
// --- $E9C001 のビット割り当て -----------------------------------------------
//   bit7 FDC 割り込み発生      bit3 HDD 割り込み許可
//   bit6 FDD 割り込み発生      bit2 FDC 割り込み許可
//   bit5 プリンタ BUSY         bit1 FDD 割り込み許可
//   bit4 HDD 割り込み発生      bit0 プリンタ割り込み許可
//
// 上位 4bit が状態 (読み出し専用)、下位 4bit が許可 (読み書き)。
//
// ROM がこの並びを裏付ける箇所:
//   $FF0D04 の初期値 $06 = bit2|bit1 = 「FDC と FDD の割り込みを許可」。
//     起動時に FDC を使うのだから、FDC の許可が最初から立っているのが正しい。
//   $FF9F14 (IOCS のプリンタ割り込み設定) は、ベクタ $63 の位置
//     ($00018C = $63*4) にハンドラを入れてから BSET #0 で bit0 を立て、
//     解除時は BCLR #0 と同時にベクタを埋め戻す。bit0 とベクタ $63 が
//     対応している = bit0 はプリンタ、と読める。
//   $FF81CC の BTST #5,$E9C001 はプリンタ出力ルーチン ($E8C003 を叩く
//     コードの中) にあり、bit5 が BUSY であることと合う。
//   $FFEA7E のプリンタ割り込みハンドラ (末尾が RTE) が BCLR #0 で
//     自分の許可を落とす。
//
// --- ベクタ番号 -------------------------------------------------------------
// $E9C003 は「bit7-2 = ベクタ上位」「bit1-0 = デバイス番号」という形で、
// 実際に配送されるベクタ番号は (ベース & $FC) | デバイス番号 になる。
//   デバイス 0 = FDC / 1 = FDD / 2 = HDD / 3 = プリンタ
// ROM はベースに $60 を書き、$180 ($60*4) から 4 本ぶんのベクタを張る
// ($FF0CAE の転送ループ)。並びは実測で
//   $60 = $FF1130 (FDC。結果表 $000C90 を埋める)
//   $61 = $FF11FE (FDD。$E94005 のドライブ制御を触る)
//   $62 = $FF124E (HDD)
//   $63 = $63FF0540 (ダミー。$FF9F14 が実行時に差し替える)
// となり、上のデバイス番号の並びと一致する。
//
// --- 割り込みレベル ---------------------------------------------------------
// レベル 1。MFP (6) と SCC (5) より低い最下位で、MAME の x68k ドライバも
// IRQ1 に繋いでいる。ROM 側の裏付けは $FF0D38 の ANDI #$F8FF,SR で、
// $FF0D04 で許可を書いた直後に IPL を 0 まで下げている。レベル 1 が通るのは
// IPL=0 のときだけなので、ここを下げる意味があるのはレベル 1 だから。
//
// Why not レベル 6 や 5 にしないか: MFP / SCC と衝突する。X68000 は 1 レベルに
// 1 デバイスを割り当てており、FDC の完了通知がキーボードやタイマより優先
// されると、転送のたびにシステムタイマを取りこぼす。

#ifndef X68K_CORE_DEV_IOSC_H
#define X68K_CORE_DEV_IOSC_H

#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class IoSc
{
public:
    // デバイス番号。ベクタ番号の下位 2bit にそのまま載る。
    static constexpr u8 kDeviceFdc = 0;
    static constexpr u8 kDeviceFdd = 1;
    static constexpr u8 kDeviceHdd = 2;
    static constexpr u8 kDevicePrinter = 3;

    // $E9C001 の許可ビット (下位 4bit)。
    static constexpr u8 kEnablePrinter = 0x01;
    static constexpr u8 kEnableFdd = 0x02;
    static constexpr u8 kEnableFdc = 0x04;
    static constexpr u8 kEnableHdd = 0x08;

    // $E9C001 の状態ビット (上位 4bit)。読み出し専用。
    static constexpr u8 kStatusHdd = 0x10;
    static constexpr u8 kStatusPrinterBusy = 0x20;
    static constexpr u8 kStatusFdd = 0x40;
    static constexpr u8 kStatusFdc = 0x80;

    // 割り込みレベル。上のコメントの根拠により 1。
    static constexpr u32 kInterruptLevel = 1;

    void reset();

    // $E9C001 (許可 + 状態) / $E9C003 (ベクタ)。
    // 奇数アドレスだけが有効。8bit デバイスがデータバスの下位に繋がっている。
    [[nodiscard]] u8 read(u32 offset) const;
    void write(u32 offset, u8 value);

    // デバイス側から割り込み線を上げ下げする。
    //
    // Why not 「立てる」だけの口にするか: FDC の割り込みは
    // SENSE INTERRUPT STATUS で落ちるまで上がりっぱなしのレベル割り込みで、
    // エッジではない。落とす口が無いと一度上がったら二度と下がらず、
    // ハンドラが無限に呼ばれる。
    void setSource(u8 device, bool asserted);

    // FDC の線だけを直に張り替える。
    //
    // Why not setSource(kDeviceFdc, ...) で済ませないか: Machine は
    // **毎命令** FDC の線を取り直す (DMA 完了など、バスアクセス以外の契機でも
    // 状態が変わるため)。setSource は別 TU の関数呼び出しで、中で
    // statusMaskOf() の switch を引く。ここが 1 命令ごとに乗ると実効クロックに
    // 効く — 実測で保留判定をインライン化しただけでも 18% → 9% まで戻った。
    //
    // FDC は 1 本しか無く、ビットも定数なので、番号からマスクを引く必要が無い。
    // 汎用の口 (setSource) は他のデバイスのために残す。
    void setFdcLine(bool asserted)
    {
        if (asserted)
        {
            status_ = static_cast<u8>(status_ | kStatusFdc);
            return;
        }
        status_ = static_cast<u8>(status_ & static_cast<u8>(~kStatusFdc));
    }

    // 許可されていて、かつ上がっているデバイスがあるか。
    //
    // ここはインライン。**毎命令通る経路**なので、関数呼び出しと
    // デバイス 4 個のループがそのまま実効クロックに効く。
    //
    // Why not pendingDevice() をそのまま呼ばないか: あちらは
    // 「どのデバイスか」を返すために 4 回回り、1 回ごとに
    // enableMaskOf/statusMaskOf を呼ぶ。実際にはどちらのビットも
    // 立っていない状態が圧倒的に多く、その判定は
    // 「許可と状態の積が 0 か」を 1 回見れば済む。
    //
    // ループを素直に呼ぶ形にしていたところ、ホストのスループットが
    // 1.26s → 1.49s へ落ちた (400M サイクルの起動を交互に 6 回ずつ、実測)。
    // **18% の低下**で、issue #4 の目標に真っ向から反する大きさだった。
    //
    // Why not 状態を 4bit 右へ寄せて許可と重ねないか: 許可と状態は
    // **対応するビット位置が揃っていない**。
    //   FDC : 許可 $04 / 状態 $80
    //   FDD : 許可 $02 / 状態 $40
    //   HDD : 許可 $08 / 状態 $10
    // 一律のシフトでは対応しない (一度そう書いて、FDC の許可を
    // 別のデバイスのビットで判定するところだった)。組ごとに書く。
    //
    // プリンタは status 側が BUSY 信号で割り込み要因ではないので、
    // statusMaskOf が 0 を返す = ここでも対象外。
    [[nodiscard]] bool hasPendingInterrupt() const
    {
        const bool fdc = (enable_ & kEnableFdc) != 0 && (status_ & kStatusFdc) != 0;
        const bool fdd = (enable_ & kEnableFdd) != 0 && (status_ & kStatusFdd) != 0;
        const bool hdd = (enable_ & kEnableHdd) != 0 && (status_ & kStatusHdd) != 0;
        return fdc || fdd || hdd;
    }

    // CPU が受理した。届けるベクタ番号を返す。0 なら届けるものが無い。
    //
    // Why not ここで保留を落とさないか: レベル割り込みなので、要因が消える
    // まで下がらないのが実機の振る舞い。落とすのは要因側 (FDC なら
    // SENSE INTERRUPT STATUS) の仕事。ここで落とすと、ハンドラが要因を
    // 読む前に線が下がって取りこぼす。
    [[nodiscard]] u8 acknowledgeInterrupt() const;

    [[nodiscard]] u8 enableRegister() const
    {
        return enable_;
    }
    [[nodiscard]] u8 vectorRegister() const
    {
        return vector_;
    }

private:
    // 一番若いデバイス番号が最優先。FDC (0) が最優先になる。
    [[nodiscard]] int pendingDevice() const;

    // 各デバイスの許可ビット。添字はデバイス番号。
    [[nodiscard]] static u8 enableMaskOf(u8 device);
    // 各デバイスの状態ビット。添字はデバイス番号。
    [[nodiscard]] static u8 statusMaskOf(u8 device);

    // $E9C001 の下位 4bit。起動直後は 0 (ROM が $06 を書くまで通さない)。
    u8 enable_ = 0;
    // $E9C001 の上位 4bit。デバイスから setSource で立つ。
    u8 status_ = 0;
    // $E9C003。ROM が $60 を書く。
    u8 vector_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_IOSC_H
