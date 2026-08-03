// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 実機ファームのエントリ。CoreS3 上で X68000 を動かす。
//
// 起動の流れ:
//   M5.begin → メモリ先行予約 → SD マウント → ROM 読み込み → エミュレータ起動
//
// メモリ確保の順序が重要:
//   PSRAM は後から確保しようとすると断片化していて連続領域が取れない。
//   起動直後、他の何かが PSRAM を触る前に必要な分をまとめて押さえる。
//   ここを崩すと「free は数 MB あるのに largest が数 KB」という状態になり、
//   1MB のメインメモリが確保できなくなる。

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "display_lcd.h"
#include "input_touch.h"
#include "machine.h"
#include "storage_sd.h"
#include "video/cgrom_fallback.h"

namespace
{

constexpr char kTag[] = "x68k";

// エミュレータが必要とするメモリ。
constexpr std::size_t kMainRamBytes = x68k::kMainRamSize;    // 1MB
constexpr std::size_t kTextVramBytes = x68k::kTvramSize;     // 512KB
constexpr std::size_t kGraphicVramBytes = x68k::kTvramSize;  // 512KB
constexpr std::size_t kIplromBytes = x68k::kIplromSize;      // 128KB
constexpr std::size_t kCgromBytes = x68k::kCgromSize;        // 768KB
constexpr std::size_t kFrameBufferBytes =
    x68k_platform::DisplayLcd::kScreenWidth * x68k_platform::DisplayLcd::kScreenHeight * 2;

// 確保した領域。
x68k::u8* g_mainRam = nullptr;
x68k::u8* g_textVram = nullptr;
x68k::u8* g_graphicVram = nullptr;
x68k::u8* g_iplRom = nullptr;
x68k::u8* g_cgRom = nullptr;
x68k::u16* g_frameBuffer = nullptr;

x68k::Machine g_machine;
x68k_platform::SdDisk g_disk;
x68k_platform::DisplayLcd g_display;
x68k_platform::TouchKeyboard g_keyboard;

void reportMemory(const char* phase)
{
    ESP_LOGI(kTag, "[mem:%s] internal free=%u largest=%u | psram free=%u largest=%u", phase,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

// PSRAM と内部 SRAM から必要な領域をまとめて押さえる。
//
// IPL-ROM を内部 SRAM に置きたいのは、ブート中の命令フェッチがほぼ全部そこから
// 来るため。PSRAM は内部 SRAM の 1/10 以下の速度しか出ず、しかも 32KB の
// キャッシュを超えるアクセスは素の速度まで落ちる。取れなければ PSRAM へ
// フォールバックする (動くが遅くなる)。
x68k::u8* g_sasiBuffer = nullptr;

bool reserveMemory()
{
    reportMemory("before reserve");

    // 大きいものから順に PSRAM を取る。
    g_mainRam = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kMainRamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_textVram = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kTextVramBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_graphicVram = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kGraphicVramBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_cgRom = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kCgromBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_frameBuffer = static_cast<x68k::u16*>(
        heap_caps_calloc(1, kFrameBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    // SASI の転送バッファ (64KB 弱)。
    //
    // Machine に埋め込むと内部 SRAM の .bss が 88KB まで膨らみ、
    // すぐ下で IPL-ROM 128KB を内部 SRAM へ置こうとして失敗する。
    // SASI の転送は DMA の完了待ちで一気に流すだけで遅延に敏感ではない
    // ので、PSRAM に置いても実害が小さい。
    g_sasiBuffer = static_cast<x68k::u8*>(
        heap_caps_calloc(1, x68k::Machine::kSasiBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    // IPL-ROM は内部 SRAM を優先。ブート中のホットパスなので効果が大きい。
    g_iplRom = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kIplromBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (g_iplRom == nullptr)
    {
        ESP_LOGW(kTag, "IPL-ROM を内部 SRAM に置けません。PSRAM へ退避します (遅くなります)");
        g_iplRom = static_cast<x68k::u8*>(
            heap_caps_calloc(1, kIplromBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    else
    {
        ESP_LOGI(kTag, "IPL-ROM を内部 SRAM に配置しました");
    }

    reportMemory("after reserve");

    const bool ok = g_mainRam != nullptr && g_textVram != nullptr && g_iplRom != nullptr &&
                    g_frameBuffer != nullptr && g_sasiBuffer != nullptr;
    if (!ok)
    {
        ESP_LOGE(kTag, "メモリの確保に失敗しました");
    }
    return ok;
}

// SD から ROM を読む。IPL-ROM が無ければ起動できない。
bool loadRoms()
{
    const std::size_t iplSize =
        x68k_platform::loadFile(x68k_platform::kIplromPath, g_iplRom, kIplromBytes);
    if (iplSize == 0)
    {
        ESP_LOGE(kTag, "IPL-ROM を読めません: %s", x68k_platform::kIplromPath);
        return false;
    }
    ESP_LOGI(kTag, "IPL-ROM: %u バイト", static_cast<unsigned>(iplSize));

    // CGROM は無くても英数字は出せる (IPL-ROM 内の 6x12 ANK フォントを使う)。
    //
    // 代替を組み立てずに nullptr のまま進めると、IOCS は字形として 0 以外の
    // 「読めた値」を使い、画面がベタ塗りの矩形になって Human68k の出力が
    // 一切読めなくなる。ホスト側 (host/main.cpp) と同じ扱いに揃える。
    const bool hasCgromBuffer = g_cgRom != nullptr;
    const std::size_t cgSize =
        hasCgromBuffer ? x68k_platform::loadFile(x68k_platform::kCgromPath, g_cgRom, kCgromBytes)
                       : 0;
    bool cgromReady = cgSize > 0;
    if (cgromReady)
    {
        ESP_LOGI(kTag, "CGROM: %u バイト", static_cast<unsigned>(cgSize));
    }
    else if (hasCgromBuffer)
    {
        x68k::buildCgromFromIplRom(g_iplRom, g_cgRom);
        cgromReady = true;
        ESP_LOGW(kTag, "CGROM がありません。IPL-ROM 内蔵 6x12 ANK フォントで代替します");
    }
    else
    {
        ESP_LOGE(kTag, "CGROM 用の PSRAM を確保できませんでした。文字は表示できません");
    }

    if (!g_disk.open(x68k_platform::kHddPath))
    {
        ESP_LOGW(kTag, "HDD イメージを開けません: %s", x68k_platform::kHddPath);
    }

    x68k::MemoryMap memory;
    memory.mainRam = g_mainRam;
    memory.textVram = g_textVram;
    memory.graphicVram = g_graphicVram;
    memory.iplRom = g_iplRom;
    memory.cgRom = cgromReady ? g_cgRom : nullptr;
    g_machine.setMemory(memory);
    g_machine.setSasiBuffer(g_sasiBuffer);
    g_machine.setDisk(&g_disk);

    return true;
}

// エミュレーションを回すタスク。
//
// Core1 に固定するのは、システムタスク (Wi-Fi/lwIP など) が Core0 寄りに
// 配置されるため。エミュレーションのホットループを侵されたくない。
void emulatorTask(void* /*arg*/)
{
    // 1 回の run で進める量。1 フレーム (約 180,000 サイクル) より細かくして、
    // 画面更新と入力の応答が鈍くならないようにする。
    constexpr x68k::u32 kSliceCycles = 20000;

    while (true)
    {
        if (g_machine.isHalted())
        {
            ESP_LOGE(kTag, "停止しました。未実装命令 %04X", g_machine.haltedOpcode());
            x68k_platform::DisplayLcd::showMessage("HALTED", "unimplemented opcode");
            vTaskDelete(nullptr);
            return;
        }

        g_machine.run(kSliceCycles);

        // ウォッチドッグに殺されないよう必ず譲る。
        vTaskDelay(1);
    }
}

}  // namespace

extern "C" void app_main(void)
{
    auto cfg = M5.config();
    M5.begin(cfg);

    ESP_LOGI(kTag, "x68k-stackchan 起動");
    x68k_platform::DisplayLcd::showMessage("x68k-stackchan", "booting...");

    // 何より先にメモリを押さえる。後回しにすると断片化して取れなくなる。
    if (!reserveMemory())
    {
        x68k_platform::DisplayLcd::showMessage("MEMORY ERROR", "not enough PSRAM");
        return;
    }

    if (!x68k_platform::mountSd())
    {
        ESP_LOGE(kTag, "SD をマウントできません");
        x68k_platform::DisplayLcd::showMessage("NO SD CARD", "see NOTICE.md for ROM setup");
        return;
    }

    if (!loadRoms())
    {
        x68k_platform::DisplayLcd::showMessage("NO IPL-ROM", "/x68k/iplrom.dat is required");
        return;
    }

    g_machine.reset();
    ESP_LOGI(kTag, "リセット完了 PC=%08X", g_machine.cpu().state().pc - 4);

    g_display.begin(g_frameBuffer);
    // Human68k のコンソールは 768x512。左上から映す。
    g_display.setViewport(0, 0);
    g_keyboard.begin();

    // エミュレーションは Core1 で回す。
    xTaskCreatePinnedToCore(emulatorTask, "x68k", 8192, nullptr, 5, nullptr, 1);

    // Core0 は画面と入力を担当する。
    while (true)
    {
        M5.update();
        g_keyboard.poll(g_machine);
        g_display.present(g_machine, g_textVram);
        vTaskDelay(pdMS_TO_TICKS(16));  // 約 60Hz
    }
}
