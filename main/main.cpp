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
#include <driver/usb_serial_jtag.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstdio>

#include "display_lcd.h"
#include "input_touch.h"
#include "io/ascii_keymap.h"
#include "machine.h"
#include "storage_sd.h"
#include "video/cgrom_fallback.h"
#include "video/text_scrape.h"

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
    constexpr std::uint32_t kReportIntervalMs = 5000;

    std::uint64_t totalCycles = 0;
    std::uint64_t lastReportCycles = 0;
    std::uint32_t lastReportMs = 0;

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
        totalCycles += kSliceCycles;

        // 生きていることと実効クロックを定期的に出す。実機は画面を直接
        // 見られないので、止まったのか遅いだけなのかがログでしか分からない。
        const std::uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const bool isReportDue = now - lastReportMs >= kReportIntervalMs;
        if (isReportDue)
        {
            const std::uint32_t elapsed = now - lastReportMs;
            const unsigned khz =
                elapsed > 0 ? static_cast<unsigned>((totalCycles - lastReportCycles) / elapsed) : 0;
            ESP_LOGI(kTag, "%llu サイクル実行 (実効 %u kHz)",
                     static_cast<unsigned long long>(totalCycles), khz);
            lastReportMs = now;
            lastReportCycles = totalCycles;
        }

        // ウォッチドッグに殺されないよう必ず譲る。
        vTaskDelay(1);
    }
}

// シリアルからキーを受け、画面を ASCII でシリアルへ返すコンソール。
//
// 実機の LCD は 320x240 しかなく Human68k のコンソール (768x512) の
// 左上しか映らない。タッチキーボードも手で触らないと打てないので、
// 「実機で何が出ているか」「打った文字が通るか」を手元で確かめる術がない。
// USB シリアルを口にすればホストの --keys / --dump-text と同じことができる。
//
// Why not LCD の内容を画像で送るか: 768x512 のビットマップはシリアルには
// 大きすぎる。ASCII に逆引きすれば 1 行 96 バイトで済む。
class SerialConsole
{
public:
    // 打った文字は押下と解放を分けて送る。X68000 のキーボードは
    // 離した通知が来ないと押しっぱなしと見なす。
    void poll()
    {
        // USB Serial JTAG から直接読む。
        //
        // Why not getchar を使うか: stdin を非ブロッキングにしないと
        // 入力待ちで画面更新ごと止まる。fcntl で非ブロッキングにすると
        // 今度は VFS 側の口が塞がって、ログ出力まで巻き添えで止まった。
        // ドライバを直接叩けばどちらの問題も起きない。
        std::uint8_t buf[32];
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
        for (int i = 0; i < n; ++i)
        {
            enqueue(static_cast<char>(buf[i]));
        }

        drainQueue();
    }

private:
    // 打つ文字を溜めておく。
    //
    // 押下と解放を続けざまに送ると Human68k が取りこぼす。MFP のキーボード
    // 受信は 1 バイトずつしか保持できず、CPU が読み出す前に次を書くと
    // 前のものが消える。実効 3.8MHz では 1 バイト読むのに数フレームかかる。
    static constexpr std::size_t kQueueSize = 64;
    char queue_[kQueueSize] = {};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;

    // 押下を送ってから解放を送るまでに空ける回数。poll は約 16ms 周期。
    static constexpr int kFramesPerStep = 4;
    int framesLeft_ = 0;
    x68k::u8 pendingRelease_ = 0;

    void enqueue(char c)
    {
        const std::size_t next = (tail_ + 1) % kQueueSize;
        const bool isFull = next == head_;
        if (isFull)
        {
            return;
        }
        queue_[tail_] = c;
        tail_ = next;
    }

