// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 実機ファームのエントリ。
//
// M0 時点ではエミュレータ本体 (src/x68k/core/) がまだ無いので、ここでは
//   1. M5Unified が CoreS3 を初期化できること
//   2. メモリ予算 (内部 SRAM / PSRAM) の実測値
// だけを出す。2 は後続マイルストーンの設計を左右する数字なので、最初から測る。
//
// なぜメモリ実測を最初に置くか:
//   本プロジェクトの最大のリスクは「内部 SRAM に何を置けるか」。IPLROM 128KB と
//   X68000 メインメモリの低位側を内部 SRAM に載せられるかで速度が桁で変わる。
//   コードを書く前に上限を知っておきたい。

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdio.h>

namespace
{

// 内部 SRAM と PSRAM の空き状況を出す。
//
// largest free block を併記するのが要点。エミュレータは「1MB の連続領域」の
// ような大きな確保を行うので、free の合計だけ見ても意味がない。断片化していると
// free が数 MB あっても連続では取れない。
void reportMemory(const char* phase)
{
    printf("[mem:%s] internal free=%u largest=%u | psram free=%u largest=%u\n", phase,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    fflush(stdout);
}

}  // namespace

extern "C" void app_main(void)
{
    auto cfg = M5.config();
    M5.begin(cfg);

    reportMemory("after M5.begin");

    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.println("x68k-stackchan");
    M5.Display.setTextSize(1);
    M5.Display.printf("internal free: %u\n",
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    M5.Display.printf("psram free   : %u\n",
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    while (true)
    {
        M5.update();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
