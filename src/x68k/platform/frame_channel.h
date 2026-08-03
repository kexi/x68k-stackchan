// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// エミュレーションコアと表示コアの間でフレームを受け渡す。
//
// なぜ要るか:
//   Machine の状態 (テキスト VRAM、パレット、ダーティフラグ、MFP の
//   キー受信レジスタ) を両コアから触ると C++ 上のデータ競合になる。
//   実害としても、Core1 が VRAM を書いている最中に Core0 が読むと
//   1 フレームの中に新旧が混ざるし、Core0 がダーティフラグを消す瞬間に
//   Core1 が立てると更新通知が消えて画面が古いまま止まる。
//
//   Machine を触るのは Core1 だけにして、Core0 へは「完成した RGB565 の
//   フレーム」だけを渡す。こうすると競合が原理的に起きない。
//
// 受け渡しの形:
//   バッファを 2 枚持ち、Core1 が書く側と Core0 が送る側を入れ替える。
//   入れ替えの瞬間だけを排他すれば足りるので、150KB の変換と 150KB の
//   SPI 転送はどちらもロックの外で走る。
//
//   Why not 1 枚をミューテックスで守るか: 変換と転送のどちらか一方が
//   走っている間、もう片方のコアが待つことになる。2 枚あれば
//   「Core1 が次のフレームを作りながら Core0 が前のフレームを送る」が
//   成立し、待ち時間が消える。増えるのは PSRAM 150KB だけ。

#ifndef X68K_PLATFORM_FRAME_CHANNEL_H
#define X68K_PLATFORM_FRAME_CHANNEL_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "machine.h"

namespace x68k_platform
{

class FrameChannel
{
public:
    // バッファ 2 枚を受け取る。実体の確保は呼び出し側の責務
    // (PSRAM の断片化を避けるため起動直後に一括確保したい)。
    // 各バッファは width * height 個の u16 が要る。
    bool begin(x68k::u16* bufferA, x68k::u16* bufferB);

    // --- Core1 (エミュレーション) 側 ---

    // 次のフレームを書き込む先。
    [[nodiscard]] x68k::u16* writeBuffer()
    {
        return writeBuffer_;
    }

    // 書き終えたフレームを Core0 へ渡す。以後 writeBuffer() は
    // もう 1 枚を返す。
    void publish();

    // --- Core0 (表示) 側 ---

    // 新しいフレームがあれば返す。無ければ nullptr。
    //
    // 返ったポインタは done() を呼ぶまで有効。その間 Core1 は
    // もう 1 枚へ書くので、SPI 転送中に内容が変わることはない。
    [[nodiscard]] x68k::u16* take();

    // 転送が終わったことを伝える。これを呼ぶまで Core1 は
    // 新しいフレームを公開できない (公開しようとしたぶんは捨てられる)。
    void done();

private:
    // 入れ替えの一瞬だけを守る。変換も転送もこの外で走る。
    SemaphoreHandle_t mutex_ = nullptr;

    x68k::u16* writeBuffer_ = nullptr;  // Core1 が書いている
    x68k::u16* frontBuffer_ = nullptr;  // 公開済み。Core0 が送る
    bool hasNewFrame_ = false;          // publish 済みでまだ take されていない
    bool isFrontInUse_ = false;         // Core0 が転送中。入れ替え禁止
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_FRAME_CHANNEL_H
