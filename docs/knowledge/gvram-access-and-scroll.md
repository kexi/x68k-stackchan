---
type: Attested Computation
title: GVRAMのCPUアクセス幅とMODE3表示スクロールの分離
description: VC R0をCPUアクセス幅に流用した誤りを訂正し、CRTC R20とMODE3 G0スクロールをホストで検証。
status: draft
generated: { by: codex, at: 2026-09-06T07:27:30Z }
verified:
  - { by: process:host-video-hardware-tests, at: 2026-09-06T07:27:30Z }
  - { by: process:host-regression-tests, at: 2026-09-06T07:31:00Z }
sources:
  - id: mame-crtc
    resource: https://raw.githubusercontent.com/mamedev/mame/master/src/mame/sharp/x68k_crtc.cpp
  - id: mame-video
    resource: https://raw.githubusercontent.com/mamedev/mame/master/src/mame/sharp/x68k_v.cpp
  - id: access-tests
    resource: ../../test/test_gvram_bus.cpp
  - id: raster-tests
    resource: ../../test/test_graphic_raster.cpp
  - id: compositor-tests
    resource: ../../test/test_compositor.cpp
  - id: tiled-tests
    resource: ../../test/test_tiled_compositor.cpp
---

# 訂正した前提

旧実装はCPUのGVRAMアクセス幅と表示色数の両方にVC R0 ($E82400) を使用し、
256色の上位窓と65536色の全窓を先頭へ折り返していた。これは誤り。
CPUのアクセス幅はCRTC R20 ($E80028) のbits9..8で選ぶ。
表示側VC R0と独立し、4bitアクセス中に16bit表示を選ぶこともできる。[^mame-crtc]

| R20 & 0x0300 | 有効CPU窓 | 共有wordのlane |
| --- | --- | --- |
| 0 | C00000/C80000/D00000/D80000 | 下から4bitずつ |
| 0x0100 | C00000/C80000 | 下位/上位8bit |
| 0x0300 | C00000..C7FFFF | 16bit全体 |

標準モードの未使用窓は読取FFFF、書込無視。byteアクセスは同じlaneを保ち、
4/8bitの偶数byte書込は画素を変更しない。R20 bit11のbuffer accessは先頭512KBを
raw wordとして扱う。上位buffer窓の読取と未定義0x0200は、標準仕様を確定した
ものではなく、安全なFFFF/書込無視とした。[^mame-crtc][^access-tests]

# 表示スクロール

MODE3 (VC R0 bits1..0 = 3) はG0のR12/R13だけを使い、表示座標に加えたXYを
それぞれ511でmaskして512×512のGVRAMを読む。CPU側物理アドレスは動かさない。
text/spriteの座標にも足さない。MODE0/1/予約MODE2の描画は今回変更しない。
[^mame-video][^raster-tests][^compositor-tests]

GraphicRaster→Compositor→TiledCompositorとLCD通常/zoom、host main/play/guiの
各入口で実Crtcを渡す。末尾nullptr既定値はraw fixtureの旧API互換用で、
本番接続の代用ではない。TiledCompositorは有効MODE3 XYの変化をrender時に
検出し、MMIOだけのpage flipでも両bufferを失効する。同じ有効XYへの書込では
再合成しない。LCD captureは完成したframeの転送なのでprotocol変更は不要。
[^tiled-tests]

# 検証と限界

2026-09-06、Nix devShellの `just test-video-hardware` で63 cases / 32,000 assertions成功。
R20/VCの不一致、MMIO byte/word、窓境界、XY折返し、stride/canary、非zero viewport、
text/sprite固定、二枚buffer、null/接続/mode往復を含む。ROM・実機は使用していない。
`just test-host` の全host回帰も2/2 CTest成功（約34秒）。

未対応：R20 bufferbitによるGVRAM表示抑止、indexed各planeのscroll、CRTCによる
表示寸法/周期の変更、既知のsprite属性bitとBGセル寸法の不一致。
今回の成功を標準X68000互換の完成やCoreS3の性能改善実証とは扱わない。

[^mame-crtc]: `sources.mame-crtc`
[^mame-video]: `sources.mame-video`
[^access-tests]: `sources.access-tests`
[^raster-tests]: `sources.raster-tests`
[^compositor-tests]: `sources.compositor-tests`
[^tiled-tests]: `sources.tiled-tests`
