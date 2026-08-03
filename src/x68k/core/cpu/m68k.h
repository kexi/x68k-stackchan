// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MC68000 インタプリタ。
//
// 実装方針:
//   - デコードは「命令語の上位 4bit で 1 次分岐 → グループ内で switch」の 2 段。
//     65536 エントリの関数ポインタ表 (256KB) は ESP32-S3 の内部 SRAM 512KB に対して
//     重すぎるため採らない。
//   - 未実装の命令に当たったら halted を立てて止める。IPL-ROM を走らせて
//     「落ちた命令から実装する」という開発ループを回すため。仕様書を先に読み込むより速い。
//   - サイクル数は命令ごとの固定値 + 実効アドレス計算の加算という粒度。
//     バスサイクル単位の精度は追わない (Human68k のコンソールには不要で、
//     ESP32-S3 の性能に余裕がない)。
//
// プリフェッチについて:
//   68000 は実行中の命令の先を 2 ワード読んでいる。テストベクタが初期状態・
//   最終状態としてプリフェッチキューを持つため、エミュレータ側も同じ形で
//   保持しないと突き合わせられない。ir が現在の命令語、irc が次のワード。

#ifndef X68K_CORE_CPU_M68K_H
#define X68K_CORE_CPU_M68K_H

#include "m68k_types.h"

namespace x68k
{

class M68k
{
public:
    explicit M68k(Bus& bus) : bus_(bus) {}

    // ベクタ $000 から SSP、$004 から PC を読んで初期化する。
    void reset();

    // 命令を 1 つ実行し、消費したサイクル数を返す。
    // halted または stopped の場合は何もせず 0 を返す。
    u32 step();

    // 割り込みを要求する。level は 1-7 (7 はマスク不可)。
    // 実際に受け付けられるかは SR の割り込みマスクによる。
    //
    // vectorNumber に 0 以外を渡すと、自動ベクタ (24+level) ではなく
    // その番号のベクタを使う。X68000 の MFP は自分のベクタ番号を返す
    // デバイスなので、これを使わないと未定義割り込みのハンドラへ飛んでしまう。
    void requestInterrupt(u32 level, u32 vectorNumber = 0);

    // RESET 命令が実行されたときに呼ばれる。
    //
    // 68000 の RESET は RESET 信号を外部へ出すだけで CPU 自身は何もしないが、
    // X68000 ではこれを受けてメモリコントローラが $000000 の ROM 写像を解除する。
    // IPL-ROM は起動直後にこれを実行して通常のメモリ配置へ切り替える。
    // 機種固有の反応なので、CPU からは外へ通知するだけにする。
    using ResetCallback = void (*)(void* context);
    void setResetCallback(ResetCallback callback, void* context)
    {
        resetCallback_ = callback;
        resetContext_ = context;
    }

    [[nodiscard]] M68kState& state()
    {
        return st_;
    }
    [[nodiscard]] const M68kState& state() const
    {
        return st_;
    }

    // SR を書き換える。S ビットが変わる場合は A7 を USP/SSP と入れ替える。
    // 命令実装から直接 sr を代入すると A7 の切り替えを忘れるので、必ずこれを通す。
    void setSr(u16 value);

    // テストベクタから状態を流し込むときに使う。プリフェッチも含めて外から
    // 完全に指定したいので、setSr のような副作用を挟まない。
    void loadStateForTest(const M68kState& s);

private:
    // --- プリフェッチ --------------------------------------------------------
    // 命令語を 1 ワード取り出し、キューを 1 つ進める。
    u16 fetch();
    // プリフェッチキューを PC の位置から埋め直す (分岐後など)。
    void refillPrefetch(u32 newPc);

