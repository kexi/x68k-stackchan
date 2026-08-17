---
title: ストレージ実装方針 — 先読みバッファ + SPI クロック + セクタ長
description: SD の読み出しが遅い問題への確定方針。flash への移行と LRU キャッシュを却下した根拠、および「そもそもディスク由来か」を先に実測する撤退条件つきの段階計画。
type: reference
tags: [x68k, emulator, esp32s3, sd, sdspi, fatfs, storage, performance]
verified: measured
sources:
  - 実測（M5Stack CoreS3、Human68k 3.02 稼働中、2026-08-17）
  - ESP-IDF v5.5.2 のソース読解（sdspi_host.c / vfs_fat.c）
  - 多エージェントによる設計と敵対的検証（調査 3 / 設計 3 / 攻撃 12 / 統合 1）
updated: 2026-08-17
---

# 実装方針: 「先読みバッファ + SPI クロック + FATFS セクタ長」の 3 点に絞る

## 0. 最重要の前提訂正 (実装前に必ず読むこと)

事前調査と 3 案が共有していた前提のうち、**実際にソースを読んで反証されたもの**が 3 つある。これを知らずに着手すると、丸ごと 1 段階が無駄になる。

### (A) 「g_sasiBuffer が PSRAM だから CMD18 が無効化される」は**この基板では成立しない**

3 案すべてが「内部 SRAM バウンスで CMD18 が有効になり 38% 改善」を根拠にしていたが、**誤り**。

CoreS3 の SD は **SDSPI (SPI モード)** であって SDMMC ペリフェラルではない。事前調査が引用した `sdmmc_cmd.c:596` の `!esp_ptr_external_ram(dst)` ガードは `#if !SOC_SDMMC_PSRAM_DMA_CAPABLE` の中にあり、これは SDMMC ペリフェラル用のマクロ。SDSPI 経路で使われるのは `sdspi_host_check_buffer_alignment` (`~/esp/esp-idf/components/esp_driver_sdspi/src/sdspi_host.c:1014`) で、**4 バイト整列なら PSRAM でも true を返す。PSRAM 判定は無い。**

さらに決定的なのは、SDSPI ドライバが**転送先に関係なく必ず自前の内部 DMA バッファを経由する**こと:

- `sdspi_host.c:31` `#define SDSPI_BLOCK_BUF_SIZE (SDSPI_MAX_DATA_LEN + 4)` = 516 バイト
- `sdspi_host.c:160-168` `get_block_buf()` が `heap_caps_malloc(..., MALLOC_CAP_DMA)` で 1 本だけ確保 (**毎回 malloc ではない。起動後は使い回し**)
- `sdspi_host.c:772` `size_t will_receive = MIN(rx_length, SDSPI_MAX_DATA_LEN) - extra_data_size;` — **512B ずつのループ**
- `sdspi_host.c:802` `memcpy(data + extra_data_size, rx_data, will_receive);` — 呼び出し側のバッファへ memcpy

**つまり呼び出し側のバッファは DMA の着地点に一度もならない。** バウンスバッファを内部 SRAM に置いても、変わるのは「既に起きている memcpy の宛先」だけで、SD の I/O は 1 バイトも変わらない。しかも SPI 転送は 512B 単位のループで固定なので、`max_transfer_sz` を上げても 1 回のトランザクションは大きくならない。

→ **内部 SRAM バウンス (psram-cache 段階 2 / sd-tuning 層 C) はやらない。** 最も逼迫している資源 (内部 SRAM、reserve 後の largest 110KB、IPL-ROM 128KB の内部配置と直接競合) を、効果ゼロの変更に使うところだった。

### (B) CLMT は `r+b` で開いたファイルには**絶対に作られない**

sd-tuning の層 B (raw sdmmc + CLMT) の土台が崩れる。

`~/esp/esp-idf/components/fatfs/vfs/vfs_fat.c:401`:
```c
if(!(fat_mode_conv(flags) & (FA_WRITE))) {
    ...
    file->cltbl = clmt_mem;      // 412 行
} else {
    file->cltbl = NULL;          // 426 行
}
```

`storage_sd.cpp:243` は `std::fopen(path, "r+b")` で開く → `O_RDWR` → `FA_READ|FA_WRITE` → **else 側で必ず NULL**。`CONFIG_FATFS_USE_FASTSEEK=y` にしても hdd0.hdf の CLMT は永久に作られない。

しかも `FILE*` → `FIL` に到達する公開 API が無い (`FIL` は `vfs_fat.c:51` の静的構造体の flexible array member の中)。読み取り専用で開き直せば writeSector が壊れ、2 本開けば `max_files = 4` (`storage_sd.cpp:69`) を圧迫したうえ、sd-tuning 自身が危険 1 で禁じている「同じファイルに FATFS の書き込みハンドルと raw 書き込みが同居する」状態そのものになる。

→ **raw sdmmc 経路はやらない。** 実装不能であり、仮に回避しても sd-tuning の危険 1 (静かなディスク破損) を自ら作り込む。

### (C) 実効クロックの落ち込みが本当にディスク由来かは、まだ誰も測っていない

perf-reality の攻撃が指摘した通り、`main/main.cpp:770-783` の実効クロック表示は**スライス全体の実時間**しか見ていない。`readSector` の中で何 ms 使っているかを分離した計測は実機・ホストのどちらにも存在しない。

3 案の期待効果はすべて「dir の 16.19ms/スライスのうち 12.31ms が SD 待ち」という**逆算による推定**に立っており、その逆算は「1 スライスあたり 32 ブロック読む」という仮定に依存する。ホストのトレースでは定常状態のリクエストは 4 セクタ (1KB) 単位なので、この仮定は成立しない可能性が高い。

→ **段階 0 で実測する。それが 12ms なら続行、0.1ms なら本方針ごと撤退する。** これが最初の分岐点。

---

## 1. 採用する方針と、却下した案の why not

### 採用: 3 つだけ

| 層 | 内容 | 変更規模 |
|---|---|---|
| **層 1** | `host.max_freq_khz = 40000` + 失敗時 20MHz フォールバック | 約 15 行 |
| **層 2** | `CONFIG_FATFS_SECTOR_512=y` (可変セクタ長モードの解消) | sdkconfig.defaults 1 行 |
| **層 3** | `SdDisk` に **単一の先読みバッファ (16KB)** を PSRAM に持つ | 約 60 行、`storage_sd.cpp` 内で完結 |

### 却下: flash-mmap (全面)

