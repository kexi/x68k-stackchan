// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// スプライトコントローラ (CYNTHIA)。$EB0000 のレジスタと $EB8000 のスプライト VRAM。
//
// 実装範囲: スプライトレジスタ 128 個、BG スクロールと BG 制御、
// PCG (キャラクタパターン) と BG ネームテーブルを収める 32KB の VRAM。
// 描画そのものは video/sprite_raster.h が持つ。ここは「状態を持つ」だけにする。
//
// --- アドレスの割り当て ------------------------------------------------------
//
//   $EB0000-$EB03FF : スプライトレジスタ。8 バイト x 128 個
//   $EB0800-$EB0803 : BG0 スクロール (X, Y)
//   $EB0804-$EB0807 : BG1 スクロール (X, Y)
//   $EB0808         : BG 制御 (表示面の選択と表示許可)
//   $EB080A-$EB080D : 解像度レジスタ (CRTC のモードに追随する)
//   $EB8000-$EBBFFF : PCG。8x8 は 32 バイト、16x16 は 128 バイト
//   $EBC000-$EBDFFF : BG0 ネームテーブル。64x64 セル x 1 ワード
//   $EBE000-$EBFFFF : BG1 ネームテーブル。同上
//
// 根拠は rom/iplrom.dat ($FE0000 -> ファイル先頭) の IOCS スプライト処理。
// 主要な裏付けを以下に挙げる (アドレスは実機の番地)。
//
// (1) レジスタの個数と間隔: $FFBFDA の初期化が
//     `lea $EB0000,a0 / move.w #$7F,d0 / moveq #0,d1 / move.l d1,(a0)+ /
//      move.l d1,(a0)+ / dbra d0,*-4` で $EB0000 から 128 回 x 8 バイトを
//     ゼロで埋める。128 個 x 8 バイトが確定する。
//     (生ワード列 $FFBFDA: 41f9 00eb 0000 303c 007f 7200 20c1 20c1 51c8 fffa)
//
// (2) 1 個あたり 4 ワードの並び: $FFC11E の SP_REGST が
//     `andi.w #$7F,d1 / lea $EB0000,a0 / lsl.w #3,d1 / adda.w d1,a0` で
//     番号 x 8 を足し、そこから d2 → d3 → d4 → d5 を 2 バイトずつ書く。
//     最後の d5 だけ `andi.w #$3,d5` を通す。つまり 4 ワード目は 2bit しか
//     意味を持たない = プライオリティ。
//     (生ワード列 $FFC126: 0241 007f 41f9 00eb 0000 e749 d0c1)
//
// (3) BG スクロールと制御: $FFC18E の BGSCRLST が `btst #0,d1` で
//     $EB0800 か $EB0804 を選び、そこへ d2 (X) と d3 (Y) をワードで書く。
//     $FFC1E4 の BGCTRLST は $EB0808 を読み書きし、BG1 側では
//     `rol.w #3,d0 / lsl.w #3,d2` と 3bit ずらす。BG0 が bit2-0、
//     BG1 が bit5-3 に居ることが分かる。$FFC230 の BGCTRLGT は逆に
//     BG1 で `andi.l #$38,d0 / lsr.l #3,d0`、BG0 で `andi.l #$7,d0` と
//     取り出しており、同じ割り当てを裏から確かめられる。
//
// (4) スプライト表示の許可: $FFC050 の SP_ON が
//     `ori.w #$40,$E82600 / ori.w #$200,$EB0808`、$FFC066 の SP_OFF が
//     `andi.w #$FFBF,$E82600 / andi.w #$FDFF,$EB0808` と、$E82600 の bit6 と
//     $EB0808 の bit9 を必ず対で操作する。両方が立って初めて出る。
//     (生ワード列 $FFC054: 0079 0040 00e8 2600 0079 0200 00eb 0808)
//
// (5) PCG の大きさ: $FFC09E の SP_DEFCG が、d2==0 なら
//     `lsl.w #5,d1` (x32) して 16 ワード、d2!=0 なら `lsl.w #7,d1` (x128)
//     して 64 ワードを $EB8000 からの位置へ転送する。8x8 が 32 バイト、
//     16x16 が 128 バイトで、どちらも 1 ドット 4bit に一致する
//     (8*8*4/8 = 32、16*16*4/8 = 128)。
//
// (6) BG ネームテーブルの形: $FFC286 の BGTEXTST が
//     `andi.w #$3F,d2 / andi.w #$3F,d3 / add.w d2,d2 / lsl.w #7,d3 /
//      add.w d3,d2` で $EBC000 (BG0) / $EBE000 (BG1) からの位置を作る。
//     X は 0-63 で 2 バイト刻み、Y は 0-63 で 128 バイト刻み。
//     64x64 セル x 1 ワードが確定する。
//     (生ワード列 $FFC2A2: 0242 003f 0243 003f d442 ef4b d443 d0c2)
//
// Why not スプライト VRAM をバス側で素通しの配列にしないか: $EB8000 からの
// 32KB は PCG と BG ネームテーブルが同居する 1 つの実体で、IOCS も
// $EBC000 を「$EB8000 の続き」として直接触る (上の (6))。PCG 用と
// ネームテーブル用に別の配列を持つと、$EBBF00 から 16x16 パターンを
// 定義したときに BG0 のネームテーブルへはみ出す実機の挙動が消える。
// 実際に PCG 領域を使い切るソフトはこの重なりを前提にする。

