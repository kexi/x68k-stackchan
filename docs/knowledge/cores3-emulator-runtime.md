---
title: CoreS3 実機でエミュレータを動かすときの知見
description: M5Stack CoreS3 (ESP32-S3) 上で X68000 エミュレータを走らせて分かったこと。ウォッチドッグ、メモリ配置、シリアル経由のデバッグ、LCD の見え方。実測値のみ。
type: reference
tags: [cores3, esp32s3, freertos, psram, watchdog, usb-serial-jtag, m5gfx, debugging]
verified: confirmed
sources:
  - 実測（M5Stack CoreS3、ESP-IDF v5.5.2、PSRAM 8MB Quad 80MHz、CPU 240MHz）
  - 実測（x68k-stackchan の実機ログ 2026-08-03）
updated: 2026-08-03
---

# CoreS3 実機でエミュレータを動かすときの知見

ホストで動いたエミュレータをそのまま実機へ持っていくと、**ホストでは
起きない種類の問題**にぶつかる。実際に踏んだものだけを記録する。

## 1. `taskYIELD()` ではウォッチドッグから逃げられない

エミュレーションのホットループで CPU を明け渡すとき、`taskYIELD()` を
毎スライス呼ぶとリセットのループに入る。

**理由**: `taskYIELD()` は同じ優先度のタスクへ譲るだけで、**アイドル
タスクは優先度 0 なので回ってこない**。ESP-IDF のタスクウォッチドッグは
アイドルタスクが一定時間走らないことを検出して落とすので、譲っている
つもりでも殺される。

**直し方**: `vTaskDelay(1)` を使う。これはブロック状態に入るので
アイドルタスクが走れる。

```cpp
g_machine.run(kSliceCycles);
vTaskDelay(1);   // taskYIELD() ではだめ
```

**代償**: `vTaskDelay(1)` は最低 1 tick (既定 10ms) 待つ。20000
サイクルのスライスなら実効 2MHz が上限になる。速度が要るなら
「N スライスに 1 回だけ待つ」形にするか、tick を短くする。

### リセットループに入ると書き込めなくなる

繰り返しリセットしている間は esptool が接続できない。

```
A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.
```

**USB ケーブルを抜き差しする**のが最も確実。CoreS3 のリセットボタンの
長押しは**電源オフ**であって、ダウンロードモードではない。

## 2. 実機の画面は VRAM から読み戻せる

実機のデバッグで最初に困るのは「LCD に何が出ているか分からない」こと。
写真を撮ってもらう以外の手が無いと、確認のたびに人の手が要る。

テキスト VRAM はビットマップなので「どの文字が書かれたか」は直接には
分からないが、**CGROM の 8x16 ANK 字形と 1 セルずつ照合すれば逆に引ける**。
ANK は 256 通りしかなく、総当たりでも十分に速い。

これを USB シリアルへ出す口を付ければ、実機の画面をホストと同じように
テキストで確認できる。

```
 5|A>dir
10|HUMAN              SYS      58496
11|COMMAND            X        28382
```

16x16 の漢字は 2 セルにまたがるので ANK としては当たらない。どの字かまで
分からなくてよく、「漢字が出ている」ことが分かれば CGROM が効いているかの
判断はつく。

実装は `src/x68k/core/video/text_scrape.cpp`。core 側に置いてあるので
ホスト (`--dump-text`) と実機で同じコードが動く。

## 3. シリアル入力は VFS ではなくドライバを直接叩く

シリアルからキーを受け取りたいとき、`getchar()` は既定でブロックする。
入力待ちで画面更新ごと止まるので、非ブロッキングにする必要がある。

**やってはいけない**: `fcntl(fileno(stdin), F_SETFL, O_NONBLOCK)`。
これを入れたところ**ログ出力まで巻き添えで止まった**。VFS 側の口が
塞がるらしく、`ESP_LOGI` も `printf` も出なくなり、実機が生きているのか
死んでいるのかも分からない状態になる。

**正しくは**: USB Serial JTAG ドライバから直接読む。

```cpp
usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
usb_serial_jtag_driver_install(&cfg);

std::uint8_t buf[32];
const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);  // timeout 0
```

`CMakeLists.txt` の `REQUIRES` に `esp_driver_usb_serial_jtag` が要る。

**出力は VFS のままにしておく**。両方をドライバに寄せると `ESP_LOG` の
行が混ざって読めなくなる。

## 4. キーは押下と解放の間隔を空けて送る

MFP のキーボード受信は 1 バイトしか保持しない。押下 (`$xx`) と解放
(`$xx|$80`) を続けざまに書くと、CPU が読み出す前に上書きされて**入力が
丸ごと消える**。

実効 3〜4MHz では 1 バイト読むのに数フレームかかる。キューに溜めて
数フレームおきに 1 イベントずつ送る。

```cpp
// 押下 → 4 フレーム待つ → 解放 → 4 フレーム待つ → 次の文字
```

「打ったのに何も起きない」ときは、まずここを疑う。ホスト側の `--keys` も
200 万サイクルの間隔を空けている。

## 5. PSRAM のバッファを pushImageDMA に直接渡してはいけない

**症状**: 転送しても LCD が更新されない。にもかかわらず古い画面は表示され
続ける。`fillScreen` も効かない。