    // --- メモリアクセス ------------------------------------------------------
    // ワード/ロングの奇数アドレスアクセスはアドレスエラーになる。
    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    // --- 例外 ----------------------------------------------------------------
    // 積む PC の基準が 2 通りある。TRAP / CHK / DIVU の 0 除算 / 割り込みは
    // 「次の命令」を積み、不当命令・A-line・F-line・特権違反は「例外を起こした
    // 命令そのもの」を積む。後者は faulting = true。
    void takeException(u32 vectorNumber, bool faulting = false);
    void takeAddressError(u32 addr, bool isRead);
    void takeBusError(u32 addr, bool isRead);
    // アドレスエラーとバスエラーは同じ 14 バイトフレームを積む。
    // 違うのはベクタ番号だけなので共通化する。
    void takeGroup0Exception(u32 vectorNumber, u32 addr, bool isRead);
    [[nodiscard]] bool requirePrivilege();

    // --- 実効アドレス --------------------------------------------------------
    // mode/reg から実効アドレスを計算する。size はディスプレースメント計算と
    // -(An)/(An)+ の増減幅に効く (バイトで A7 を触ると 2 増減する特例がある)。
    u32 effectiveAddress(u32 mode, u32 reg, u32 size);
    u32 readEa(u32 mode, u32 reg, u32 size);
    void writeEa(u32 mode, u32 reg, u32 size, u32 value);
    // 書き込み先の実効アドレスを一度だけ計算して使い回すための版。
    // ADD.b (An)+,D0 のように「読んでから同じ場所へ書く」命令で、
    // ポインタを二重に進めてしまう事故を防ぐ。
    u32 readEaForModify(u32 mode, u32 reg, u32 size, u32& addrOut);
    void writeEaToAddr(u32 mode, u32 reg, u32 size, u32 addr, u32 value);

    // --- フラグ --------------------------------------------------------------
    void setLogicFlags(u32 value, u32 size);
    [[nodiscard]] bool testCondition(u32 cond) const;

    // --- 命令グループ --------------------------------------------------------
    // 戻り値は消費サイクル数。未実装なら halted を立てて 0 を返す。
    u32 execute(u16 op);
    u32 groupMove(u16 op, u32 size);
    u32 groupImmediate(u16 op);  // 0000: ORI/ANDI/SUBI/ADDI/EORI/CMPI/BTST 等
    u32 groupMisc(u16 op);       // 0100: MOVEM/LEA/JMP/JSR/CLR/NEG/NOT/TST/EXT 等
    u32 groupQuickAlu(u16 op);   // 0101: ADDQ/SUBQ/Scc/DBcc
    u32 groupBranch(u16 op);     // 0110: Bcc/BRA/BSR
    u32 groupMoveq(u16 op);      // 0111
    u32 groupOrDiv(u16 op);      // 1000: OR/DIVU/DIVS/SBCD
    u32 groupSub(u16 op);        // 1001: SUB/SUBA/SUBX
    u32 groupCmpEor(u16 op);     // 1011: CMP/CMPA/CMPM/EOR
    u32 groupAndMul(u16 op);     // 1100: AND/MULU/MULS/ABCD/EXG
    u32 groupAdd(u16 op);        // 1101: ADD/ADDA/ADDX
    // ABCD と SBCD。命令語の形式が同じで補正の向きだけが違うのでまとめる。
    // memoryMode は -(Ay),-(Ax) 形式か、isAdd は ABCD か SBCD か。
    u32 execBcdAddSub(u16 op, bool memoryMode, bool isAdd);

    u32 groupShift(u16 op);
    // メモリに対する 1 ビットシフト。命令語の形式がレジスタ版と違う。
    u32 memoryShift(u16 op);  // 1110: ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR

    // 未実装命令に当たったときの共通処理。
    u32 unimplemented(u16 op);

    Bus& bus_;
    M68kState st_;
    // 保留中の割り込みレベル (0 = なし)。
    u32 pendingIrq_ = 0;
    // 保留中の割り込みが使うベクタ番号 (0 = 自動ベクタ)。
    u32 pendingVector_ = 0;
    // アドレスエラー処理の入れ子の深さ。
    // ハンドラのベクタ自体が奇数を指す場合など、実機でも入れ子は起きる。
    // 無限に潜ると SP を食い潰すだけなので段数で打ち切る。
    int addressErrorDepth_ = 0;

    // RESET 命令の通知先。
    ResetCallback resetCallback_ = nullptr;
    void* resetContext_ = nullptr;
};

}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_H