    void drainQueue()
    {
        if (framesLeft_ > 0)
        {
            --framesLeft_;
            return;
        }

        // 押下だけ送ってあるなら、先に解放を送る。
        const bool hasPendingRelease = pendingRelease_ != 0;
        if (hasPendingRelease)
        {
            g_machine.pressKey(static_cast<x68k::u8>(pendingRelease_ | 0x80u));
            pendingRelease_ = 0;
            framesLeft_ = kFramesPerStep;
            return;
        }

        const bool isEmpty = head_ == tail_;
        if (isEmpty)
        {
            return;
        }

        const char c = queue_[head_];
        head_ = (head_ + 1) % kQueueSize;

        // '~' で画面を出す。Human68k に渡す文字と衝突しないものを選んだ。
        const bool isDumpRequest = c == '~';
        if (isDumpRequest)
        {
            dumpScreen();
            return;
        }

        // '%' で LCD へ送っている画素をそのまま吸い出す。
        //
        // テキスト画面の逆引き ('~') は VRAM の中身しか見ないので、
        // 「VRAM は正しいのに LCD がおかしい」形の不具合を見つけられない。
        // ラスタライズ後の画素を見れば、パレット・拡大・転送のどこで
        // 崩れているかまで分かる。
        const bool isFrameDumpRequest = c == '%';
        if (isFrameDumpRequest)
        {
            dumpFrameBuffer();
            return;
        }

        // '!' で画面を消して描き直す。表示が崩れたときに、崩れが
        // 描画側に残っているのか LCD 側に残っているのかを切り分ける。
        const bool isRefreshRequest = c == '!';
        if (isRefreshRequest)
        {
            g_display.forceClear();
            ESP_LOGI(kTag, "画面を消去して描き直します");
            return;
        }

        // '<' '>' で拡大率を変える。読みやすさと見える範囲は
        // 引き換えなので、実機を見ながら選べるようにする。
        const bool isZoomOut = c == '<';
        const bool isZoomIn = c == '>';
        if (isZoomOut || isZoomIn)
        {
            const x68k::u32 next = isZoomIn ? g_display.zoom() + 1 : g_display.zoom() - 1;
            g_display.setZoom(next);
            ESP_LOGI(kTag, "拡大率 %u 倍 (%u 桁 x %u 行)", static_cast<unsigned>(g_display.zoom()),
                     static_cast<unsigned>(x68k_platform::DisplayLcd::kScreenWidth /
                                           g_display.zoom() / 8),
                     static_cast<unsigned>(x68k_platform::DisplayLcd::kScreenHeight /
                                           g_display.zoom() / 16));
            return;
        }

        const x68k::u8 code = x68k::asciiToScanCode(c);
        const bool isTypable = code != 0;
        if (!isTypable)
        {
            return;
        }

        g_machine.pressKey(code);
        pendingRelease_ = code;
        framesLeft_ = kFramesPerStep;
    }

    // LCD へ送っている RGB565 の画素をそのまま吐く。
    //
    // base64 で送る。生のバイナリだと ESP_LOG の行が混ざったときに
    // 区切りが分からなくなり、0x0A を含む画素が改行と区別できない。
    // 4/3 に膨らむが、150KB が 200KB になるだけで実用上は困らない。
    static void dumpFrameBuffer()
    {
        if (g_frameBuffer == nullptr)
        {
            return;
        }

        constexpr x68k::u32 kWidth = x68k_platform::DisplayLcd::kScreenWidth;
        constexpr x68k::u32 kHeight = x68k_platform::DisplayLcd::kScreenHeight;

        std::printf("---- フレームバッファ %ux%u RGB565 base64 ----\n",
                    static_cast<unsigned>(kWidth), static_cast<unsigned>(kHeight));

        static const char kBase64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(g_frameBuffer);
        const std::size_t total = static_cast<std::size_t>(kWidth) * kHeight * 2;

        // 3 バイトを 4 文字へ。行が長すぎると受け側の取りこぼしが増えるので
        // 76 文字で折る。
        char line[80];
        std::size_t col = 0;
        for (std::size_t i = 0; i < total; i += 3)
        {
            const std::uint32_t b0 = bytes[i];
            const std::uint32_t b1 = (i + 1 < total) ? bytes[i + 1] : 0;
            const std::uint32_t b2 = (i + 2 < total) ? bytes[i + 2] : 0;
            const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

            line[col++] = kBase64[(triple >> 18) & 0x3F];
            line[col++] = kBase64[(triple >> 12) & 0x3F];
            line[col++] = (i + 1 < total) ? kBase64[(triple >> 6) & 0x3F] : '=';
            line[col++] = (i + 2 < total) ? kBase64[triple & 0x3F] : '=';

            if (col >= 76)
            {
                line[col] = '\0';
                std::printf("%s\n", line);
                col = 0;
                // USB CDC の送信バッファを詰まらせない。詰まると
                // 画面更新のループごと止まる。
                vTaskDelay(1);
            }
        }
        if (col > 0)
        {
            line[col] = '\0';
            std::printf("%s\n", line);
        }

        std::printf("---- フレームバッファここまで ----\n");
        std::fflush(stdout);
    }