M5GFX (0.2.26 で確認) は、入力と出力がどちらも RGB565 だと「変換不要」と
判断し、**渡したポインタをそのまま SPI の DMA descriptor に設定する**。

```cpp
// Panel_LCD.cpp
if (param->no_convert) { write_bytes(src, wb * h, use_dma); }

// Bus_SPI.cpp の _setup_dma_desc_links
dmadesc->buf = (uint8_t*)data;   // DMA 可能メモリかの判定なし
```

この経路には次のどれも無い。

- `esp_ptr_dma_capable()` などの判定
- DMA 非対応時のバウンスバッファ
- ESP32-S3 向けの `esp_cache_msync()` (ESP32-P4 の `#if` 内にしかない)

ESP32-S3 は PSRAM DMA 自体には対応しているが、**CPU キャッシュと PSRAM の
同期はハードウェアが保証しない**。同期を挟まないと、CPU がキャッシュへ
書いた内容と GDMA が PSRAM から読む内容が食い違う。

**「古い画面だけ残る」理由**: 320x240x2 = 150KB はデータキャッシュ (64KB)
より大きいので、描画中のキャッシュ追い出しで偶然まとまったフレームが
PSRAM へ書き戻されることがある。それが最後に成功した転送として LCD の
GRAM に残り続ける。LCD は次の書き込みが来るまで内容を保持するので、
以後の転送が失敗しても画面は消えない。

**直し方**: `setSwapBytes(true)` を呼ぶ。変換が入ると M5GFX は内部の
`MALLOC_CAP_DMA` バッファへ画素を写してから送るので、PSRAM を直接
DMA する経路に入らない。

```cpp
M5.Display.setColorDepth(16);
M5.Display.setSwapBytes(true);   // これが無いと PSRAM を直接 DMA する
```

**Why not `esp_cache_msync` を自分で呼ぶか**: M5GFX の DMA 経路は外部 RAM
判定もエラー処理も持たないため、同期しても descriptor の扱いは変わらない。
内部バッファに寄せる方が確実。

**この症状の見分け方**: フレームバッファの中身をシリアルへ吸い出して
確かめられるようにしておくと切り分けが速い。「バッファは正しいのに
LCD が違う」と分かった時点で、描画ロジックではなく転送経路を疑える。
実際、これが分かるまで回転・座標・`fillScreen` を何度も疑って外した。

## 6. LCD は 320x240。等倍では読めない

Human68k の標準コンソールは 768x512、CoreS3 の LCD は 320x240。

等倍で左上を切り出すと 40 桁 x 15 行が入る。**桁数は稼げるが、2 インチの
画面に 8x16 ドットの文字が出るので実際には判読できない。**

2 倍に拡大すると 20 桁 x 7 行と狭くなるが、16x32 になって読める。
どちらが良いかは用途によるので、切り替えられるようにしておく。

**拡大時はダーティ行の追跡をやめて全画面を作り直す方が単純。** ダーティ
管理はテキスト VRAM のタイル行が単位で、拡大すると 1 タイル行が LCD 上の
複数行に伸びるため対応が煩雑になる。変換元が 1/zoom² に減るので、
全画面を作り直しても間に合う（等倍 3.8MHz → 2 倍 3.3MHz、13% の低下）。

## 7. メモリ配置の実測値

| 領域 | サイズ | 配置 |
|---|---|---|
| IPL-ROM | 128KB | **内部 SRAM**（ブート中の命令フェッチ元） |
| メインメモリ | 1MB | PSRAM |
| テキスト VRAM | 512KB | PSRAM |
| グラフィック VRAM | 512KB | PSRAM |
| CGROM | 768KB | PSRAM |
| フレームバッファ | 150KB | PSRAM |
| SASI 転送バッファ | 64KB 弱 | PSRAM |

起動直後の実測:

```
[mem:before reserve] internal free=300727 largest=241664 | psram free=8385100 largest=8257536
[mem:after  reserve] internal free=169651 largest=110592 | psram free=5280308 largest=5242880
```

**SASI の転送バッファは `Machine` に埋め込まないこと。** 埋め込むと内部
SRAM の `.bss` が 88KB まで膨らみ、その後 IPL-ROM 128KB を内部 SRAM へ
置こうとして失敗する。外から注入する形にして PSRAM に置く。SASI の転送は
DMA の完了待ちで一気に流すだけなので遅延に敏感ではない。

**外部注入にしたら `reset()` でポインタを消さないこと。** `sasi_ =
SasiState{}` のような初期化はポインタごと nullptr に戻す。ディスクが
1 セクタも読めなくなり、原因が分かりにくい。

## 8. 実効速度の実測

| 条件 | 実効クロック | 実機比 |
|---|---|---|
| 等倍表示 + ダーティ転送 | 3.8MHz | 38% |
| 2 倍拡大 + 全画面転送 | 3.3MHz | 33% |

いずれも `vTaskDelay(1)` を毎スライス、スライス 20000 サイクル、
IPL-ROM を内部 SRAM に配置した状態。

進捗ログを定期的に出しておくと、**止まったのか遅いだけなのか**が
すぐ分かる。実機は画面を直接見られないので、この 1 行の価値が大きい。

```
I (50919) x68k: 147520000 サイクル実行 (実効 3314 kHz)
```
