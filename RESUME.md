# 再開の手順

`fix-issues-and-sxwindow-prereqs` ブランチの残作業。**2 件は物理的な操作が要る。**

## 1. issue #4 の計測 (ケーブルの抜き差しが要る)

IPL-ROM 側の fast path は実装済みだが**未計測**。CoreS3 が esptool に
応答しない (`No serial data received`。どの `--before` モードでも同じ)。

```sh
# USB ケーブルを抜き差しする ← ここだけ手が要る
just flash
just monitor        # 実効 NNNN kHz を読む。起動直後は 1500-2000 から
                    # 上がってくるので、収束するまで数十秒待つ
```

直近の実測値 (すべて実機):

| 条件 | 実効クロック | 実機比 |
|---|---|---|
| 毎スライス譲る | 2760 kHz | 27.6% |
| 8 スライスに 1 回 | 3195 kHz | 32.0% |
| + メイン RAM の fast path | 3711 kHz | 37.1% |
| + IPL-ROM の fast path | **未計測** | — |

**目標は 50% (5000 kHz)。** 37.1% で止まっている。

## 2. SX-Window の起動 (ソフトウェアが要る)

SX-Window 本体がこの環境に無い。`/tmp/h302` と `/tmp/x68dl` にあるのは
Human68k 3.02 と IPL-ROM だけで、全ファイルシステムを探しても 0 件。

イメージを入手したら:

```sh
just make-hdd <展開したディレクトリ> rom/hdd0.hdf
just run --hdd rom/hdd0.hdf --cycles 900000000 --ppm /tmp/out.ppm
```

足りないものの一覧は `docs/knowledge/sx-window-requirements.md`。

## 3. フロッピー起動 (コードで解ける)

**これは手が要らない。** 止まる場所まで特定済み。

FDC の読み書きと割り込みは動く (`$000C8F` が立ち、ROM のハンドラ
`$FF1130` が走る)。しかしブートセクタが `$002000` に載らない。

```
$FF9086: MOVE.W D1,D0 / LSR.W #8,D0    ; D0 = D1 の上位バイト
$FF908A: TST.B D0 / BEQ +$48           ; ← ここで D0 が 0 だと先へ進まない
         AND.B #3,D0 / OR.B #$80,D0
         BSET #7,$0009E1               ; 完了フラグ
         MOVE.B D0,$E94007             ; ドライブ選択 + モータ
```

`$FF908A` の D0 の由来を追う。ハンドラが積む ST0 と
SENSE INTERRUPT STATUS の返し方を実機と突き合わせるのが筋。

再現:

```sh
# HUMN302I.LZH の human302.xdf (本物の 2HD 起動ディスク) を使う
just run --iplrom rom/iplrom.dat --fd0 <path>/human302.xdf \
  --cycles 400000000 --watch 0x000C8F --trace-disk
```

注意: SRAM の起動デバイスを標準優先順位にしても効かない。ROM は
`$FF0144` の `MOVE.W $ED0018,D1` で**非常に早く**読むので、`reset()` の
後に書いても間に合わない。

## 4. 残り 8% のスループット

FDC の割り込みを繋いだぶんでホストが 1.26s → 1.49s へ落ち、
インライン化で 1.36s まで戻した。残り 8% の出どころは未特定。

```sh
# 計測のやり方 (交互に回してばらつきを潰す)
for i in 1 2 3 4 5; do
  /usr/bin/time -p ./build-host/x68k-run --iplrom rom/iplrom.dat \
    --hdd <image> --cycles 400000000 >/dev/null
done
```

## push

```sh
git push -u origin fix-issues-and-sxwindow-prereqs
```

この環境には git の資格情報が無いので未 push。