**why not:**
- perf-reality が `fatal` 判定。加えて data-loss / wear / usability の 3 攻撃すべてが独立に破綻点を挙げた。
- **決定的なのは wear の指摘**: FAT (offset 131072) とルートディレクトリ (offset 212992) が、提案する 512KB の CRC32 検証窓の内側にある。ゲストが 1 バイトでも書けば CRC が変わり、**次回起動のたびに 512KB (4KB ブロック 128 個) を消去・再書き込みする**。「初回だけ数秒」が「ほぼ毎回の起動で数秒〜40 秒」になる。設計の核心である「2 回目以降は追加コストゼロ」が成立しない。
- NOTICE.md の配布条件に**実際に抵触する**。CGROM はシャープの無償公開対象外で、flash に焼くと本体譲渡時に非再配布物が同梱されて渡る。SD なら利用者が管理する取り外し可能な媒体だが、flash は本体と一体化して見えない。この差は性能課題ではなく配布条件の変化。
- `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` (sdkconfig:553) と `CONFIG_ESPTOOLPY_FLASHMODE="dio"` (sdkconfig:558) が食い違っており、esptool に渡るのは文字列側の `"dio"`。QIO 前提の帯域見積もりが崩れる。

→ **やらない。partitions.csv の storage 領域 (0x410000 / 0xBF0000) は未使用のまま温存する。**

### 却下: psram-cache の 8 面 LRU (128KB)

**why not:** ヒット率の根拠が、この**リポジトリ自身のコミット済み実測と正面から矛盾する**。

`docs/knowledge/cores3-emulator-runtime.md:470-478` は、同じ `--trace-disk` セッション (dir + type human.sys) について:

| | 値 |
|---|---|
| リクエスト回数 | 58 回 |
| 再読み | 23 回 (**わずか 4%**) |
| 直前の続きから読む | **68%** |

と記録し、`:479` で「**LRU キャッシュはほぼ効かない**」と明記している。事前調査の「99 リクエスト / 再読み 49% / 88% ヒット」はこの検証の中にしか存在せず、生成物がリポジトリに無い。

そして**両者は矛盾していない**。「4 セクタ刻みの連番スイープ」は再利用ではなく**逐次性**であり、記録済みの「直前の続きから読む 68%」と同じ現象を別の言葉で述べている。**逐次性に効くのは LRU ではなく先読み。** 128KB / 8 面 / タグ探索 / 書き込み無効化という構造は、この分布に対して過剰かつ的外れ。

さらに実測で確認した通り、PSRAM→PSRAM の memcpy は同一バス上で read/write 両方を払うため実効約 21MB/s。16KB ブロックのミス充填は **memcpy だけで往復 1.5ms** かかり、SD ブロック 4 個分に相当する。ミス時にキャッシュ無しより遅くなる。

→ **同じ効果を、単一の 16KB 先読みバッファ (約 60 行、タグ 2 個) で得る。** 逐次性 68% はこれで丸ごと拾える。

### 却下: 書き込みの遅延化 (PSRAM オーバーレイ / ライトバック)

**why not:** data-loss 攻撃が正しい。`machine.cpp:909-910` の `finishSasiWrite` が `writeSector` の戻り値で `sasi_.status = 0x02` を立てており、**これがゲストに書き込み失敗を伝える唯一の口**。遅延化するとこのエラーチャネルが構造的に消え、`machine.cpp` のコメントが警告する「Human68k は書けたつもりで先へ進む」状態に退行する。しかも本方針は読み出しだけを対象にするので、書き込みを触る理由がそもそも無い。

---

## 2. データの流れ (確定版)

### 読み出し

```
machine.cpp:1155  disk_->readSector(lba, sasi_.buffer, sectors)
  ↓  [DiskImage 境界 — core/ は無変更]
SdDisk::readSector(lba, dst, count)          storage_sd.cpp:255
  │
  ├─ 要求 [lba, lba+count) が先読みバッファのタグ範囲に完全に収まる
  │    → memcpy(dst, ra_ + (lba - raLba_)*256, count*256)   … SD に触らない
  │
  ├─ 収まらない、かつ count <= 64 (16KB)
  │    → raLba_ = lba;  fseek(lba*256) + fread(ra_, 16KB)
  │      末尾がファイル末尾を越える場合は読めた分だけを有効長 raLen_ とする
  │      → memcpy(dst, ra_, count*256)
  │
  └─ count > 64 (起動時の 221 セクタのみ)
       → 先読みを迂回して従来どおり dst へ直読み。バッファは無効化する
```

**バッファは 1 本、16KB、PSRAM。** タグは `raLba_` (開始 SASI LBA) と `raLen_` (有効バイト数) の 2 個だけ。LRU も連想探索も無い。

**なぜ 16KB か:** 定常リクエストが 4 セクタ (1KB) 固定なので、1 回の充填で最大 16 リクエストを賄える。32KB にすると充填の memcpy と fread が倍になり、逐次性が途切れた瞬間の損が増える。

**なぜアラインしないか:** 逐次読みは「直前の続き」なので、要求 LBA をそのまま開始点にする方がヒットが長く続く。16KB 境界に丸めると、要求が境界の直前から始まったとき手前側を無駄に読む。

### 書き込み (**現状から一切変えない**)

```
machine.cpp:909-910  disk_->writeSector(lba, sasi_.buffer, sectors)
  ↓
SdDisk::writeSector                          storage_sd.cpp:271
  fseek + fwrite + fflush        … 現状のまま
  + 先読みバッファとの重なりを無効化 (raLen_ = 0)   ← 唯一の追加
```

無効化は**部分更新せず必ず全体を捨てる**。書き込みは稀 (ホストのトレースで dir/type セッション中 0 件) なので、正しさを単純さで買う。

**重なり判定は「重なるなら捨てる」の一方向のみ**なので、境界計算を誤っても「余計に捨てる」側にしか倒れない。psram-cache が data-loss 攻撃で指摘された「64KB 書き込みで先頭ブロックしか更新せず残りに古い値が残る」欠陥は、**部分更新をしないので原理的に起こらない**。

### 永続化

**SD が唯一の正。変更なし。** flash も PSRAM オーバーレイも使わない。

ただし data-loss 攻撃が発見した**既存のバグ**が 1 件ある (本方針とは独立):

`storage_sd.cpp:296` の `std::fflush(f)` には「FATFS は fclose まで FAT を確定させないので 1 セクタごとに吐き出す」というコメントがあるが、**この主張はこのビルドでは成立しない**。`fflush` は newlib のユーザ空間バッファを `write()` へ落とすだけで、`vfs_fat_write` が `f_sync` を呼ぶのは `CONFIG_FATFS_IMMEDIATE_FSYNC` の中だけ。sdkconfig:1490 は `# CONFIG_FATFS_IMMEDIATE_FSYNC is not set`。

→ **段階 4 で `fsync(fileno(f))` を足して別途直す。** 本方針の一部ではなく、独立した耐久性の修正として扱う。速度に影響するので同時に入れない (詳細は §5)。

---

## 3. 利用者の手順

### **変わらない。1 文字も。**

