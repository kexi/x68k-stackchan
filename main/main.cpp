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

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "display_lcd.h"
#include "frame_channel.h"
#include "input_touch.h"
#include "key_queue.h"
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
// フレームバッファは 2 枚。エミュレーションコアが片方へ変換している間に、
// 表示コアがもう片方を LCD へ送る。
x68k::u16* g_frameBufferA = nullptr;
x68k::u16* g_frameBufferB = nullptr;

x68k::Machine g_machine;
x68k_platform::SdDisk g_disk;
x68k_platform::DisplayLcd g_display;
x68k_platform::TouchKeyboard g_keyboard;
x68k_platform::FrameChannel g_frames;
x68k_platform::KeyQueue g_keys;

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
    g_frameBufferA = static_cast<x68k::u16*>(
        heap_caps_calloc(1, kFrameBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_frameBufferB = static_cast<x68k::u16*>(
        heap_caps_calloc(1, kFrameBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    // SASI の転送バッファ (64KiB)。
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

    // 確保できたかを確かめる。
    //
    // グラフィック VRAM は外してある。今の表示はテキスト VRAM しか使わず、
    // バスに null チェックがあるので (読むと 0、書きは捨てる)、取れなくても
    // コンソールは出せる。グラフィック画面を実装したら必須に加える。
    //
    // CGROM は必須にする。バスの null チェックで落ちはしないが、字形が
    // 1 つも無ければ画面が真っ黒のままで、起動しても何もできない。
    // 768KB が取れない時点で PSRAM の断片化が起きており、そのまま進んでも
    // 先で別の失敗を招く。
    const bool ok = g_mainRam != nullptr && g_textVram != nullptr && g_iplRom != nullptr &&
                    g_cgRom != nullptr && g_frameBufferA != nullptr && g_frameBufferB != nullptr &&
                    g_sasiBuffer != nullptr;
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
    // 長さまで確かめる。
    //
    // loadFile はバッファに収まる限り任意の長さを成功として返す。
    // 途中で切れた IPL-ROM を受け入れると、読めなかった部分は calloc の
    // ゼロのままになり、リセットベクタが 0 を指して PC=0 で暴走する。
    // 「ROM が壊れている」と分かる形で止める方がずっとよい。
    const bool isIplComplete = iplSize == kIplromBytes;
    if (!isIplComplete)
    {
        ESP_LOGE(kTag, "IPL-ROM が %u バイトです (%u バイト必要): %s",
                 static_cast<unsigned>(iplSize), static_cast<unsigned>(kIplromBytes),
                 x68k_platform::kIplromPath);
        return false;
    }
    ESP_LOGI(kTag, "IPL-ROM: %u バイト", static_cast<unsigned>(iplSize));

    // CGROM のファイルは無くてもよい。IPL-ROM 内の 6x12 ANK フォントで
    // 代替すれば英数字は出せる。
    //
    // 代替を組み立てずに nullptr のまま進めると、IOCS は字形として 0 以外の
    // 「読めた値」を使い、画面がベタ塗りの矩形になって Human68k の出力が
    // 一切読めなくなる。ホスト側 (host/main.cpp) と同じ扱いに揃える。
    //
    // バッファ自体は reserveMemory で確保済み (取れなければそこで止まる)。
    const std::size_t cgSize =
        x68k_platform::loadFile(x68k_platform::kCgromPath, g_cgRom, kCgromBytes);

    // 長さも見る。短いものを受け入れると大半の字形が欠け、「表示は出るが
    // 読めない」という切り分けにくい状態になる。合わなければ代替へ落とす。
    const bool isCgromComplete = cgSize == kCgromBytes;
    if (cgSize > 0 && !isCgromComplete)
    {
        ESP_LOGW(kTag, "CGROM が %u バイトです (%u バイト必要)。代替を使います",
                 static_cast<unsigned>(cgSize), static_cast<unsigned>(kCgromBytes));
    }
    if (isCgromComplete)
    {
        ESP_LOGI(kTag, "CGROM: %u バイト", static_cast<unsigned>(cgSize));
    }
    else
    {
        x68k::buildCgromFromIplRom(g_iplRom, g_cgRom);
        ESP_LOGW(kTag, "CGROM がありません。IPL-ROM 内蔵 6x12 ANK フォントで代替します");
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
    memory.cgRom = g_cgRom;
    g_machine.setMemory(memory);
    g_machine.setSasiBuffer(g_sasiBuffer);
    g_machine.setDisk(&g_disk);

    return true;
}

// エミュレーションコアから使うが、定義は下にある。
void followCursor();
void dumpScreen();

// シリアルから画面ダンプを求められたら立つ。
//
// Why not SerialConsole の中に持たせるか: そうすると SerialConsole を
// エミュレーションタスクより前に定義する必要が出て、宣言順が窮屈になる。
// フラグ 1 つなら外に置いた方が読みやすい。
//
// 複数コアから触るので atomic にする。取りこぼしは「もう一度打てばよい」
// で済むが、未定義動作は避けたい。
std::atomic<bool> g_dumpRequested{false};

// 使ってほしい拡大率。表示コアが書き、エミュレーションコアが読む。
//
// Why not 表示コアから直接 setZoom を呼ぶか: zoom_ は変換中に何度も
// 読まれる。renderZoomed は zoom_ から srcHeight を決めた後、書き込み先の
// 行を y * zoom_ で計算する。途中で zoom_ が増えるとバッファの外へ書く
// (240 行のバッファに対し、2 倍→4 倍の変化で 476 行目まで届く)。
// 変換の所有者に変えさせれば、変換中に動かない。
//
// Why not 「要求」として消費する形にするか: 一度そう書いたが、
// エミュレーションコアが要求を取り出してから現在値を書き戻すまでの間に
// 表示コアが次の要求を作ると、古い現在値を基準にしてしまい 1 打ぶん消える。
// 「今どうあってほしいか」を持ち続ける形なら、読む側は現在値と比べるだけで
// 済み、取り出しと書き戻しの隙間が生まれない。
std::atomic<x68k::u32> g_desiredZoom{1};

// エミュレーションが停止した。表示コアがメッセージを出す。
//
// Why not エミュレーションコアから showMessage を呼ぶか: M5.Display を
// 両コアから触ることになる。表示コアは同時に pushFrame を走らせているので、
// SPI の操作が競合するうえ、公開済みの古いフレームがエラー表示を
// 上書きしてしまう。
std::atomic<bool> g_halted{false};

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
            // 表示は所有者に任せる。ここで M5.Display を触ると
            // 表示コアの転送とぶつかる。
            g_halted = true;
            vTaskDelete(nullptr);
            return;
        }

        g_machine.run(kSliceCycles);
        totalCycles += kSliceCycles;

        // 溜まったキーを MFP へ流す。押下と解放の間隔は KeyQueue が持つ。
        g_keys.drain(g_machine);

        // シリアルから画面ダンプを求められていたらここで出す。
        // テキスト VRAM の所有者はこのコア。
        if (g_dumpRequested.exchange(false))
        {
            dumpScreen();
        }

        // 希望の拡大率が今と違えば、変換の前に合わせる。
        if (const x68k::u32 desired = g_desiredZoom.load(); desired != g_display.zoom())
        {
            g_display.setZoom(desired);
            ESP_LOGI(kTag, "拡大率 %u 倍 (%u 桁 x %u 行)", static_cast<unsigned>(g_display.zoom()),
                     static_cast<unsigned>(x68k_platform::DisplayLcd::kScreenWidth /
                                           g_display.zoom() / 8),
                     static_cast<unsigned>(x68k_platform::DisplayLcd::kScreenHeight /
                                           g_display.zoom() / 16));
        }

        // 表示位置をカーソルへ追わせてから、変化があれば変換する。
        //
        // Machine を読むのはこのコアだけ。表示コアへは完成した RGB565 を
        // 渡すので、テキスト VRAM やダーティフラグを両コアで奪い合わない。
        followCursor();
        const bool rendered = g_display.renderTo(g_machine, g_textVram, g_frames.writeBuffer());
        if (rendered && !g_frames.publish())
        {
            // 表示コアがまだ前のフレームを転送中で渡せなかった。
            //
            // renderTo はダーティフラグを消した後なので、このままだと
            // 次に VRAM が変わるまで画面が更新されない。作ったフレームは
            // 捨てられたのに「描いた」ことになってしまう。
            // 次のスライスで描き直させる。
            g_display.invalidateAll();
        }

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
// このクラスは表示コアで動く。Machine には触らず、打たれた文字は
// KeyQueue へ積むだけ。画面のダンプもテキスト VRAM を読む必要があるので、
// 要求を立ててエミュレーションコアに実行させる。
class SerialConsole
{
public:
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
            handleChar(static_cast<char>(buf[i]));
        }
    }

private:
    void handleChar(char c)
    {
        // '~' で画面を出す。Human68k に渡す文字と衝突しないものを選んだ。
        // Why not ここで出さないか: テキスト VRAM を読むので、
        // エミュレーションコアが書いている最中に読むとデータ競合になる。
        // 要求だけ立てて、読むのは所有者に任せる。
        const bool isDumpRequest = c == '~';
        if (isDumpRequest)
        {
            g_dumpRequested = true;
            return;
        }

        // '<' '>' で拡大率を変える。読みやすさと見える範囲は
        // 引き換えなので、実機を見ながら選べるようにする。
        //
        // 目標値を立てるだけ。実際に変えるのはエミュレーションコア。
        // 変換の途中で拡大率が動くとバッファの外へ書きうる。
        const bool isZoomOut = c == '<';
        const bool isZoomIn = c == '>';
        if (isZoomOut || isZoomIn)
        {
            // 希望値を 1 段階動かす。まだ反映されていなくても、
            // 希望値そのものを持ち続けているので取りこぼさない。
            //
            // ここで上下限に収める。反映側に任せると、4 倍の状態で
            // '>>><<<' と打ったとき希望値が 7 まで伸びて戻る途中で
            // 頭打ちになり、打った回数どおりに縮まない。
            const x68k::u32 base = g_desiredZoom.load();
            const bool canZoomIn = base < x68k_platform::DisplayLcd::kMaxZoom;
            const bool canZoomOut = base > x68k_platform::DisplayLcd::kMinZoom;
            if (isZoomIn && canZoomIn)
            {
                g_desiredZoom = base + 1;
            }
            else if (isZoomOut && canZoomOut)
            {
                g_desiredZoom = base - 1;
            }
            return;
        }

        // 残りは X68000 へ。押下と解放に分ける仕事は KeyQueue が持つ。
        g_keys.push(c);
    }
};

// テキスト画面を ASCII に逆引きしてシリアルへ出す。
//
// テキスト VRAM を読むので、エミュレーションコアから呼ぶ。
void dumpScreen()
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

    g_display.begin();

    // フレームの受け渡しとキューを用意する。
    //
    // これが無いと、エミュレーションコアが画面を作れずキーも届かない。
    // 起動できないので、失敗したらここで止める。
    if (!g_frames.begin(g_frameBufferA, g_frameBufferB))
    {
        ESP_LOGE(kTag, "フレームの受け渡しを用意できません");
        x68k_platform::DisplayLcd::showMessage("INIT ERROR", "frame channel");
        return;
    }
    if (!g_keys.begin())
    {
        ESP_LOGE(kTag, "キューを用意できません");
        x68k_platform::DisplayLcd::showMessage("INIT ERROR", "key queue");
        return;
    }

    // Human68k のコンソールは 768x512。左上から映す。
    //
    // 等倍だと 8x16 の文字が 2 インチの画面にそのまま出て読み取れない。
    // 2 倍にすると 20 桁 x 7 行と狭くなるが、文字として判別できる。
    g_display.setViewport(0, 0);
    // タッチキーボードは今は出さない。
    //
    // Why: 毎フレーム 320x240 の全面を送るので、描いても即座に消える。
    // 見えないのに触ると反応する状態は、意図しない文字が入るだけで害しかない。
    // 使うなら、フレームの下部を空けて描くか、キーボード領域だけ別に
    // 転送する仕組みが要る。今はシリアルから打てるので急がない。
    g_keyboard.setVisible(false);
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
    // 失敗を見逃さない。
    //
    // ここまでに PSRAM を数 MB と内部 SRAM を 128KB 確保しているので、
    // スタック用の内部メモリが足りずに失敗することは現実に起こりうる。
    // 見逃すと Core0 の表示・入力ループだけが回り続け、エミュレータは
    // 1 命令も実行されない。利用者にはフリーズとしか見えない。
    const BaseType_t taskCreated =
        xTaskCreatePinnedToCore(emulatorTask, "x68k", 8192, nullptr, 5, nullptr, 1);
    if (taskCreated != pdPASS)
    {
        ESP_LOGE(kTag, "エミュレーションタスクを作れません (内部メモリ不足)");
        x68k_platform::DisplayLcd::showMessage("TASK ERROR", "cannot start emulator");
        return;
    }

    // Core0 は画面と入力を担当する。
    bool isHalted = false;
    while (true)
    {
        M5.update();
        g_keyboard.poll(g_keys);
        g_console.poll();

        // エミュレーションが止まったらメッセージを出して、以後は
        // フレームを送らない。送ると公開済みの古い画面でエラー表示を
        // 上書きしてしまう。
        if (g_halted.exchange(false))
        {
            x68k_platform::DisplayLcd::showMessage("HALTED", "unimplemented opcode");
            isHalted = true;
        }
        if (isHalted)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 変換済みのフレームがあれば LCD へ送る。
        //
        // Machine には一切触らない。触るのはエミュレーションコアだけで、
        // ここへは完成した RGB565 が渡ってくる。
        if (x68k::u16* frame = g_frames.take(); frame != nullptr)
        {
            g_display.pushFrame(frame);
            g_frames.done();
        }

        vTaskDelay(pdMS_TO_TICKS(16));  // 約 60Hz
    }
}