    static void dumpScreen()
    {
        if (g_textVram == nullptr || g_cgRom == nullptr)
        {
            return;
        }

        // ESP_LOG ではなく printf を使う。ログはタイムスタンプとタグが
        // 行頭に付いて画面の形が崩れる。
        std::printf("---- テキスト画面 ----\n");
        char line[x68k::TextScrape::kColumns + 1];
        for (x68k::u32 row = 0; row < x68k::TextScrape::kRows; ++row)
        {
            x68k::TextScrape::readRow(g_textVram, g_cgRom, row, line);
            std::printf("%2u|%s\n", static_cast<unsigned>(row), line);
        }
        std::printf("----------------------\n");
        std::fflush(stdout);
    }
};

SerialConsole g_console;

// 書き込みが進んだ先へ表示範囲を追わせる。
//
// 拡大すると 20 桁 x 7 行しか映らず、Human68k の 96 桁 x 32 行の左上しか
// 見えない。プロンプトは左上にあるので起動直後は問題ないが、行が伸びたり
// 画面が下へ進んだりすると、打っている場所が窓の外へ出てしまう。
//
// Why not Human68k のカーソル位置ワークを読むか: その番地は OS の
// バージョンに依存する。VRAM から「最後に文字が書かれた位置」を求めれば
// 内部構造を知らなくても済み、ホストでも同じ判定が使える。
void followCursor()
{
    if (g_textVram == nullptr)
    {
        return;
    }

    // 画面全体の走査は 96x32 セル分あって毎フレームには重い。
    // 追従が数フレーム遅れても操作感は変わらない。
    static int skip = 0;
    if (++skip < 8)
    {
        return;
    }
    skip = 0;

    const x68k::u32 row = x68k::TextScrape::lastUsedRow(g_textVram);
    const x68k::u32 column = x68k::TextScrape::lastUsedColumn(g_textVram, row);

    // 窓に入る桁数・行数。
    const x68k::u32 zoom = g_display.zoom();
    const x68k::u32 cols = x68k_platform::DisplayLcd::kScreenWidth / zoom / 8;
    const x68k::u32 rows = x68k_platform::DisplayLcd::kScreenHeight / zoom / 16;

    // 書いている位置が右端・下端に来るように寄せる。行頭へ戻ったら
    // 左端まで戻す。1 文字ぶん余白を持たせてカーソルが端に貼り付かない
    // ようにする。
    const x68k::u32 marginCols = 1;
    const x68k::u32 wantCols = column + marginCols + 1;
    const x68k::u32 x = wantCols > cols ? (wantCols - cols) * 8 : 0;
    const x68k::u32 wantRows = row + 1;
    const x68k::u32 y = wantRows > rows ? (wantRows - rows) * 16 : 0;

    g_display.setViewport(x, y);
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
    //
    // 等倍だと 8x16 の文字が 2 インチの画面にそのまま出て読み取れない。
    // 2 倍にすると 20 桁 x 7 行と狭くなるが、文字として判別できる。
    g_display.setViewport(0, 0);
    g_keyboard.begin();

    // シリアルからキーを拾えるようにする。
    //
    // ログ出力は VFS 経由のままにして、入力だけドライバから直接読む。
    // 両方をドライバに寄せると ESP_LOG の行が混ざって読めなくなる。
    usb_serial_jtag_driver_config_t usbCfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const bool isConsoleReady = usb_serial_jtag_driver_install(&usbCfg) == ESP_OK;
    if (isConsoleReady)
    {
        ESP_LOGI(kTag, "シリアルコンソール: 文字を打つと X68000 へ、'~' で画面をダンプ");
    }
    else
    {
        ESP_LOGW(kTag, "シリアルコンソールを開けませんでした");
    }

    // エミュレーションは Core1 で回す。
    xTaskCreatePinnedToCore(emulatorTask, "x68k", 8192, nullptr, 5, nullptr, 1);

    // Core0 は画面と入力を担当する。
    while (true)
    {
        M5.update();
        g_keyboard.poll(g_machine);
        g_console.poll();
        followCursor();
        g_display.present(g_machine, g_textVram);
        vTaskDelay(pdMS_TO_TICKS(16));  // 約 60Hz
    }
}