```
/x68k/iplrom.dat   (128KB, 必須)
/x68k/cgrom.dat    (768KB, 任意)
/x68k/hdd0.hdf     (必須)
/x68k/fd0.xdf      (任意)
```

SD に置いて電源を入れる。焼く手順は増えない。NOTICE.md は無変更。README の SD 配置表も無変更。

### justfile に足すレシピ: **1 つだけ**

```just
[doc('ディスク読み出しの実時間を実機で測る (段階 0 の判断材料)')]
measure-disk:
    idf.py -D X68K_MEASURE_DISK=1 build flash monitor
```

ビルドフラグで計測コードを囲むのは、`storage_sd.cpp` のホットパスに `esp_timer_get_time()` を常駐させないため。段階 0-3 の判断が済んだら**このレシピごと消す**。

新しい設定ファイルも、パーティション書き込みも、カードの再フォーマット指示も**追加しない**。

### README への追記

**段階 3 完了後、実測値が出てから**、`README.md:46,73,85` の実効クロック表に「dir 実行中」の行を 1 行足す。現在の「5.1MHz / 実機比 51%」は A> 放置時の値で、ディスク待ちがゼロの状態。本方針が改善するのは看板に載っていない側なので、**実測が出る前に README を触らない**。

usability 攻撃が指摘した「reformat as FAT32」の類の**トラブルシュート文言は一切書かない**。raw 経路を採らないので、そもそもカードに起因する分岐が発生しない。

---

## 4. fatal 判定への対処

fatal は **flash-mmap / perf-reality の 1 件のみ**。

### fatal: 「実効クロックの落ち込みがディスク由来である証拠が無い」

> 137 リクエスト / 349,479,312 サイクルに対し、ディスクに触るスライスは 17,623 中 133 (0.75%) のみ。0.75% のスライスに 12.3ms を課しても平均スライスは 3.89ms → 3.98ms、つまり 5145 → 5022 kHz の 2% 効果にしかならない。80% の落ち込みはディスク遅延では説明できない。

**対処: 段階 0 で実測し、結果次第で本方針ごと撤退する。** 推定で先へ進まない。

具体的には `SdDisk::readSector` / `writeSector` を `esp_timer_get_time()` で挟み、スライスあたりの累積ディスク時間を実効クロックと**並べて**出す (`main.cpp:779` の既存ログに追記):

```
[disk] slice=20000cyc wall=16.19ms disk=12.31ms req=32 (76% がディスク)
```

- **disk が wall の 60% 以上** → ディスク由来で確定。段階 1 へ進む。
- **disk が wall の 10% 未満** → **本方針を全面撤退し、原因の再調査に戻る。** その場合の第一容疑は `dmac.cpp:96-114` の `runChannel` が `device->dmaRead(&value)` を**1 バイトずつ**呼び、`machine.cpp:877-882` が PSRAM の `sasi_.buffer[sasi_.bufferPos++]` を索く経路。1KB セクタあたり PSRAM バイトロード 1024 回 + 仮想呼び出し 2 段で、ここは**キャッシュでは 1us も減らない**。
- **中間 (10-60%)** → 層 1 と層 2 だけ入れて再測。層 3 (先読み) は保留。

この分岐を段階 0 に置くことが、この方針の最も重要な設計判断。**3 案はいずれも段階 0 を「計測基盤の整備」としか位置づけておらず、撤退条件にしていなかった。**

### fatal ではないが対処するもの

| 攻撃の指摘 | 対処 |
|---|---|
| PSRAM バウンスの前提が誤り (§0-A) | **やらない。** 内部 SRAM を消費しない |
| CLMT が作れない (§0-B) | **やらない。** raw 経路を採らない |
| 8 面 LRU がリポジトリの実測と矛盾 | **やらない。** 単一先読みバッファに置換 |
| `max_transfer_sz=4096` が効いていない (`storage_sd.cpp:52-58` が `ESP_ERR_INVALID_STATE` を握り潰す) | 依存しない設計にした。SDSPI は 512B ループ固定なのでこの値は読み出し性能に影響しない |
| `fflush` が永続化していない | 段階 4 で `fsync` を追加 (§5) |
| LCD とのバス競合 (GPIO35 が MISO と D/C を共有) | **物理制約なので解消しない。** 段階 0 の計測で `spi_device_acquire_bus` の待ち時間を別カウンタに分離し、支配的なら段階 3 の後に対処を検討する。「最大 30.7ms」は docs に根拠が見つからなかったので、推測ではなく実測する |
| SD ホットスワップでキャッシュが古い値を返す | 先読みバッファは `readSector` 1 回の寿命に近く、書き込みで必ず無効化される。現状の `fread` と危険度が変わらない |

### 対処しない (明示)

- **LCD/SD の SPI バス分離** — GPIO35 が基板上で共有されており、ソフトで解決できない
- **`readSector` の非同期化** — `machine.cpp:1155` が CPU の 1 命令の内側で完了を要求する構造の変更が必要で、影響範囲が本方針の 10 倍
- **flash への移行全般** — §1 の通り
- **書き込み経路の性能改善** — 計測上ボトルネックでなく、エラー経路を壊す危険に見合わない

---

## 5. 段階的な導入順序と、各段で動くはずの数字

各段は**独立に revert 可能**。前段の数字が想定と違ったら次へ進まない。

### 段階 0 — 計測 (実機、コード変更は計測のみ)

**やること:**
1. `SdDisk::readSector` (`storage_sd.cpp:255`) / `writeSector` (`storage_sd.cpp:271`) を `esp_timer_get_time()` で挟み、累積時間と呼び出し回数を静的カウンタに貯める。`X68K_MEASURE_DISK` で囲む
2. `main.cpp:779` の実効クロックログに `disk=%.2fms req=%u` を追記
3. バス取得待ちを分離するため、`readSector` の先頭でも時刻を取り、`fread` 直前との差を別カウンタへ
4. `host/main.cpp:96` の `FileDisk::writeSector` に `[disk] write` のトレースを追加 (`readSector` の `:83` と同形)。**なお `:103` に既にトレースがあるという事前調査の記述は誤りで、実際に存在しない**

**動くはずの数字:** 実効クロックは**変わらない** (dir 1235 kHz のまま)。新たに `disk=` の実測値が出る。

**判断:** §4 の分岐。60% 以上なら続行。

### 段階 1 — SPI クロック 40MHz

**やること:** `storage_sd.cpp:38` の `SDSPI_HOST_DEFAULT()` の直後に `host.max_freq_khz = 40000;`。`esp_vfs_fat_sdspi_mount` (`:70`) が失敗したら 20MHz で 1 回だけ再試行。実際の到達クロックは `sdspi_host_get_real_freq` (`sdspi_host.c:309`) で取得してログに出す (**要求値ではなく実測値**。SPI マスタは 80MHz の整数分周に丸めるので 40000 を要求しても 40MHz とは限らない)。

