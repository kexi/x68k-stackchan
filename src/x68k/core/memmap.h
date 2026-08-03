// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// X68000 のアドレス空間の定義。
//
// 対象機種は ACE 以降 (実質 EXPERT 相当)。初代のみカスタムチップ構成が異なり
// (VINAS/VSOP/RESERVE 系)、シャープが無償公開した IPLROM も EXPERT 用の
// ver 1.0 が基準なので、ここに合わせるのが最も資料と実物が揃う。
//
// 出典は docs/knowledge/x68000-memory-map.md を参照。

#ifndef X68K_CORE_MEMMAP_H
#define X68K_CORE_MEMMAP_H

#include <cstdint>

namespace x68k
{

// 68000 のアドレスバスは 24bit。上位 8bit は出力されないので、
// バスアクセスのたびにこのマスクで折り返す必要がある。
inline constexpr std::uint32_t kAddressMask = 0x00FFFFFFu;

// --- メインメモリ ---------------------------------------------------------
// 実装容量は SRAM $ED0008 の値と一致させる必要がある (IPLROM がそこを見る)。
// 本エミュレータは 1MB で始める。Human68k のプロンプト到達に 2MB は要らず、
// PSRAM の消費を抑えられるため。増やす場合は kSramDefaultRamSize も変えること。
inline constexpr std::uint32_t kMainRamBase = 0x000000u;
inline constexpr std::uint32_t kMainRamSize = 0x100000u;  // 1MB

// --- ベクタ / ワーク領域の主要アドレス ------------------------------------
// ブート時に IPLROM とブートセクタが使う固定アドレス。
inline constexpr std::uint32_t kIocsVectorBase = 0x000400u;  // IOCS コールベクタ
inline constexpr std::uint32_t kIocsWorkBase = 0x000800u;
inline constexpr std::uint32_t kDosCallVectorBase = 0x001800u;
inline constexpr std::uint32_t kHumanWorkBase = 0x001C00u;
// IPLROM はブートセクタ (1024 バイト) をここに読み込んで実行する。
inline constexpr std::uint32_t kBootSectorLoadAddr = 0x002000u;
// ブートセクタが HUMAN.SYS を読み込む先。実行ファイルヘッダ込みで $0067C0 から
// 読み、ヘッダを除いた本体が $006800 に載る (ロード先固定形式)。
inline constexpr std::uint32_t kHumanLoadReadAddr = 0x0067C0u;
inline constexpr std::uint32_t kHumanLoadAddr = 0x006800u;

// --- グラフィック VRAM ----------------------------------------------------
// 実 VRAM は 512KB だが、アドレス空間としては 2MB 分が割り当てられている。
inline constexpr std::uint32_t kGvramBase = 0xC00000u;
inline constexpr std::uint32_t kGvramEnd = 0xE00000u;

// --- テキスト VRAM --------------------------------------------------------
// 4 プレーン × 128KB。1bit/dot で 4 枚重ねて 16 色。1 ライン = 128 バイト。
// キャラクタ VRAM ではなくビットマップである点に注意 (どの文字が書かれたかは
// VRAM からは分からない)。
inline constexpr std::uint32_t kTvramBase = 0xE00000u;
inline constexpr std::uint32_t kTvramPlaneSize = 0x20000u;  // 128KB
inline constexpr std::uint32_t kTvramPlaneCount = 4u;
inline constexpr std::uint32_t kTvramSize = kTvramPlaneSize * kTvramPlaneCount;  // 512KB
inline constexpr std::uint32_t kTvramEnd = kTvramBase + kTvramSize;
// テキスト画面 1 ラインのバイト数 (1 プレーンあたり)。1024 dot / 8 = 128。
inline constexpr std::uint32_t kTvramBytesPerLine = 128u;

// --- I/O ------------------------------------------------------------------
inline constexpr std::uint32_t kCrtcBase = 0xE80000u;       // VICON
inline constexpr std::uint32_t kVideoCtrlBase = 0xE82000u;  // VIPS: パレットと画面制御
inline constexpr std::uint32_t kDmacBase = 0xE84000u;       // HD63450
inline constexpr std::uint32_t kAreaSetBase = 0xE86000u;    // メモリコントローラ
inline constexpr std::uint32_t kMfpBase = 0xE88000u;        // MC68901
inline constexpr std::uint32_t kRtcBase = 0xE8A000u;        // RP5C15
inline constexpr std::uint32_t kPrinterBase = 0xE8C000u;
inline constexpr std::uint32_t kSysPortBase = 0xE8E000u;
inline constexpr std::uint32_t kOpmBase = 0xE90000u;    // YM2151
inline constexpr std::uint32_t kAdpcmBase = 0xE92000u;  // MSM6258
inline constexpr std::uint32_t kFdcBase = 0xE94000u;    // uPD72065
inline constexpr std::uint32_t kSasiBase = 0xE96000u;
inline constexpr std::uint32_t kSccBase = 0xE98000u;  // Z8530: マウス
inline constexpr std::uint32_t kPpiBase = 0xE9A000u;  // i8255: ジョイスティック
inline constexpr std::uint32_t kIoScBase = 0xE9C000u;

// エリアセットレジスタ。IPLROM が起動直後に書き込んでスーパーバイザ領域を設定する。
//
// 罠: IPLROM 1.3 以降はここへの書き込みに CLR.B を使う。68000 の CLR は
// read-modify-write として実装されており、実機は「読んでから書く」。
// このアドレスの読み出しに副作用を持たせると 1.3 以降が起動しない。
inline constexpr std::uint32_t kAreaSetReg = 0xE86001u;

// --- スプライト -----------------------------------------------------------
inline constexpr std::uint32_t kSpriteRegBase = 0xEB0000u;  // CYNTHIA
inline constexpr std::uint32_t kSpriteVramBase = 0xEB8000u;
inline constexpr std::uint32_t kSpriteVramEnd = 0xEC0000u;

// --- SRAM (バッテリバックアップ) ------------------------------------------
inline constexpr std::uint32_t kSramBase = 0xED0000u;
inline constexpr std::uint32_t kSramSize = 0x4000u;  // 16KB
inline constexpr std::uint32_t kSramEnd = kSramBase + kSramSize;

// --- ROM ------------------------------------------------------------------
inline constexpr std::uint32_t kCgromBase = 0xF00000u;
inline constexpr std::uint32_t kCgromSize = 0xC0000u;  // 768KB
inline constexpr std::uint32_t kCgromEnd = kCgromBase + kCgromSize;

inline constexpr std::uint32_t kIplromBase = 0xFE0000u;
inline constexpr std::uint32_t kIplromSize = 0x20000u;  // 128KB
inline constexpr std::uint32_t kIplromEnd = kIplromBase + kIplromSize;

// リセット後の実行開始アドレス。IPLROM 先頭のベクタ ($FF0000 の 0/4 番地相当) から
// SSP=$002000, PC=$FF0010 が読まれる。トレースの起点として頻繁に参照するので定数化する。
inline constexpr std::uint32_t kResetPc = 0xFF0010u;
inline constexpr std::uint32_t kResetSsp = 0x002000u;

// IPLROM 内の 6x12 ANK フォントの位置。
//
// CGROM はシャープの無償公開の対象外 (漢字フォントのベンダ権利のため) で
// 入手できないことがあるが、6x12 の ANK フォントは IPLROM 内にある。
// これを使えば CGROM 無しでも英数字コンソールを表示できる。
// 実際の代替経路は video/cgrom_fallback.h にある。
//
// アドレスは rom/iplrom.dat (EXPERT 用 v1.0) を読んで確かめた実測値。
// 資料でよく挙がる $FFCFF6 には 68000 の命令列があり、フォントはその
// $22 バイト後ろから始まる。$FFD018 から 254 文字 x 12 バイトが連続し、
// $FFDC00 でちょうど終わる。添字は文字コードそのもの。
// 注意: 文字数は 256 ではなく 254。
inline constexpr std::uint32_t kIplromAnk6x12Addr = 0xFFD018u;
inline constexpr std::uint32_t kIplromAnk6x12Count = 254u;

}  // namespace x68k

#endif  // X68K_CORE_MEMMAP_H