#ifndef X68K_CORE_DEV_SPRITE_H
#define X68K_CORE_DEV_SPRITE_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Sprite
{
public:
    // スプライトの個数と 1 個あたりのバイト数。上の (1)(2)。
    static constexpr u32 kSpriteCount = 128;
    static constexpr u32 kSpriteStride = 8;

    // スプライト VRAM は 32KB ($EB8000-$EBFFFF)。
    static constexpr u32 kVramSize = 0x8000u;

    // VRAM 内での BG ネームテーブルの位置。上の (6)。
    static constexpr u32 kBg0NameOffset = 0x4000u;  // $EBC000
    static constexpr u32 kBg1NameOffset = 0x6000u;  // $EBE000

    // BG ネームテーブルは 64x64 セル。1 セル 1 ワード。
    static constexpr u32 kBgCellsX = 64;
    static constexpr u32 kBgCellsY = 64;

    // PCG 1 パターンのバイト数。上の (5)。
    static constexpr u32 kPcg8Bytes = 32;
    static constexpr u32 kPcg16Bytes = 128;

    // BG が使うセルの大きさ。BG は 16x16 の PCG を並べる。
    static constexpr u32 kBgCellSize = 16;

    void reset();

    // --- レジスタ ($EB0000-$EB080D) ---
    //
    // オフセットは $EB0000 からのバイト数。ワード単位でしか意味を持たない。
    [[nodiscard]] u16 read(u32 offset) const;
    void write(u32 offset, u16 value);

    // --- スプライト VRAM ($EB8000-$EBFFFF) ---
    //
    // オフセットは $EB8000 からのバイト数。
    [[nodiscard]] u8 vramRead8(u32 offset) const
    {
        return offset < kVramSize ? vram_[offset] : 0u;
    }

    void vramWrite8(u32 offset, u8 value)
    {
        if (offset < kVramSize)
        {
            vram_[offset] = value;
        }
    }

    [[nodiscard]] const u8* vram() const
    {
        return vram_.data();
    }

    // --- スプライトレジスタの読み解き -----------------------------------------
    //
    // 1 個は 4 ワード。上の (2) で確かめた並び。
    //
    //   +0 : X 座標
    //   +2 : Y 座標
    //   +4 : パターン番号とパレットブロック、反転
    //   +6 : プライオリティ (bit1-0)
    //
    // 座標には 16 の下駄が履かせてある。実機のスプライト面は表示領域の
    // 外側にも座標を持ち、画面左端が X=16、上端が Y=16 になる。
    // 画面外へ出しきる (= 消す) ために負の位置を表せる必要があるので、
    // 符号なしのまま下駄を引いて符号付きへ落とす。
    static constexpr int kCoordOffset = 16;

    // スプライト n の画面上の位置。画面左上を (0,0) とする。
    [[nodiscard]] int spriteX(u32 index) const
    {
        return static_cast<int>(spriteWord(index, 0) & 0x03FFu) - kCoordOffset;
    }

    [[nodiscard]] int spriteY(u32 index) const
    {
        return static_cast<int>(spriteWord(index, 1) & 0x03FFu) - kCoordOffset;
    }

    // +4 のワード (属性)。パターン番号・反転・パレットブロックが同居する。
    //
    // 【裏付けの限界】このワードの内訳だけは IPL-ROM から確かめられない。
    // SP_REGST ($FFC140) は呼び出し側の d2 をそのまま `move.w d2,(a0)` で
    // 書くだけで、ビットを分解しないため ROM に情報が残らない。
    // 確かなのは「+4 が 1 ワードである」ことまで。
    //
    // 内訳は X68000 の一般的な資料 (CYNTHIA のスプライトスコープ) に従い
    //   bit7-0  : パターン番号
    //   bit8    : 水平反転 (HREV)
    //   bit9    : 垂直反転 (VREV)
    //   bit15-12: パレットブロック
    // としてある。パターン番号が 8bit である点だけは ROM 側にも傍証があり、
    // SP_DEFCG ($FFC0A6) が `andi.l #$FF,d1` でパターン番号を 8bit に
    // 丸めてから PCG のアドレスを組み立てる。
    //
    // bit8/bit9 が反転、bit15-12 がパレットブロックという割り当ては
    // 未検証。実物のソフトで絵が左右反転する / 色が化けるようなら、
    // まずここを疑うこと。
    [[nodiscard]] u16 spriteAttr(u32 index) const
    {
        return spriteWord(index, 2);
    }

    // PCG のパターン番号 (0-255)。
    //
    // Why not 上位ビットまでパターン番号に含めないか: SP_DEFCG ($FFC0A6) が
    // `andi.l #$FF,d1` で 8bit に丸めてから PCG のアドレスを作る。
    // 9bit 以上として読むと、IOCS が定義できない番号をスプライトが指せて
    // しまい、PCG 領域の外 (BG ネームテーブル) をパターンとして読み始める。
    [[nodiscard]] u16 spritePattern(u32 index) const
    {
        return static_cast<u16>(spriteWord(index, 2) & 0x00FFu);
    }

    // パレットブロック (0-15)。テキスト/スプライトパレット 16 色 x 16 ブロック。
    [[nodiscard]] u8 spritePaletteBlock(u32 index) const
    {
        return static_cast<u8>((spriteWord(index, 2) >> 12) & 0x0Fu);
    }

    // 水平反転 (bit8) / 垂直反転 (bit9)。
    [[nodiscard]] bool spriteFlipH(u32 index) const
    {
        return (spriteWord(index, 2) & 0x0100u) != 0;
    }

    [[nodiscard]] bool spriteFlipV(u32 index) const
    {
        return (spriteWord(index, 2) & 0x0200u) != 0;
    }

    // プライオリティ。0 は非表示、1-3 が表示 (値が大きいほど手前)。
    //
    // 根拠: SP_REGST ($FFC158) が 4 ワード目を `andi.w #$3,d5` で 2bit に
    // 丸めてから書く。値 0 が非表示なのは、$FFBFDA の初期化が全レジスタを
    // ゼロで埋めることと対応する。ゼロクリアが「全スプライトを消す」を
    // 意味しなければ、起動直後に 128 個のスプライトが画面左上に重なる。
    [[nodiscard]] u8 spritePriority(u32 index) const
    {
        return static_cast<u8>(spriteWord(index, 3) & 0x03u);
    }

    [[nodiscard]] bool spriteVisible(u32 index) const
    {
        return spritePriority(index) != 0;
    }

    // --- BG ($EB0800-$EB0808) -------------------------------------------------

    // BG plane (0 or 1) のスクロール量。
    [[nodiscard]] u16 bgScrollX(u32 plane) const
    {
        return plane < 2 ? reg_[(0x0800u + plane * 4u) / 2u] : 0u;
    }

    [[nodiscard]] u16 bgScrollY(u32 plane) const
    {
        return plane < 2 ? reg_[(0x0802u + plane * 4u) / 2u] : 0u;
    }

    // $EB0808 の生の値。
    [[nodiscard]] u16 bgControl() const
    {
        return reg_[0x0808u / 2u];
    }

    // BG plane の表示許可。3bit フィールドの bit0 (BG0 は bit0、BG1 は bit3)。
    //
    // 根拠: BGCTRLST ($FFC1E4) が d2 (ネームテーブル番号) と d3 (表示するか) を
    // どのビットへ置くかを追うと決まる。d2 が有効なとき $FFC1F6 は
    // `moveq #$F8,d0 / andi.w #$1,d2 / add.l d2,d2` で d2 を 1bit 左へ寄せる
    // (= bit1)。d3 が有効かつ非 0 なら $FFC20A の `ori.w #$1,d2` で bit0 を
    // 立てる。BG1 のときだけ $FFC214 が `rol.w #3,d0 / lsl.w #3,d2` と
    // マスクと値をまとめて 3bit ずらす。よって
    //   bit0 / bit3 = 表示許可、bit1 / bit4 = ネームテーブル番号。
    // (生ワード列 $FFC1F6: 70f8 0242 0001 d482 / $FFC208: 6704 0042 0001)
    //
    // Why not bit1 を表示ビットとしないか: 一度そう読み違えた。d2 側にだけ
    // `add.l d2,d2` があるので「先に書かれる d2 が下位」と見えるが、
    // シフトされているのは d2 のほうで、d3 は shift 無しの bit0 に落ちる。
    // 逆に取ると、起動時の `move.w #$10,$EB0808` ($FF6426) が
    // 「BG1 表示 ON」の意味になり、スプライト面の表示許可 (bit9) が
    // 立っていない起動直後に BG1 が出ることになってしまう。
    // $10 は bit4 = BG1 のネームテーブル番号 1 の指定で、表示は OFF が正しい。
    [[nodiscard]] bool bgEnabled(u32 plane) const
    {
        if (plane >= 2)
        {
            return false;
        }
        const u32 shift = plane * 3u;
        return (bgControl() & (1u << shift)) != 0;
    }

    // BG plane が使うネームテーブル番号 (0 or 1)。3bit フィールドの bit1。
    [[nodiscard]] u32 bgTextArea(u32 plane) const
    {
        if (plane >= 2)
        {
            return 0;
        }
        const u32 shift = plane * 3u + 1u;
        return (bgControl() >> shift) & 1u;
    }

    // スプライト/BG 全体の表示許可 ($EB0808 の bit9)。上の (4)。
    //
    // $E82600 の bit6 (VideoController::spriteEnabled) と AND で効く。
    // IOCS は必ず両方を対で操作するので、片方だけ立てても出ない。
    [[nodiscard]] bool displayEnabled() const
    {
        return (bgControl() & 0x0200u) != 0;
    }

    // --- 描画側が使う早見 -----------------------------------------------------

    // 表示すべきスプライトが 1 つでもあるか。
    //
    // 走査を丸ごと省くための早期判定に使う。レジスタ書き込みのたびに
    // 更新しておくので、描画のたびに 128 個を数え直さずに済む。
    // Human68k のコンソールはスプライトを一切使わないため、この判定が
    // そのまま「スプライト処理の費用がゼロ」を意味する。
    [[nodiscard]] bool anySpriteVisible() const
    {
        return visibleCount_ != 0;
    }

    [[nodiscard]] u32 visibleSpriteCount() const
    {
        return visibleCount_;
    }

    // BG が 1 面でも表示されているか。
    [[nodiscard]] bool anyBgEnabled() const
    {
        return bgEnabled(0) || bgEnabled(1);
    }

private:
    // レジスタ空間は $EB0000-$EB080D。ワード配列として持つ。
    //
    // Why not スプライト 128 個を構造体の配列にしないか: レジスタは
    // バイト単位でも書かれる (Machine::ioWrite8 が read-modify-write で
    // 通す) ので、構造体にすると書き込みのたびにフィールドへ分解し直す
    // ことになる。生のワード配列で持ち、読み解きはアクセサ側で行う。
    static constexpr u32 kRegWords = 0x0810u / 2u;

    [[nodiscard]] u16 spriteWord(u32 index, u32 word) const
    {
        if (index >= kSpriteCount || word >= 4)
        {
            return 0;
        }
        return reg_[(index * kSpriteStride) / 2u + word];
    }

    // 表示中のスプライト数を数え直す。プライオリティのワードが変わったときだけ呼ぶ。
    void recountVisible();

    std::array<u16, kRegWords> reg_{};
    std::array<u8, kVramSize> vram_{};
    u32 visibleCount_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_SPRITE_H