**動くはずの数字:** 512B あたりのデータフェーズ 210us → 105us。段階 0 の `disk=` が **12.31ms → 約 8.9ms**。dir **1235 → 約 1560 kHz**。

**外れたら:** 到達クロックが 20MHz のままなら、カードか配線の制約。層 2/3 は独立に効くので設計は変えず、期待値だけ 1.3 倍の時間で再計算する。

### 段階 2 — FATFS を固定セクタ長モードへ

**やること:** `sdkconfig.defaults` に `CONFIG_FATFS_SECTOR_512=y` (現状 sdkconfig:1457 が `CONFIG_FATFS_SECTOR_4096=y`)。

**なぜ効くか:** `ffconf.h:244-245` の `FF_MIN_SS = MIN(512,4096) = 512` / `FF_MAX_SS = MAX(512,4096) = 4096` により、FatFs が**可変セクタ長モード**でコンパイルされている。全ての `SS(fs)` が実行時ロードになり、セクタ長の判断が分岐になる。加えて `CONFIG_FATFS_PER_FILE_CACHE=y` (sdkconfig:1483) なので各 `FIL` が 512B でなく **4096B の窓バッファ**を抱える (`max_files=4` で約 16KB)。

**注意:** `FF_MAX_SS` を変えるとマウント済み全ボリュームのセクタ長検証が変わる。sd-tuning が「効果は小さく副作用も小さい」と評価していたのは誤りで、**副作用は小さくない**。この段は単独で焼き、SRAM の保存・フロッピー・ROM ロードが全て通ることを確認してから次へ進む。

**動くはずの数字:** `disk=` が **約 8.9ms → 7.5-8.5ms**。dir **約 1560 → 1650-1750 kHz**。内部 SRAM の `[mem:after reserve] internal free` が **約 14KB 増える**。

### 段階 3 — 先読みバッファ 16KB (本命)

**やること:**
1. `main.cpp:391` の `g_sasiBuffer` 確保の直後に、16KB を `MALLOC_CAP_SPIRAM` で確保。`reserveMemory()` 内に置くのは PSRAM 断片化を避けるため (同関数のコメント `:367-370` の方針に従う)。**必須ではないので、取れなくても起動を止めない** (取れなければ先読み無効で従来動作)
2. `SdDisk` に `ra_` / `raLba_` / `raLen_` を追加し、`setReadAheadBuffer(u8*, size)` で外から与える (`Machine::setSasiBuffer` (`machine.h:73`) と同じ「実体の確保は呼び出し側」の作法に合わせる)
3. `readSector` / `writeSector` を §2 の通りに変更
4. 10 秒ごとのログにヒット率を追加

**動くはずの数字:** SD の `fread` 回数が定常状態で **約 1/8**。`disk=` が **7.5-8.5ms → 1.5-2.5ms**。dir **約 1700 → 3000-3800 kHz**、type **約 1400 → 2800-3500 kHz**。ヒット率は**逐次性 68% を根拠に 60-85%** を見込む (再読み 4% ではなく逐次性に賭けている)。

**外れたら:** ヒット率が 40% を割ったら、`docs/knowledge/cores3-emulator-runtime.md:474` の「直前の続きから読む 68%」自体が dir/type 以外のワークロードで成立していない。バッファを 32KB に上げて 1 回だけ再測し、それでも駄目なら**層 3 を revert して層 1+2 で確定させる** (段階 4 は独立に続行可)。

### 段階 4 — 書き込みの永続化を直す (独立、性能とは別件)

**やること:** `storage_sd.cpp:296` と `:424` (SdFloppy) の `std::fflush(f)` の後に `fsync(fileno(f))` を追加。`:294-296` のコメントを事実に合わせて書き換える (「fflush では FAT が確定しない。IMMEDIATE_FSYNC が無効なので f_sync は呼ばれない」)。

**動くはずの数字:** 実効クロックは**書き込みを伴わない dir/type では変わらない**。ファイルコピー時の書き込みが遅くなる (1 セクタあたり FAT + ディレクトリの実書き込みが増える)。段階 0 で入れた `write` の計測でコストを確認する。

**なぜ最後か:** 速度を落とす方向の変更なので、段階 1-3 の測定に混ぜない。ただし**耐久性の修正なので必ず入れる** (「電源を切るだけ」で最後の書き込みが失われる現状は、性能より優先度が高い)。

### 段階 5 — docs の訂正

`docs/knowledge/cores3-emulator-runtime.md:479` の「LRU キャッシュはほぼ効かない」は**正しいので残す**。同 `:481-483` の「先読み・生の sdmmc_read_sectors・境界合わせが効く」のうち、**raw sdmmc は §0-B の理由で実装不能**であることを追記する。8.5 節の末尾に、段階 0-3 の実測値と §0 の 3 つの前提訂正を書き足す。

---

## 6. テスト (core/ の ESP32 非依存を壊さない形)

### 重要な制約: `SdDisk` は `src/x68k/platform/` にあり、ホストでビルドされない

先読みのロジックをそのまま `storage_sd.cpp` に書くと、ホストのテストに一切載らない。かといって `CachedDisk` を `core/` に置くと (psram-cache の案)、core/ に「ディスクキャッシュ」という platform の関心事が漏れる。

**採る形: 純関数だけを core/ に切り出す。**

`src/x68k/core/read_ahead.h` (ヘッダのみ、`ESP32` 非依存、`just core-guard` を通る):

```cpp
namespace x68k {
// 先読みバッファの状態。実体 (バッファ) は持たない。
struct ReadAheadTag { u32 lba = 0; u32 sectors = 0; };

// 要求がタグの範囲に完全に収まるか。収まるならバッファ内の
// セクタオフセットを *offset に返す。
bool readAheadHit(const ReadAheadTag&, u32 lba, u32 count, u32* offset);

// 書き込みがタグと重なるか (重なるなら捨てる)。
bool readAheadOverlaps(const ReadAheadTag&, u32 lba, u32 count);
}
```

`storage_sd.cpp` はこの 2 関数を呼ぶだけになり、**判断ロジックは 100% ホストのテストに載る**。`storage_sd.cpp` 側に残るのは `fseek`/`fread`/`memcpy` の実行だけ。

### ホストで守れること

`test/` に追加 (doctest、既存の作法に合わせる):

