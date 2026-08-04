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

#include "app_mode.h"
#include "avatar.h"
#include "display_lcd.h"
#include "frame_channel.h"
#include "input_touch.h"
#include "key_queue.h"
#include "servo.h"
#include "io/ascii_keymap.h"
#include "machine.h"
#include "storage_sd.h"
#include "video/cgrom_fallback.h"
#include "video/text_scrape.h"

namespace
{

constexpr char kTag[] = "x68k";

// エミュレータが必要とするメモリ。
constexpr std::size_t kMainRamBytes = x68k::kMainRamSize;    // 2MB
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
x68k_platform::MouseQueue g_mouse;

// --- スタックチャン (顔 ⇄ X68000) ---
//
// FSM を触るのは表示コア (Core0) だけ。エミュレーションコアへは下の
// atomic 1 つだけを渡す (app_mode.h の AppModeMachine のコメントを見よ)。
x68k_platform::AppModeMachine g_mode;
x68k_platform::Avatar g_avatar;

// サーボは付いていない前提。NullServo は指示を捨てる (servo.h を見よ)。
// 実物を付けるときはここを差し替える。
x68k_platform::NullServo g_servo;

// 1 スライスで進めてよいサイクル数。表示コアが書き、エミュレーションコアが読む。
//
// 0 は「顔モードで止める」(EmulationPolicy::Paused)。
//
// Why not エミュレーションコアから g_mode を読ませないか: AppModeMachine は
// mode_ と policy_ の 2 つを持ち、遷移はその両方にまたがる。ロック無しで
// 別コアから読むと、切り替えの途中の食い違った組を見うる。表示コアが
// 決めた結果を 1 つの値へ落としてから渡せば、読む側は組の一貫性を
// 気にせずに済む (g_desiredZoom が同じ形をしている)。
std::atomic<x68k::u32> g_allowedSliceCycles{0};

// 1 回の run で進める量 (X68K モードでの値)。1 フレーム (約 180,000 サイクル)
// より細かくして、画面更新と入力の応答が鈍くならないようにする。
//
// Why not emulatorTask の中に置いたままにしないか: 表示コアが
// AppModeMachine::sliceCycles(kSliceCycles) を呼んで g_allowedSliceCycles を
// 決めるので、両方のコアから見える必要がある。2 か所に書くと、
// 片方だけ変えたときに顔モードとの比が意図せず変わる。
constexpr x68k::u32 kSliceCycles = 20000;

// X68000 へ戻ったので画面を作り直してほしい。表示コアが立て、
// エミュレーションコアが消す。
//
// Why not 表示コアから DisplayLcd::invalidateAll を直接呼ばないか:
// forceFullRedraw_ を読んで消すのは renderTo で、それはエミュレーション
// コアが回している (display_lcd.h が「エミュレーションコアから呼ぶ」と
// 明記している)。表示コアから立てると、renderTo がフラグを消す瞬間と
// ぶつかって全画面の描き直しが 1 回消える。顔の残った画面が次に VRAM が
// 変わるまで残ってしまう。要求だけ立てて、実行は所有者に任せる
// (g_dumpRequested と同じ形)。
std::atomic<bool> g_redrawRequested{false};

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

// 顔のスプライト。無くても起動する (仮の顔は M5.Display へ直接描く)。
x68k::u16* g_avatarSprite = nullptr;

bool reserveMemory()
{
    reportMemory("before reserve");

    // 必須のものを先に取る。
    //
    // グラフィック VRAM は無くてもコンソールは出せる (下で最後に取る)。
    // 先に 512KB を消費すると、必須のものだけなら収まる状況でも
    // そちらが失敗して起動を中止することになる。
    g_mainRam = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kMainRamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_textVram = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kTextVramBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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

    // 顔のスプライト (320x240 の RGB565 = 150KB)。
    //
    // ここで押さえるのは、後から取ると PSRAM が断片化していて連続領域が
    // 取れないため (このファイル冒頭の方針)。顔を最初に出すのは
    // 切り替えた後なので「使うときに取る」でも動きそうに見えるが、
    // そのときには 2MB のメインメモリと CGROM が既に PSRAM を分断している。
    //
    // 取れなくても起動する。仮の実装は M5.Display へ直接描いており
    // (avatar.cpp)、スプライトが無くても顔は出る。本物の Avatar へ
    // 差し替えたときに要るぶんを、今の段階から実測で押さえておく。
    g_avatarSprite = static_cast<x68k::u16*>(heap_caps_calloc(
        1, x68k_platform::Avatar::kSpriteBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_avatarSprite == nullptr)
    {
        ESP_LOGW(kTag, "顔のスプライトを確保できません。仮の顔は出ますが Avatar は載せられません");
    }

    // グラフィック VRAM は最後。取れなくても起動する。
    //
    // 取れれば SX-Window のようにグラフィック面を使うソフトが動く。
    // 取れなくてもバスに null チェックがあり (読むと 0、書きは捨てる)、
    // 表示側も nullptr ならテキストだけを描くので、コンソールは使える。
    g_graphicVram = static_cast<x68k::u8*>(
        heap_caps_calloc(1, kGraphicVramBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_graphicVram == nullptr)
    {
        ESP_LOGW(kTag, "グラフィック VRAM を確保できません。テキスト画面のみで続けます");
    }

    reportMemory("after reserve");

    // 確保できたかを確かめる。
    //
    // グラフィック VRAM は外してある。バスに null チェックがあり
    // (読むと 0、書きは捨てる)、表示側も nullptr ならテキストだけを
    // 描くので、取れなくてもコンソールは出せる。
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

// シリアルコンソール (SerialConsole) から使うが、定義は下にある。
// FSM の決定を周辺へ反映する。
void applyModeTransition(const x68k_platform::ModeTransition& transition);

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
    constexpr std::uint32_t kReportIntervalMs = 5000;

    // SRAM を SD へ書き戻す間隔。
    //
    // Why not 書き込みのたびに保存するか: Human68k は設定を数バイトずつ書くので、
    // 1 バイトごとに 16KB を書くと SD の同じセクタを何百回も潰す。
    // Why not 終了時にまとめて保存するか: 組み込みに「終了」は無い。
    // 利用者は電源を切るだけなので、書き戻す機会がそこにしかないと必ず失われる。
    // 10 秒なら、間引きとしては十分で、失うのは直前の 1 回ぶんの設定変更で済む。
    constexpr std::uint32_t kSramSaveIntervalMs = 10000;

    std::uint64_t totalCycles = 0;
    std::uint64_t lastReportCycles = 0;
    std::uint32_t lastReportMs = 0;
    std::uint32_t lastSramSaveMs = 0;

    while (true)
    {
        if (g_machine.isHalted())
        {
            ESP_LOGE(kTag, "停止しました。未実装命令 %04X", g_machine.haltedOpcode());
            // 止まる前に SRAM を書き戻す。
            //
            // Why not 定期保存だけに任せるか: 保存は 10 秒間隔で間引いている。
            // 設定を書き換えた直後に未実装命令を踏むと、ここでタスクごと
            // 消えるので次の保存機会が永久に来ない。エミュレータが止まる
            // 原因は未実装命令であって SD ではないから、書き戻しは通る。
            x68k_platform::saveSramIfDirty(g_machine);
            // 表示は所有者に任せる。ここで M5.Display を触ると
            // 表示コアの転送とぶつかる。
            g_halted = true;
            vTaskDelete(nullptr);
            return;
        }

        // 顔モードの扱いは表示コアが決めて、進めてよい量だけを渡してくる。
        //
        // 既定 (KeepRunning) では kSliceCycles がそのまま来るので、
        // ここは X68K モードと変わらない。Throttled なら細く、
        // Paused なら 0 (run を飛ばす)。
        //
        // Why not 0 のときに長く眠らないか: このループは run 以外にも
        // SRAM の書き戻しと停止の検出を持つ。眠りを伸ばすとそちらの
        // 反応まで鈍る。停止中は下の vTaskDelay(1) を毎周回すので、
        // 止めている間の CPU は run を飛ばすだけで十分に空く。
        const x68k::u32 sliceCycles = g_allowedSliceCycles.load();
        if (sliceCycles > 0)
        {
            g_machine.run(sliceCycles);
            totalCycles += sliceCycles;
        }

        // 溜まったキーを MFP へ流す。押下と解放の間隔は KeyQueue が持つ。
        g_keys.drain(g_machine);

        // 溜まったマウスの動きを SCC へ流す。
        //
        // キーと同じくこのコアから送る。SCC の受信 FIFO と割り込み保留は
        // 割り込み処理がここで触るので、表示コアから書くと競合する。
        g_mouse.drain(g_machine);

        // シリアルから画面ダンプを求められていたらここで出す。
        // テキスト VRAM の所有者はこのコア。
        if (g_dumpRequested.exchange(false))
        {
            dumpScreen();
        }

        // X68000 へ戻ったなら全画面を作り直す。顔が描いた内容が
        // 残っているので、ダーティ行だけでは消えない。
        if (g_redrawRequested.exchange(false))
        {
            g_display.invalidateAll();
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

        // 変更された SRAM を SD へ書き戻す。
        //
        // ここで行うのは、SRAM を触ってよいのがこのコアだけだから。表示コアから
        // 呼ぶと、エミュレーションが書いている最中の 16KB を読むことになる。
        const bool isSramSaveDue = now - lastSramSaveMs >= kSramSaveIntervalMs;
        if (isSramSaveDue)
        {
            lastSramSaveMs = now;
            if (x68k_platform::saveSramIfDirty(g_machine))
            {
                ESP_LOGI(kTag, "SRAM を保存しました: %s", x68k_platform::kSramPath);
            }
        }

        // ウォッチドッグに殺されないよう定期的に譲る。
        //
        // 毎スライス譲ると、1 tick (1ms) が 1 スライスの実時間 (実測 6.1ms)
        // に対して無視できない。実測では毎回譲ると 2760kHz、8 スライスに
        // 1 回にすると 3200kHz で、譲る回数を減らした分がそのまま速度になる。
        //
        // Why not 譲るのをやめるか: docs/knowledge/cores3-emulator-runtime.md に
        // ある通り、taskYIELD() で済ませようとしてリセットループに入った。
        // アイドルタスクは優先度 0 なので、ブロックしない限り回ってこない。
        // ESP-IDF のタスクウォッチドッグはアイドルタスクが 5 秒走らないことを
        // 見て落とすので、譲る口自体は必ず残す必要がある。
        //
        // Why 8 か: 譲る間隔 = 8 スライス x 6.1ms ≒ 49ms。ウォッチドッグの
        // 5 秒に対して 100 倍の余裕がある。合成 ON で変換が最も重いとき
        // (実測 23.5ms/スライス) でも 235ms で、まだ 21 倍の余裕が残る。
        // これ以上伸ばしても速度は頭打ちで (N=16 で +1.5% の計算)、
        // 余裕を削るだけなので割に合わない。
        //
        // Why not 「N スライスごと」ではなく経過時間で判断しないか:
        // 時間を測るには esp_timer を毎周読むことになる。譲る条件が
        // 数え上げで足りるうちは、余計な計測を入れない方がホットループが軽い。
        constexpr int kSlicesPerYield = 8;
        static int slicesSinceYield = 0;

        // 停止中 (sliceCycles == 0) は毎周譲る。
        //
        // Why: 停止中は run を飛ばすのでループが一瞬で回りきる。数え上げだけで
        // 間引くと、8 周回るのに掛かる時間がほぼ 0 になり、譲らないまま
        // 空回りし続ける。それは taskYIELD() で踏んだのと同じ
        // 「アイドルタスクが回らない」状態で、ウォッチドッグに落とされる。
        const bool isPaused = sliceCycles == 0;
        const bool isYieldDue = ++slicesSinceYield >= kSlicesPerYield;
        if (isPaused || isYieldDue)
        {
            slicesSinceYield = 0;
            vTaskDelay(1);
        }
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

        // Tab で顔 ⇄ X68000 を切り替える。
        //
        // Why not 印字できる文字を使わないか: 顔モードでも X68K モードでも
        // 押せる必要があるが、X68K モードでは打った文字が Human68k へ
        // 流れる。'f' のような文字を充てると、コマンドを打つたびに
        // モードが変わる。Tab は Human68k のプロンプトで使い道が薄く、
        // 取り上げても困らない。
        //
        // Why not タッチで切り替えないか: 画面全体が X68000 のマウス領域に
        // なっている (main.cpp の setVisible(false) を見よ)。切り替え用の
        // 領域を切ると、そこだけカーソルが動かせなくなる。物理ボタンの
        // 無い CoreS3 でこれを解くには、長押しやジェスチャの判定が要る。
        // まずシリアルから切り替えられれば、FSM と入力の遮断は確かめられる。
        const bool isModeToggle = c == '\t';
        if (isModeToggle)
        {
            applyModeTransition(g_mode.request(x68k_platform::ModeRequest::Toggle));
            return;
        }

        // 顔モードの間は X68000 へ文字を送らない。
        //
        // タッチと揃える。顔を出している間に打った文字が溜まって、
        // 戻った瞬間に一気に流れ込むのを防ぐ。
        if (!g_mode.isX68kInputEnabled())
        {
            return;
        }

        // 残りは X68000 へ。押下と解放に分ける仕事は KeyQueue が持つ。
        g_keys.push(c);
    }
};

// FSM の決定を実際の周辺へ反映する。表示コアから呼ぶ。
//
// Why not AppModeMachine の中でやらないか: ここで触るのは M5.Display と
// MouseQueue と atomic で、どれも ESP-IDF に依存する。FSM に持たせると
// ホストのテストから外れる。FSM は「何をすべきか」を値で返すだけにして、
// 実行はこの関数に集める (app_mode.h の ModeTransition のコメントを見よ)。
void applyModeTransition(const x68k_platform::ModeTransition& transition)
{
    // 進めてよい量を更新する。モードが変わっていなくても、ポリシーの
    // 差し替え直後に呼ばれることがあるので毎回書く。
    g_allowedSliceCycles = g_mode.sliceCycles(kSliceCycles);

    // 入力の経路を開け閉めする。顔モードではタッチが X68000 へ届かない。
    g_keyboard.setX68kInputEnabled(g_mode.isX68kInputEnabled());

    if (!transition.changed)
    {
        return;
    }

    // 押したまま切り替えた指のボタンを離しておく。
    //
    // 顔モードの間タッチは届かないので、押しっぱなしのままだと
    // ゲストはボタンが押されたままと見なし続ける。
    if (transition.shouldReleaseMouseButtons)
    {
        g_mouse.push(0, 0, false, false);
    }

    if (transition.to == x68k_platform::AppMode::Face)
    {
        // 【仮】placeholder の顔を描く (avatar.h を見よ)。
        g_avatar.draw();
        // サーボが付いていれば正面へ戻す。NullServo は捨てる。
        g_servo.setPose({});
    }
    else
    {
        // X68000 へ戻る。顔が描いた内容が画面に残っているので、
        // ダーティ行だけを送る通常の経路では消えない。全画面を
        // 作り直させる (実行はエミュレーションコア)。
        if (transition.shouldRedraw)
        {
            g_redrawRequested = true;
        }
        // 首の保持をやめる。X68000 を触っている間サーボに力を入れ続けると
        // 電流を食い、安物のサーボは唸りが出る。
        g_servo.detach();
    }

    ESP_LOGI(kTag, "モード: %s", transition.to == x68k_platform::AppMode::Face ? "顔" : "X68000");
}

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

    // 前回の SRAM を復元する。reset() より先に行う。
    //
    // 実機の SRAM はバッテリバックアップで電源を切っても残る。起動デバイスや
    // 画面モードの設定はここにあるので、復元しないと毎回工場出荷値で立ち上がる。
    //
    // 初回起動では sram.dat が無いので false になるが、これは異常ではない。
    // Sram の構築時に工場出荷値が入っており、10 秒後の書き戻しで生成される。
    if (x68k_platform::loadSram(g_machine))
    {
        ESP_LOGI(kTag, "SRAM を復元しました: %s", x68k_platform::kSramPath);
    }
    else
    {
        ESP_LOGI(kTag, "SRAM を復元できません。工場出荷値で起動します: %s",
                 x68k_platform::kSramPath);
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
    if (!g_mouse.begin())
    {
        ESP_LOGE(kTag, "マウスのキューを用意できません");
        x68k_platform::DisplayLcd::showMessage("INIT ERROR", "mouse queue");
        return;
    }

    // グラフィック面を表示に加える。
    //
    // nullptr でも構わない (確保に失敗した場合)。そのときはテキストだけを描く。
    g_display.setGraphicVram(g_graphicVram);

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
    //
    // マウスはこの設定と無関係に効く。キーボードを出していない間は
    // 画面全体がマウス領域になる (input_touch.cpp の poll を見よ)。
    // SX-Window はマウスが無いと何も操作できないので、ここを塞げない。
    g_keyboard.setVisible(false);
    g_keyboard.begin();

    // 顔 ⇄ X68000 の切り替えを用意する。
    //
    // 既定は X68K モード (app_mode.h)。起動直後に見たいのは Human68k が
    // 立ち上がったかどうかで、顔から始めると確かめるのに切り替えが要る。
    //
    // ここで applyModeTransition を通すのは、g_allowedSliceCycles を
    // 初期化するため。0 のまま残すとエミュレーションが 1 命令も進まず、
    // 利用者にはフリーズとしか見えない。
    // 起動直後に押さえたスプライトを渡す。nullptr でもよい (仮の実装は使わない)。
    g_avatar.setSpriteBuffer(g_avatarSprite);

    g_servo.begin();
    if (!g_servo.isAttached())
    {
        // 異常ではない。サーボが付いていない CoreS3 単体でも顔と X68000 は
        // 動く (servo.h の NullServo を見よ)。
        ESP_LOGI(kTag, "サーボは付いていません。首は振りません");
    }
    applyModeTransition(g_mode.request(x68k_platform::ModeRequest::ToX68k));
    ESP_LOGI(kTag, "Tab で顔 ⇄ X68000 を切り替えます");

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
        g_keyboard.poll(g_keys, g_mouse);
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
            // 顔モードの間は LCD へ送らない。
            //
            // Why not フレームを作らせないか: KeepRunning ではエミュレーションが
            // 走り続けるので画面は変わり続ける。作るのをやめると、戻ったときに
            // VRAM が次に変わるまで古い画面が残る (ダーティ行だけを送る仕組み
            // なので、変化が無ければ何も送られない)。作らせておいて、
            // 送る直前で捨てる方が復帰が速い。
            //
            // Why not take() ごと飛ばさないか: 飛ばすと公開済みのフレームが
            // 返らず、エミュレーションコアは次の publish に失敗し続ける。
            // 受け取って done() を返し、送らないだけにする。
            if (g_mode.mode() == x68k_platform::AppMode::X68k)
            {
                g_display.pushFrame(frame);
            }
            g_frames.done();
        }

        vTaskDelay(pdMS_TO_TICKS(16));  // 約 60Hz
    }
}
