---
title: gcc の 68000 コードを Human68k の .X として動かす
description: nixpkgs の pkgsCross.m68k で X68000 の実行ファイルを作る手順と、実際に踏んだ 3 つの罠 (PIC、ELF ヘッダぶんの下駄、crt0 のリンク順)。hello が A> から動くところまで実測。
type: reference
tags: [x68000, human68k, toolchain, gcc, m68k, nix, elf]
verified: confirmed
sources:
  - 実測（x68k-stackchan + calude-famicom-game-x68k、2026-08-27、aarch64-darwin）
  - 実測（gcc 15.3.0 / binutils 2.46、nixpkgs-unstable の pkgsCross.m68k）
updated: 2026-08-27
---

# gcc の 68000 コードを Human68k の .X として動かす

X68000 用の自作プログラムを、専用のクロスアセンブラ (vasm/HAS) 無しで
作る手順。**hello が `A>` から動いて `A>` に戻るところまで実測済み**。

## ツールチェーン

`nixpkgs` の `pkgsCross.m68k` に gcc 15.3.0 と binutils 2.46 がある。
**aarch64-darwin でバイナリキャッシュから降ってくる** ので、ローカルビルドは要らない。

**Why not vasm / vlink** (X68000 界隈の定番): nixpkgs に無い。自前で
derivation を書くと、環境の再現性が upstream の tar の生死に依存する。

**Why `m68k-unknown-linux-gnu`** (ホスト付きターゲット): nixpkgs に
bare-metal の `m68k-none-elf` が無い。`-ffreestanding -nostdlib` で
libc とスタートアップを一切使わなければ、出てくるのはただの 68000 コード。

必須のフラグ:

```
-m68000 -O2 -ffreestanding -nostdlib -fno-builtin -fno-common -fno-pic -fno-PIC
```

リンクは **gcc ではなく ld を直に呼ぶ**。gcc のドライバは `-nostdlib` を
付けてもターゲット既定のリンク指定を足すため、自前のリンカスクリプトと
衝突して `PHDR segment not covered by LOAD segment` で落ちる。

```
m68k-unknown-linux-gnu-ld --emit-relocs -n -T game.ld -o game.elf crt0.o game.o
```

`--emit-relocs` が要るのは、再配置表を作るのに `R_68K_32` の位置が要るため。

## 罠 1: gcc が既定で PIC を吐く

**症状**: `PC=$000002` で halt。あるいは関数ポインタが 0 になる。

nixpkgs のクロス gcc は PIC が既定。そのままだと絶対番地への参照が
**GOT (Global Offset Table) 経由**になる。

```
b4: 4bfa 009e   lea %pc@(154 <_GLOBAL_OFFSET_TABLE_>),%a5
b8: 2f2d 0008   movel %a5@(8),%sp@-      <- GOT から引く
c6: 4e90        jsr %a0@                 <- A0 = 0 で 0 番地へ飛ぶ
```

X 形式に GOT は無いので、引いた先が全部 0 になる。**`-fno-pic -fno-PIC`**
を付けると `R_68K_32` の素直な絶対参照になる (この hello では再配置が
3 個 → 16 個に増えた。GOT に隠れていたぶんが表に出る)。

X 形式は再配置表で絶対番地を直す仕組みなので、**PIC は不要かつ有害**。

## 罠 2: ELF ヘッダぶんの下駄が二重に足される

**症状**: 命令の途中へ JSR して「未実装命令」に見える halt。

リンカスクリプトを `. = 0;` で始めると
`PHDR segment not covered by LOAD segment` で落ちるので、
`. = SIZEOF_HEADERS;` で ELF 自身のヘッダのぶんを空けることになる。
すると `.text` の開始が 0 ではなく `$74` などになり、
**リンカが埋めた絶対番地にその下駄が乗る**。

Human68k は「イメージの先頭を 0 とみなして読み込み番地を足す」ので、
下駄を残すと二重に足さり、全部の絶対参照が `$74` ずれる。

**直し方**: 変換時に、再配置される場所に入っている値から
`.text` の開始アドレスを引く。

```python
text_base = text["addr"]
for off in offsets:
    (value,) = struct.unpack_from(">I", image, off)
    struct.pack_into(">I", image, off, (value - text_base) & 0xFFFFFFFF)
```

エントリポイントも同じ理由で `e_entry - text["addr"]` に直す。

## 罠 3: main が crt0 より前に来る

**症状**: エントリが 0 にならない。`entry=$00002C` のような値になる。

gcc は `main` を **`.text.startup`** へ置くことがある。リンカスクリプトで
`*(.text.startup)` を `*(.text)` より前に書いていると、**crt0 より前に
main が並ぶ**。

**直し方**: crt0 を専用セクション (`.text.crt0`) に置き、リンカスクリプトの
`.text` の先頭に固定する。

```
.section .text.crt0,"ax",%progbits    /* crt0.S 側 */

.text : {
    *(.text.crt0)                     /* リンカスクリプト側 */
    *(.text.startup)
    *(.text)
    ...
}
```

## DOS コールは F-line、TRAP ではない

Human68k の DOS コールは **F-line 命令** (`.dc.w $FF00+n`) で発行する。
`TRAP #15` は IOCS 用で別物。詳細は
[x68000-emulator-pitfalls.md](x68000-emulator-pitfalls.md) の
「例外が積む PC」の節。

```
| void _dos_print(const char *s);
_dos_print:
    move.l  4(%sp), -(%sp)
    .dc.w   0xFF09                  | DOS _PRINT
    addq.l  #4, %sp
    rts
```

なお ELF の m68k ABI は C の識別子にアンダースコアを足さないので、
C の `_dos_print` はアセンブラでも `_dos_print` のまま
(`__dos_print` にすると undefined reference になる)。

`lea symbol, %a0` は既定で PC 相対になり
`Conversion of PC relative displacement to absolute` で落ちる。
絶対参照が欲しいなら **`lea symbol.l, %a0`** と `.l` を明示する。

## ディスクへ載せる

`tools/make_sasi_image.py inject` で、既存の起動可能イメージへ足せる。

```
python3 tools/make_sasi_image.py inject rom/hdd0.hdf out.hdf --add GAME.X
```

Human68k の配布物を再入手しなくても、**既に動いているイメージ自身が
配布物の代わりになる**。HUMAN.SYS / COMMAND.X / CONFIG.SYS は
組み直しの前後でバイト一致することを確認済み。

## 動いていることの確かめ方

```
A>hello
HELLO FROM X68000
BSS OK
A>
```

`A>` に戻ることが重要で、これで次のすべてが同時に言える:

- X 形式のヘッダと再配置表を Human68k が受け付けた
- 絶対参照が正しい先を指している (文字列が化けていない)
- DOS コール (F-line) が通る
- crt0 の bss ゼロ埋めが効いている (`BSS OK`)
- `_EXIT2` で正常終了した (halt していない)

**bss の検査は `volatile` を付けないと消える。** 付けないと
「静的変数の初期値は 0 だから合計も 0」と畳み込まれ、変数ごと消えて
`bss=0` になる (= 何も検査しないテストになる)。