1. **ヒット判定の境界** — 要求がタグの先頭ちょうど / 末尾ちょうど / 1 セクタはみ出す / 完全に外
2. **オフセット計算** — ヒット時の `*offset` が `lba - tag.lba` と一致
3. **重なり判定の対称性** — 書き込みがタグの前後に隣接するだけなら重ならない、1 セクタでも重なれば true
4. **オーバーフロー** — `lba + count` が `u32` を溢れる入力で誤ヒットしない (SASI の `count=0` は 256 セクタ扱い (`machine.h:66`) なので、境界の入力が実際に来る)
5. **`readAheadOverlaps` は保守的側にのみ誤る** — 重なるのに false を返す入力が無いことを、全探索できる小さな範囲 (lba 0-64、count 1-8) で総当たり検証

### ホストで守れないこと (明示)

- 実際の `fread` が正しいバイトを返すか → 実機でのみ検証
- ヒット率の実値 → 実機の 10 秒ログ
- 40MHz の到達可否 → 実機

**ただし段階 3 の実機検証は 1 つで足りる:** dir の一覧と `type human.sys` の出力 (58496 バイト) が段階 2 以前と**完全一致**すること。先読みは読み出し内容を変えてはいけないので、一致しなければバグ。

### 既存のガード

`just core-guard` (CI と pre-commit) は `src/x68k/core/` の ESP32 依存を grep で弾く。`read_ahead.h` は `<cstdint>` 相当しか使わないので通る。`storage_sd.cpp` は既に platform 側なので影響なし。

---

## 7. 期待効果と撤退条件

### 期待効果

| ゲストの状態 | 現状 | 段階 1+2 | 段階 3 まで |
|---|---|---|---|
| A> 放置 | 5145 kHz | 5145 kHz | 5145 kHz (不変) |
| dir 連打 | 1235 kHz | 約 1700 kHz | **3000-3800 kHz** |
| type human.sys | 1022 kHz | 約 1400 kHz | **2800-3500 kHz** |

**上限は A> 放置時の 5145 kHz** で確定している。ディスク待ちがゼロになれば A> と同じ状態に漸近するので、この方針が到達しうる最大値は 5145 kHz。それ以上は `tickDevices` などインタプリタ側の話に戻る。

**3 案との比較:** flash-mmap の想定 4843 kHz には届かない。ただし flash-mmap は wear / data-loss / usability / perf-reality の 4 攻撃すべてで破綻点が出ており、そのうち perf-reality は fatal。本方針は **`storage_sd.cpp` 内に閉じた約 75 行 + sdkconfig 1 行**で、配布条件・利用者手順・partitions.csv・`core/` のいずれにも触れない。

### 撤退条件 (段階ごと)

| 段階 | 撤退条件 | 撤退後どうするか |
|---|---|---|
| **0** | `disk=` がスライス実時間の **10% 未満** | **本方針を全面撤退。** 原因を `dmac.cpp:96-114` のバイト単位ドレインへ移して再調査 |
| **0** | `disk=` が 10-60% | 層 1+2 のみ実施。層 3 は保留し、残りの支配要因を先に特定 |
| **1** | 到達クロックが 20MHz のまま | 層 1 のみ revert (1 行)。層 2/3 は独立なので続行 |
| **2** | SRAM 保存 / フロッピー / ROM ロードのいずれかが壊れる | `sdkconfig.defaults` の 1 行を revert。層 1/3 は続行 |
| **3** | ヒット率 40% 未満 | 32KB で 1 回だけ再測。改善しなければ層 3 を revert し、層 1+2 (dir 約 1700 kHz) で確定 |
| **3** | dir/type の出力が段階 2 以前と 1 バイトでも違う | **即座に revert。** 読み出し内容を変える先読みはバグであり、調整では直さない |
| **4** | 書き込みが体感で問題になるほど遅い | revert しない。耐久性を優先する。遅さが問題なら `fsync` の頻度を落とす別設計を検討 |

### この方針が保証しないこと

- **ばらつきの解消。** LCD が SPI2 を占有している間、SD アクセスは `spi_device_acquire_bus(portMAX_DELAY)` で待つ。先読みは待つ**回数**を減らすが、1 回あたりの待ち時間は変えない。段階 0 でバス待ちを分離計測し、支配的なら別途対処を検討する
- **dir/type 以外のワークロード。** 逐次性 68% は dir/type の 1 セッションから得た値。SX-Window など広い範囲を触るソフトでは分布が違いうる。ただし先読みバッファは 16KB / タグ 2 個なので、効かない場合の損失も小さい (ミス時に 16KB 読む分だけ)
- **書き込みが多いワークロードの速度。** 書き込みは無効化を伴うので、書き込みが密なら先読みが常に冷える。ホストのトレースでは dir/type セッション中の書き込みは 0 件だが、ファイルコピーでは 58KB の書き込みが 1 回で走る

---

## 実装時に触るファイル (まとめ)

| ファイル | 変更 |
|---|---|
| `src/x68k/platform/storage_sd.cpp:38` | `host.max_freq_khz = 40000` + フォールバック (段階 1) |
| `src/x68k/platform/storage_sd.cpp:255-286` | 先読みの分岐と無効化 (段階 3) |
| `src/x68k/platform/storage_sd.cpp:296` | `fsync` 追加 (段階 4) |
| `src/x68k/platform/storage_sd.h` | `setReadAheadBuffer` と `ra_`/`raLba_`/`raLen_` |
| `src/x68k/core/read_ahead.h` | **新規。** 純関数 2 つ (段階 3) |
| `main/main.cpp:391` 付近 | 16KB の PSRAM 確保 (段階 3) |
| `main/main.cpp:538` 付近 | `g_disk.setReadAheadBuffer(...)` (段階 3) |
| `main/main.cpp:779` 付近 | `disk=` のログ追記 (段階 0) |
| `host/main.cpp:96` | `writeSector` のトレース追加 (段階 0) |
| `sdkconfig.defaults` | `CONFIG_FATFS_SECTOR_512=y` (段階 2) |
| `test/` | `read_ahead.h` のテスト (段階 3) |
| `justfile` | `measure-disk` (段階 0、段階 3 後に削除) |
| `docs/knowledge/cores3-emulator-runtime.md:479` 付近 | 実測値と前提訂正 (段階 5) |

**触らないファイル:** `partitions.csv` / `NOTICE.md` / `src/x68k/core/machine.h` / `src/x68k/core/machine.cpp` / `src/x68k/core/dev/dmac.cpp` / `README.md` (段階 3 の実測が出るまで)",
    "attacks": 12,
    "fatal": 1
  },
  "workflowProgress": [
    {
      "type": "workflow_phase",
      "index": 1,
      "title": "Survey"
    },
    {
      "type": "workflow_phase",
      "index": 2,
      "title": "Design"
    },
    {
      "type": "workflow_phase",
      "index": 3,
      "title": "Attack"
    },
    {
      "type": "workflow_phase",
      "index": 4,
      "title": "Decide"
    },
    {
      "type": "workflow_agent",
      "index": 1,
      "label": "survey:access-pattern",
      "phaseIndex": 1,
      "phaseTitle": "Survey",
      "agentId": "ae3c999ab8ebe84b1",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786962919571,
      "queuedAt": 1786962919562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786963149373,
      "tokens": 72179,
      "toolCalls": 35,
      "durationMs": 229802,
      "resultPreview": "{"findings":[{"area":"実測方法と再現手順 (--trace-disk)","detail":"rom/iplrom.dat と rom/hdd0.hdf が現存していたので、机上ではなく実際に走らせて分布を取り直した。コマンド: `./build-host/x68k-run --iplrom rom/iplrom.dat --hdd rom/hdd0.hdf --cycles 900000000 --trace-disk --keys \"dir\\
dir\\
type human.sys\\
\"`。出力形式は host/main.cpp:85 の `std::printf(\"[disk] read lba=%u count=%u%s\\
\", ...)` で、LBA と セクタ数のみ。--keys \"\" (ブートのみ) と上記を 2 本走らせ、前者が後者…"
    },
    {
      "type": "workflow_agent",
      "index": 2,
      "label": "survey:cost-breakdown",
      "phaseIndex": 1,
      "phaseTitle": "Survey",
      "agentId": "a715ee4d096977fa3",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786962919572,
      "queuedAt": 1786962919562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786963368508,
      "tokens": 114395,
      "toolCalls": 72,
      "durationMs": 448936,
      "resultPreview": "{"findings":[{"area":"支配要因の特定 (結論)","detail":"1 回の 512B ブロック読み出しの実測相当コストは約 379 us。内訳はデータクロッキング 210 us (20MHz, 512+CRC+token=524B) と、固定オーバーヘッド 169 us。fseek/fread/VFS/FATFS の「関数呼び出し」自体は 8 us 程度で無視できる。支配的なのは (a) SD ブロックあたりの固定オーバーヘッド、(b) 20MHz という低いクロック、(c) 後述の PSRAM 起因のシングルブロック転落。モデルの検算: 測定値 A> 3.89ms/スライスに対し dir 16.19ms → SD 待ち 12.31ms = 32 ブロック = 16.2KB/スライス、type 19.57ms → 15.68ms = 41 ブロック = 20.7K…"
    },
    {
      "type": "workflow_agent",
      "index": 3,
      "label": "survey:resources",
      "phaseIndex": 1,
      "phaseTitle": "Survey",
      "agentId": "a97ab709897b93250",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786962919572,
      "queuedAt": 1786962919562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786963283398,
      "tokens": 99210,
      "toolCalls": 54,
      "durationMs": 363826,
      "resultPreview": "{"findings":[{"area":"PSRAM 確保の内訳と残量","detail":"main/main.cpp の reserveMemory() が確保する 8 領域の合計は 4,458,496 B (4.25 MB)。内訳: g_mainRam 2,097,152 B (main.cpp:374, memmap.h:38 kMainRamSize=0x200000)、g_textVram 524,288 B (main.cpp:376, memmap.h:65 kTvramSize=0x80000)、g_cgRom 786,432 B (main.cpp:378, memmap.h:106 kCgromSize=0xC0000)、g_frameBufferA/B 各 153,600 B (main.cpp:380/382, display_lcd.h:36-37 の 320…"
    },
    {
      "type": "workflow_agent",
      "index": 4,
      "label": "design:flash-mmap",
      "phaseIndex": 2,
      "phaseTitle": "Design",
      "agentId": "a9ad07fd09bdf95be",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786963368545,
      "queuedAt": 1786963368528,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "結論: 「HDD イメージ全体を flash に焼く」のではなく、**flas…",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964085778,
      "tokens": 81778,
      "toolCalls": 18,
      "durationMs": 717233,
      "resultPreview": "{"approach":"結論: 「HDD イメージ全体を flash に焼く」のではなく、**flash mmap を read-only の下層に置き、書き込みは PSRAM オーバーレイで受け、SD を永続の正本にする 3 層構成**。切り口として flash mmap を採るが、単純な全面移行は成立しないので変形する。\
\
## 実測で確定した前提 (rom/hdd0.hdf を直接検査)\
\
20,971,520 B のうち **非ゼロの 4KB ブロックは 40 個 = 160KB のみ**。最後の非ゼロバイトは offset 318,483 (311KB)。つまり LBA 1244 (256B 換算) より先は完全な 0 埋め。事前調査のアクセス分布 (FAT lba 516-835、ルートディレクトリ 836、CONFIG.SYS 1244、HUMAN.SYS 8-22…"
    },
    {
      "type": "workflow_agent",
      "index": 5,
      "label": "design:psram-cache",
      "phaseIndex": 2,
      "phaseTitle": "Design",
      "agentId": "a1b6c3bbbc75ab112",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786963368548,
      "queuedAt": 1786963368529,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "## 方針: セクタ LRU ではなく「16KB ブロック x 8 面の先読み…",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786963802419,
      "tokens": 67714,
      "toolCalls": 12,
      "durationMs": 433856,
      "resultPreview": "{"approach":"## 方針: セクタ LRU ではなく「16KB ブロック x 8 面の先読みキャッシュ」\
\
切り口として与えられた「PSRAM 上に LRU のセクタキャッシュ」を、一点だけ訂正して受け取る。**セクタ単位の LRU ではなく、アラインしたブロック単位のキャッシュ**にすべき。事前調査のシミュレーションが決定的:\
\
| 構成 | SD I/O 回数 (99 リクエスト中) |\
|---|---|\
| 純セクタ LRU 64 セクタ (16KB) | 95 (ほぼ無効) |\
| 純セクタ LRU 128 セクタ (32KB) | 95 (ほぼ無効) |\
| 純セクタ LRU 256 セクタ (64KB) | 53 |\
| 純セクタ LRU 512 セクタ (128KB) | 50 (飽和) |\
| **16KB ブロック x 8 面 (128K…"
    },
    {
      "type": "workflow_agent",
      "index": 6,
      "label": "design:sd-tuning",
      "phaseIndex": 2,
      "phaseTitle": "Design",
      "agentId": "ac6dacd719dd3f8a8",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786963368563,
      "queuedAt": 1786963368529,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "## 方針: 「SD のまま、1 リクエストの固定費と本数を削る」4 …",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786963605466,
      "tokens": 59987,
      "toolCalls": 9,
      "durationMs": 236903,
      "resultPreview": "{"approach":"## 方針: 「SD のまま、1 リクエストの固定費と本数を削る」4 層構成\
\
配布方式 (利用者が SD の /x68k/ に ROM と hdf を置く) を一切変えない。partitions.csv の storage 領域も使わない。SD の上に 4 つの層を積み、下から順に単独で計測できるようにする。\
\
### 層 A: SPI クロックを 40MHz へ (storage_sd.cpp:38 の直後 1 行)\
`host.max_freq_khz = 40000;` を SDSPI_HOST_DEFAULT() の後に置く。データフェーズ 210us/512B → 105us。事前調査の [1]。M5GFX が同じ SPI2 で LCD へ 40MHz を出している実績があるので配線的には通る見込みだが、カード個体差で CRC エラーになりう…"
    },
    {
      "type": "workflow_agent",
      "index": 7,
      "label": "attack:flash-mmap:data-loss",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a2d6684a6eaaa9f68",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085795,
      "queuedAt": 1786964085786,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964232437,
      "tokens": 71114,
      "toolCalls": 21,
      "durationMs": 146642,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"SD カードが満杯、または hdd0.hdf が読み取り専用で開かれた状態 (SdDisk::open が storage_sd.cpp:243 で \"r+b\" に失敗し \"rb\" へフォールバックした場合) でゲストがファイルを書く。現状は machine.cpp:898-916 の finishSasiWrite() が writeSector の false を受けて sasi_.status = 0x02 を立て、ゲストは sasiRead (machine.cpp:1026) でその場で書き込みエラーを受け取る。オーバーレイ導入後は writeSector が PSRAM への memcpy だけで true を返すため、ゲストは「書けた」と信じて FAT とディレクトリエントリを更新…"
    },
    {
      "type": "workflow_agent",
      "index": 8,
      "label": "attack:flash-mmap:wear",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "ae8080cfea01653ea",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085796,
      "queuedAt": 1786964085786,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964181893,
      "tokens": 48509,
      "toolCalls": 15,
      "durationMs": 96097,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"Guest writes any file (or Human68k merely updates a directory timestamp). The PSRAM overlay marks the block dirty, flushDirtyBlocks() writes it back to hdd0.hdf on SD. The FAT (offset 131072, LBA 512-527) and root directory (offset 212992, LBA 832-847) both sit INSIDE the design's 512KB CRC32 validation window (131072 < 524288, 212992 < 524288 -- verifie…"
    },
    {
      "type": "workflow_agent",
      "index": 9,
      "label": "attack:flash-mmap:usability",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "aa8e4e30ceea1097f",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085796,
      "queuedAt": 1786964085786,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964238959,
      "tokens": 70087,
      "toolCalls": 18,
      "durationMs": 153163,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"利用者が `just make-hdd` でファイルの多い Human68k ディレクトリからイメージを作る、あるいは A: にゲームを 1 本コピーする。tools/make_sasi_image.py の FAT12 幾何を計算すると、データ領域は 205KB から 20480KB まで広がり、クラスタは先頭から前方に割り当てられる。flash に焼く 512KB はデータ領域の 1.5% しか覆わない ((512-205)/(20480-205))。risks が提案する 4MB 固定でも 19% で、しかも覆うのは『既に読み終えた OS の低位クラスタ』であって『これから起動するアプリケーション』ではない。読みの大半が層[3] = 現状の SdDisk::readSector に落ち、12ms …"
    },
    {
      "type": "workflow_agent",
      "index": 10,
      "label": "attack:flash-mmap:perf-reality",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a1ed3f8273f59d35d",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085796,
      "queuedAt": 1786964085786,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fatal",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964504039,
      "tokens": 100597,
      "toolCalls": 58,
      "durationMs": 418243,
      "resultPreview": "{"verdict":"fatal","breaks":[{"scenario":"実測で反証: dir x3 + type human.sys を host/main.cpp に guest cycle スタンプを仕込んでトレースした結果、137 リクエスト / 849 セクタが 352,479,312 サイクルに分散。ディスクに触るスライスは 17,623 中 133 (0.75%) のみ。126 スライスがちょうど 4 セクタ (1KB)。expectedGain の『dir 1 スライスあたり 32 ブロック (16.2KB)』は実測 1KB に対し 16 倍の過大評価で、しかも per-request コストを per-slice コストとして扱っている。","why":"本案の期待値計算がそのまま崩れる。提案自身のコストモデル (12.3ms/リクエスト) を実測密度に当てると…"
    },
    {
      "type": "workflow_agent",
      "index": 11,
      "label": "attack:psram-cache:data-loss",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "ad0f8f1655f1af5a0",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085803,
      "queuedAt": 1786964085803,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964438336,
      "tokens": 77271,
      "toolCalls": 28,
      "durationMs": 352533,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"奇数 LBA (256B 単位) への SASI 書き込みが電源断で消える。Human68k が FAT やディレクトリを 1 セクタ (256B) 更新する → SdDisk::writeSector (storage_sd.cpp:281-297) が fwrite + fflush する → fflush は stdio のバッファを write() へ流すだけで、vfs_fat_write → f_write に到達する。FATFS の SD セクタは 512B (ffconf.h:240 FF_SS_SDCARD=512) なので 256B の書き込みは必ず部分セクタ書き込みになり、ff.c:4155-4156 で fp->buf に memcpy されて FA_DIRTY が立つだけで di…"
    },
    {
      "type": "workflow_agent",
      "index": 12,
      "label": "attack:psram-cache:wear",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a76caa6a2276de489",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085803,
      "queuedAt": 1786964085803,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964420305,
      "tokens": 76792,
      "toolCalls": 35,
      "durationMs": 334502,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"複数ブロックにまたがる WRITE が先頭ブロックしかキャッシュ更新しない。実測トレース (copy human.sys test1.dat) の `w lba=1248 cnt=228` は 16KB ブロック 19..23 の 5 ブロックにまたがるが、設計の `blockIndex = lba / 64` は block 19 しか引かない。block 20..23 は SD 側だけが新しくなり、キャッシュには古い内容が残る。2 回目の copy の `w lba=1480 cnt=228` も同様に block 23..26 にまたがり、block 24..26 が取り残される。そして実測列にはその範囲をキャッシュ経由で読む READ が実在する (`r lba=1476` -> block 2…"
    },
    {
      "type": "workflow_agent",
      "index": 13,
      "label": "attack:psram-cache:usability",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "ad517fa4b1c574cf9",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085803,
      "queuedAt": 1786964085803,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964188296,
      "tokens": 60818,
      "toolCalls": 15,
      "durationMs": 102492,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"段階 2 のバウンスバッファを入れて実機に焼くが、4KB 読みが CMD18 一発にならず、期待した 379us→200us が出ない。40MHz 化 (段階 1) だけが効いた状態で「CMD18 化は効かなかった」と誤って結論づける。","why":"storage_sd.cpp:51 で busConfig.max_transfer_sz = 4096 に固定されている。4096 バイトのデータペイロードに SPI トランザクションのコマンド/レスポンス分が加わるので、4KB 転送は 1 トランザクションに収まらない。さらに致命的なのは、直後の spi_bus_initialize が ESP_ERR_INVALID_STATE を「M5Unified が LCD 用に先に SPI2 を握っている場…"
    },
    {
      "type": "workflow_agent",
      "index": 14,
      "label": "attack:psram-cache:perf-reality",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a0a860018674865cf",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085804,
      "queuedAt": 1786964085803,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964252895,
      "tokens": 62627,
      "toolCalls": 21,
      "durationMs": 167091,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"ステージ0を飛ばして本命のキャッシュを実装すると、リポジトリに記録済みの実測トレース (docs/knowledge/cores3-emulator-runtime.md:466-478 の「58 リクエスト / ユニーク 510 セクタ / 再読み 23 回 = 4%」) の方が正しかった場合、16KB ブロックキャッシュは type human.sys を今より遅くする。58 リクエスト x 1KB が約 50 ミス x 16KB に化け、最も遅い PSRAM バス上を通るバイト数が 16 倍になる。","why":"期待効果の根拠である「99 リクエスト → 12 回、再読み 49%」というシミュレーションは、この検証対象の中にしか存在しない。host/main.cpp:83-87 の [disk…"
    },
    {
      "type": "workflow_agent",
      "index": 15,
      "label": "attack:sd-tuning:data-loss",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a54666f4cb3f1a108",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085809,
      "queuedAt": 1786964085808,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964368874,
      "tokens": 67324,
      "toolCalls": 21,
      "durationMs": 283065,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"ゲストが hdf へ書いた直後に電源ボタンを押す / バッテリが切れる。現行コードでも stage 3 のフォールバック経路 (useRaw_ == false) でも、書いた内容が丸ごと消える。","why":"SdDisk::writeSector の末尾の std::fflush(f) は「FATFS は fclose まで FAT を確定させないので 1 セクタごとに吐き出す」というコメント付きで置かれているが、この主張はこのビルドでは成立しない。fflush は newlib のユーザ空間バッファを write() へ落とすだけで、esp-idf の vfs_fat_write が f_sync を呼ぶのは #if CONFIG_FATFS_IMMEDIATE_FSYNC の中だけ。本プロジェ…"
    },
    {
      "type": "workflow_agent",
      "index": 16,
      "label": "attack:sd-tuning:wear",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a0b71d38d766838a8",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964085809,
      "queuedAt": 1786964085808,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964371079,
      "tokens": 64744,
      "toolCalls": 19,
      "durationMs": 285270,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"利用者が Human68k で 512KB のファイルを SD へコピーする操作を 1 日 5 回行う。もし将来この設計の「段階 0 で計測してから flash 移行を再検討する」という逃げ道に従って partitions.csv の storage 領域 (0x410000 から 0xBF0000、SubType=spiffs) へ hdf を載せた場合、FAT 領域が寿命で死ぬ。","why":"設計は flash を退ける理由を「256B 書き込みに対し消去単位 4KB で 16 倍の増幅」「writeSector の頻度が未計測」としているが、致命的なのは増幅率ではなくホットセットの狭さである。20MB イメージ・4KB クラスタの FAT16 では FAT は 10KB = flash の消去…"
    },
    {
      "type": "workflow_agent",
      "index": 17,
      "label": "attack:sd-tuning:usability",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a2d07b3e52a6b8edc",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964181895,
      "queuedAt": 1786964085808,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964596839,
      "tokens": 106776,
      "toolCalls": 39,
      "durationMs": 414944,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"Layer C (internal-SRAM DMA bounce) is built on a premise that is false for this board. The design asserts that because g_sasiBuffer is in PSRAM, IDF \"always gives up on CMD18 and falls into a CMD17+CMD13+malloc/free/memcpy loop per 512B\", and that a 16KB internal-SRAM bounce unlocks a single CMD18. CoreS3 uses SPI mode (SDSPI), not the SDMMC peripheral…"
    },
    {
      "type": "workflow_agent",
      "index": 18,
      "label": "attack:sd-tuning:perf-reality",
      "phaseIndex": 3,
      "phaseTitle": "Attack",
      "agentId": "a68e1106475e29c52",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964188297,
      "queuedAt": 1786964085808,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fixable",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964509549,
      "tokens": 80823,
      "toolCalls": 30,
      "durationMs": 321252,
      "resultPreview": "{"verdict":"fixable","breaks":[{"scenario":"層 D (16KB x 8 面キャッシュ) を投入し、dir と type human.sys を実行する。実トレース (138 リクエスト / 853 セクタ) を再取得して提案どおりの構成をシミュレートすると、SD から読む 512B ブロック数は現状 429 (214KB) に対しキャッシュ後 384 (192KB) にしかならない。定常状態でも 244 → 160。","why":"「ヒット率 88-91%」は実際に出るが、それは SD I/O の削減率ではない。ミスのたびに 4 ブロックの要求に対して 32 ブロックを引くため、プリフェッチ増幅が想定の 1.5 倍ではなく約 8 倍になる。期待効果の表が主張する『SD I/O 回数 12%』は、この増幅を掛け戻していない。実際の転送量削減は約 …"
    },
    {
      "type": "workflow_agent",
      "index": 19,
      "label": "decide",
      "phaseIndex": 4,
      "phaseTitle": "Decide",
      "agentId": "a1143f05044c56806",
      "model": "claude-opus-5[1m]",
      "state": "done",
      "startedAt": 1786964596867,
      "queuedAt": 1786964596863,
      "attempt": 1,
      "lastToolName": "Bash",
      "lastToolSummary": "sed -n '365,400p' main/main.cpp; echo "=== 500-545 ==="; se…",
      "promptPreview": "リポジトリ: /Users/kei/ghq/github.com/kexi/x68k-stackchan
自作 X68000 エミュレータ (M5Stack CoreS3 / ESP32-S3 240MHz, PSRAM 8MB Quad 80MHz, Flash 16MB)。

## 問題 (今日の実機実測)
ディスクを読んでいる間、実効クロックが激しく落ちる。

| ゲストの状態 | 実効クロック |
|---|---|
| A> プロンプトで放置 | 5145 kHz |
| dir を連打 | 1235 kHz |
| type human.sys (58KB) を連続 | **1022 kHz (-80%)** |

1 スライス (20000 サイクル) の実時間は通常 3.89ms、読み出し中は 16-19ms。
**12ms 以上が SD 待ち**。

インタプリタ側で削ろ…",
      "lastProgressAt": 1786964853918,
      "tokens": 124689,
      "toolCalls": 14,
      "durationMs": 257050,
      "resultPreview": "I have all the grounding I need. Writing the final plan now.

The investigation settled several disputed claims decisively — notably that the "PSRAM buffer kills CMD18" premise is false for this board (SDSPI always stages through its own 516B internal buffer), and that CLMT can never be built for an `r+b`-opened file. Both would have wasted entire implementation stages.

---

# 実装方針: 「先読みバッファ + SP…"
    }
  ],
  