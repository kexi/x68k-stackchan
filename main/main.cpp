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
#include <esp_cpu.h>
#include <esp_timer.h>

// キャッシュのアクセス/ミスカウンタ (計測用。恒久機能ではない)。
#include "soc/extmem_reg.h"
#include <driver/usb_serial_jtag.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "jit/block_runner.h"
#include "jit/xtensa_encoder.h"
#include "jit/exec_memory.h"

#include "app_mode.h"
#include "audio.h"
#include "avatar.h"
#include "display_lcd.h"
#include "speaker_m5.h"
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
// フロッピー 2 台。イメージが無ければ「ディスクが入っていない」まま。
x68k_platform::SdFloppy g_floppy[x68k::Fdc::kDriveCount];
x68k_platform::DisplayLcd g_display;
x68k_platform::TouchKeyboard g_keyboard;
x68k_platform::FrameChannel g_frames;
x68k_platform::KeyQueue g_keys;
x68k_platform::MouseQueue g_mouse;

// --- 音源 (YM2151 / MSM6258V) ---
//
// 合成は Core1 (エミュレーションコア) で行う。Machine を所有するのが
// Core1 だからで、表示と同じ切り分け (frame_channel.h) をそのまま音へ
// 当てはめた形。できたサンプルは AudioChannel のリングを通って Core0 の
// 音声タスクへ渡り、そこから M5Unified のスピーカーへ流れる。
//
// Why not 3 つ目のコアに載せないか: ESP32-S3 は 2 コアしかない。
// Why not Core0 で合成しないか: 合成には OPM のレジスタと ADPCM の FIFO を
// 読む必要があり、それはゲストが $E90003 / $E92003 へ書いている最中に
// 変わる。Core0 から読めば表示で避けたのと同じデータ競合を音で再現する
// ことになる。
x68k_platform::AudioChannel g_audio;
x68k_platform::M5SpeakerSink g_speaker;

// 音源を鳴らすか。
//
// 実測 (下の docs/knowledge/cores3-emulator-runtime.md の値) では、音が
// 鳴っていない間のコストがほぼゼロなので既定は ON。合成そのものは
// Machine::renderAudio が「全スロット無音なら合成を省く」早期リターンを
// 持っており、待機中に払うのは 512 サンプルのゼロ埋めだけになる。
//
// Why not ビルド時の #if だけにしないか: 実機で「音を切ったら速くなるか」を
// 焼き直さずに比べたい。逆に実行時だけにすると、音を使わない構成でも
// スピーカーの I2S タスクと DMA バッファ (8 x 256) が常駐する。両方持つ。
#ifndef X68K_ENABLE_AUDIO
#define X68K_ENABLE_AUDIO 1
#endif
std::atomic<bool> g_audioEnabled{X68K_ENABLE_AUDIO != 0};

// 毎命令通る経路の最適化を入れるか (src/x68k/core/perf_switch.h)。
//
// Why not コンパイル時に決めないか: 最適化が効いたかどうかは実機でしか
// 判定できないのに、実効クロックは起動ごとに揺れる (SD の中身、PSRAM の
// 割り付け、温度)。焼き直して前後を比べるとその揺れが混ざるので、
// **同じ起動の中で切り替えて**比べる。音源の '|' が同じ形で先にある。
//
// これを 1 回焼けば、5 秒ごとの実効クロックの報告を挟みながら
// 全ての組み合わせを順に測れる。既定は有効 (本番の姿)。
//
// 立てるのはシリアルを受けるタスク、読むのはエミュレーションコア。
// 切り替えても状態遷移は変わらないので、遷移が 1 スライス遅れて
// 効いても計測には影響しない。
std::atomic<bool> g_fastTickEnabled{true};

// イベント駆動 (docs/knowledge/event-driven-implementation.md)。
//
// 命令ごとに全デバイスへサイクルを配るのをやめ、「次にどれかの状態が
// 変わる時点」まで時間を溜めて一度に流す。状態が変わる瞬間は 1 サイクルも
// ずれない (quantum と違う点がここ)。
//
// **既定は無効**。有効側の生成コードを変えないまま、同じ起動の中で
// '$' で切り替えて前後を測るために入れてある。効果を確かめて確定するまでは
// 本番の姿を動かさない。
//
// 立てるのはシリアルを受けるタスク、読むのはエミュレーションコア。
// 切り替えはスライスの切れ目でしか効かないので、run の途中で経路が
// 変わることは無い。
std::atomic<bool> g_eventDrivenEnabled{false};

// JIT の上限を測るモード (src/x68k/core/cpu/jit_probe.h)。
//
// 命令の実行を空回しにして走らせ、ループ運営・割り込み判定・デバイスの
// tick だけが残った状態の実効クロックを見る。ここで出る数字が
// 「JIT が命令を無限に速く実行できたとして届く上限」になる。
//
// **状態が進まないのでゲストは止まって見える。** 恒久的な機能ではなく、
// JIT に着手するかどうかを決めるための実測用。既定は無効。
std::atomic<bool> g_nullExecProbe{false};
std::atomic<int> g_nullExecStage{0};
std::atomic<bool> g_eventNullExec{false};

// スライスの実時間の内訳を測る。恒久的な機能ではない。
//
// 実効クロックからの逆算は「1 スライスあたり何回それが起きるか」の仮定に
// 依存する。その仮定が外れて誤診したことがあるので (SD が原因と読み違えた)、
// 直接測れる形を常設にしておく。
std::int64_t g_runUs = 0;
std::uint32_t g_runCount = 0;
#if X68K_MEASURE_DISK
std::int64_t g_renderUs = 0;
std::uint32_t g_renderCount = 0;
#endif

// 上限計測の段に名前を付ける。段の定義は machine.cpp の switch にある。
//
// 段 4-7 は tickDevices (全体の 59%) の中身を MFP / RTC / CRTC へ
// 分解するためにある。CRTC をイベント化するかどうかの判断が、CRTC 単独の
// 寄与を知らないと決まらない (docs/knowledge/event-driven-implementation.md)。
const char* nullExecStageName(int stage)
{
    static const char* const kNames[x68k::Machine::kNullExecStageCount] = {
        "全部含む (基準)", "tickDevices を外す", "割り込み判定を外す", "両方外す (床)",
        "MFP だけ外す",    "RTC だけ外す",       "CRTC だけ外す",      "CRTC だけ残す",
    };
    const bool isKnown = stage >= 0 && stage < x68k::Machine::kNullExecStageCount;
    if (!isKnown)
    {
        return "不明";
    }
    return kNames[stage];
}

// 段を選び直す。'_' の巡回と '#' の直接指定が両方ここを通る。
//
// Machine へ写すのはシリアルのタスクだが、setNullExecStage が書くのは
// int 1 つで、エミュレーションコアが読むのはスライスの入口だけ。
// 途中で切り替わっても、次のスライスから新しい段になるだけで壊れない。
void applyNullExecStage(int stage)
{
    g_nullExecStage = stage;
    g_machine.setNullExecStage(stage);
    ESP_LOGI(kTag, "上限計測 stage %d: %s", stage, nullExecStageName(stage));
}

// 上の値をエミュレーションコアが Machine へ写したかどうか。
// 毎スライス atomic を読んで書き戻すのは無駄なので、変化したときだけ writes。
bool g_fastTickApplied = true;

// 同じくイベント駆動を Machine へ写したかどうか。既定は無効。
bool g_eventDrivenApplied = false;

// 音声タスクが動いているか。スピーカーを開けなかったときは false のまま。
bool g_speakerReady = false;

// テスト音を鳴らしてほしい。表示コアが立て、エミュレーションコアが消す。
//
// Why 要るか: 「音が鳴る」ことを実機で確かめたいが、耳では確かめられない
// (人が横にいないと分からない)。ゲストのソフトが音を出すのを待つのも、
// Human68k のプロンプトからは何も鳴らないので当てにならない。OPM を
// 直接キーオンする口があれば、待機中は振幅ゼロ・鳴らせば非ゼロ、という
// 対比をログの数字だけで作れる。
//
// Why not これを core/ に置かないか: これは実機の確認手段であって
// X68000 の機能ではない。Opm のレジスタを 2 段書きするだけなので、
// ゲストがやるのと同じ経路 (writeAddress/writeData) で足りる。
//
// Why not 表示コアから直接 Opm を叩かないか: Opm は Machine の一部で、
// 所有者は Core1。表示コアから書くと、合成の最中にレジスタが変わる。
// 画面ダンプ (g_dumpRequested) と同じく要求だけ立てる。
std::atomic<bool> g_testToneRequested{false};

// OPM ch0 を「すぐ最大音量で鳴り続ける」設定にしてキーオンする。
// エミュレーションコアから呼ぶ (Machine を触るため)。
void playTestTone(x68k::Machine& machine)
{
    x68k::Opm& opm = machine.opm();
    const auto writeReg = [&opm](x68k::u8 reg, x68k::u8 value)
    {
        // 実機と同じ 2 段書き (アドレス latch → データ)。
        opm.writeAddress(reg);
        opm.writeData(value);
    };

    // 4 スロットとも AR 最大・減衰なし・TL 0 (最大音量)。
    // test/test_audio.cpp の setupLoudChannel と同じ設定にしてある。
    // ホストのテストが検査したのと同じ音を実機で出せば、両者の突き合わせが
    // できる (ホストで測った振幅と実機のログが一致するはず)。
    for (x68k::u8 slot = 0; slot < 4; ++slot)
    {
        const x68k::u8 offset = static_cast<x68k::u8>(slot * 8);
        writeReg(static_cast<x68k::u8>(0x40 + offset), 0x01);  // DT1=0 MUL=1
        writeReg(static_cast<x68k::u8>(0x60 + offset), 0x00);  // TL=0
        writeReg(static_cast<x68k::u8>(0x80 + offset), 0x1F);  // KS=0 AR=31
        writeReg(static_cast<x68k::u8>(0xA0 + offset), 0x00);  // D1R=0
        writeReg(static_cast<x68k::u8>(0xC0 + offset), 0x00);  // DT2=0 D2R=0
        writeReg(static_cast<x68k::u8>(0xE0 + offset), 0x0F);  // D1L=0 RR=15
    }
    writeReg(0x20, 0xC7);  // RL=両方on FL=0 CONN=7
    writeReg(0x28, 0x4A);  // KC (オクターブ 4 の A)
    writeReg(0x30, 0x00);  // KF=0
    writeReg(0x08, 0x78);  // 全スロットキーオン
}

// テスト音を止める (キーオフ)。RR=15 にしてあるのですぐ減衰する。
void stopTestTone(x68k::Machine& machine)
{
    machine.opm().writeAddress(0x08);
    machine.opm().writeData(0x00);
}

// --- スタックチャン (顔 ⇄ X68000) ---
//
// FSM を触るのは表示コア (Core0) だけ。エミュレーションコアへは下の
// atomic 1 つだけを渡す (app_mode.h の AppModeMachine のコメントを見よ)。
x68k_platform::AppModeMachine g_mode;
x68k_platform::Avatar g_avatar;

// 顔の大きさが LCD と一致していることを確かめる。
//
// Avatar は core/ を引き込まないために自前で 320x240 を持っている
// (avatar.h の kWidth を見よ)。食い違うと pushFrame が別の大きさの
// バッファを送り、画面が崩れるか読み過ぎる。
static_assert(x68k_platform::Avatar::kWidth == x68k_platform::DisplayLcd::kScreenWidth,
              "顔の幅が LCD と違う");
static_assert(x68k_platform::Avatar::kHeight == x68k_platform::DisplayLcd::kScreenHeight,
              "顔の高さが LCD と違う");

// サーボ。既定は NullServo (何もしない)。
//
// Why 既定を NullServo にするか: CoreS3 単体が通常の開発形態で、
// サーボの信号線を出す Port.A (GPIO 1/2) は M5Unified が外部 I2C にも
// 使う。Port.A に I2C の Unit を挿している環境で既定を PWM にすると、
// I2C のバスへ 50Hz の矩形波を流し込むことになる (servo.h の冒頭)。
//
// Why not コンパイル時に選ばせないか: どちらを使うかは焼いた後に
// 「サーボを挿してみる」で決まる。焼き直しが要ると、繋いで試すたびに
// 数分待つことになる。両方を実体として持ち、ポインタで差し替える。
x68k_platform::NullServo g_nullServo;
x68k_platform::LedcServo g_ledcServo;
x68k_platform::Servo* g_servo = &g_nullServo;

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
// Why 60000 か: イベント駆動の期限は armDeadline が sliceEnd_ で切り詰める
// ので、**スライス長がそのまま期限の上限になる** (scheduler.h)。20000 の
// ときは期限の平均が 13,000 サイクルにしかならず、実測でも 1 スライスあたり
// 約 2 回も遅い側へ落ちていた。60000 にすると期限平均が 25,000 へ倍増し、
// 実効クロックが 7579 -> 8024 kHz (+5.9%) になった (実機で実測)。
//
// Why not もっと伸ばさないか: 180000 (1 フレーム相当) でも試したが
// 8307 kHz で、60000 からの伸びは +3.5% しかない。一方でキー入力と
// マウスの投入はスライスの切れ目でしか起きないので、待ち時間が
// スライス長に比例して増える。音声も 1 スライスにつき最大 1 ブロック
// (32.8ms ぶん) しか補充しないので、伸ばしすぎると途切れる。
// 伸ばす価値と応答の悪化が釣り合う点として 60000 を選んだ。
//
// kFallbackSpan (65536) は超えないので、期限が頭打ちになることもない。
constexpr x68k::u32 kSliceCycles = 60000;

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

// JIT が成立するかを確かめる最小の実験。
//
// 「実行可能メモリを取れるか」「そこへ書いた Xtensa 命令が本当に走るか」の
// 2 点だけを見る。ここが通らなければ動的再コンパイルの検討自体が無意味。
//
// 生成するのは a2 (戻り値レジスタ) へ定数を入れて返るだけの関数。
//   MOVI.N a2, imm   … 0x0C + (imm<<4) の 2 バイト (imm は 0-15)
//   RET.N            … 0xF00D の 2 バイト
// どちらも Xtensa の narrow (2 バイト) 命令。リトルエンディアンで並べる。
//
// Why not 大きい定数を入れないか: MOVI.N の即値は 4bit しか無い。
// ここで確かめたいのは「走るか」だけなので、幅は要らない。
// シリアルの 'j' で呼ぶ。
//
// かつて「CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y の間は MALLOC_CAP_EXEC が
// 必ず失敗する」と書いていたが、**現在その設定は無効** (sdkconfig の
// # CONFIG_ESP_SYSTEM_MEMPROT_FEATURE is not set) で、実機で確かめたら
// EXEC|32BIT で確保でき、生成したコードが正しく走った (2026-08-18)。
// 前提が変わったら測り直すこと。
// ネイティブ発行の上限を測る。**恒久的な機能ではない。**
//
// インタプリタは 1 ゲスト命令に約 85 CPU サイクル使っている (実測)。
// これをネイティブコードにしたら何サイクルになりうるかを、
// 「ゲスト命令 1 つぶんの仕事」を手書きの Xtensa で回して測る。
//
// ここで測るのは **床** であって、実際の JIT が出す値ではない。
// 床がインタプリタと大差なければ、JIT を書いても意味が無い。
// 段 0-C2: ブロックキャッシュの経路そのものの費用を測る。
// **恒久的な機能ではない。**
//
// JIT が届くかは「ネイティブ命令本体の速さ」だけでは決まらない。
// ブロックへ入るたびに検索・タグ比較・世代検査を払い、出るたびに
// 状態を書き戻す。**これらはエミッタを書かなくても測れる。**
//
// ここで出すのは E_budget = ネイティブ命令本体に使える予算:
//
//   E_budget = 必要 Chit - (検索 + 世代検査 + ゲートウェイ + 出口)
//
// これが小さすぎるなら、エミッタをいくら磨いても届かない。
//
// **床でないものを引く。** 空ループのコストを必ず差し引く
// (かつて 14.1 サイクルと記録した床が、実は 4.7 サイクルのループと
// windowed ABI の交絡込みだった)。
[[maybe_unused]] void probeCachePathCost()
{
    // 実際のブロックキャッシュに近い形の表を内部 SRAM に置く。
    // PSRAM は使わない (散らばったアクセスで実機が止まった実績がある)。
    constexpr unsigned kSlots = 512;
    struct Slot
    {
        std::uint32_t pc;  // タグ
        std::uint16_t ir;  // プリフェッチ状態も鍵に含む
        std::uint16_t irc;
        std::uint32_t epoch;  // 写像の世代
        std::uint16_t gen0;   // コードページの世代 (2 ページぶん)
        std::uint16_t gen1;
        std::uint32_t code;  // 生成コードの位置
    };
    auto* slots = static_cast<Slot*>(
        heap_caps_calloc(kSlots, sizeof(Slot), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto* gens = static_cast<std::uint16_t*>(
        heap_caps_calloc(2048, sizeof(std::uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (slots == nullptr || gens == nullptr)
    {
        ESP_LOGW(kTag, "[c2] 内部 SRAM を確保できない (空き %u / 最大ブロック %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        heap_caps_free(slots);
        heap_caps_free(gens);
        return;
    }

    // 実ワークロードに近い分布で埋める。ホストの計測では上位 50 ブロックが
    // 96.6% を占めていたので、少数のスロットを繰り返し引く。
    constexpr unsigned kHot = 64;
    for (unsigned i = 0; i < kSlots; ++i)
    {
        slots[i].pc = 0xFF0000u + i * 8u;
        slots[i].ir = static_cast<std::uint16_t>(i);
        slots[i].irc = static_cast<std::uint16_t>(i * 3u);
        slots[i].epoch = 1;
        slots[i].gen0 = 0;
        slots[i].gen1 = 0;
        slots[i].code = 0x40000000u + i * 64u;
    }

    constexpr int kIters = 2000000;
    std::uint32_t sink = 0;

    // --- 空ループ (これを全部から引く) ---
    std::uint32_t t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        asm volatile("" ::: "memory");
        sink += static_cast<std::uint32_t>(i);
    }
    std::uint32_t t1 = esp_cpu_get_cycle_count();
    const double loopCycles = (double)(t1 - t0) / (double)kIters;

    // --- 1. 検索 (ハッシュ + タグ比較) ---
    // ブロックへ入るたびに必ず払う。
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        const std::uint32_t pc = 0xFF0000u + (static_cast<std::uint32_t>(i) % kHot) * 8u;
        const unsigned idx = (pc >> 1) & (kSlots - 1);
        const Slot& s = slots[idx];
        if (s.pc == pc)
        {
            sink += s.code;
        }
    }
    t1 = esp_cpu_get_cycle_count();
    const double lookupCycles = (double)(t1 - t0) / (double)kIters - loopCycles;

    // --- 2. 検索 + プリフェッチ状態の照合 ---
    // 鍵は pc だけでは足りない。ir/irc と写像の世代も一致していないと、
    // 違うプリフェッチ状態のまま実行してしまう。
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        const std::uint32_t pc = 0xFF0000u + (static_cast<std::uint32_t>(i) % kHot) * 8u;
        const unsigned idx = (pc >> 1) & (kSlots - 1);
        const Slot& s = slots[idx];
        const std::uint16_t ir = static_cast<std::uint16_t>(idx);
        const std::uint16_t irc = static_cast<std::uint16_t>(idx * 3u);
        if (s.pc == pc && s.ir == ir && s.irc == irc && s.epoch == 1u)
        {
            sink += s.code;
        }
    }
    t1 = esp_cpu_get_cycle_count();
    const double keyCycles = (double)(t1 - t0) / (double)kIters - loopCycles;

    // --- 3. 検索 + 鍵 + 世代検査 2 ページ ---
    // ブロックが跨ぐ可能性のある 2 ページぶんを見る。
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        const std::uint32_t pc = 0xFF0000u + (static_cast<std::uint32_t>(i) % kHot) * 8u;
        const unsigned idx = (pc >> 1) & (kSlots - 1);
        const Slot& s = slots[idx];
        const std::uint16_t ir = static_cast<std::uint16_t>(idx);
        const std::uint16_t irc = static_cast<std::uint16_t>(idx * 3u);
        const unsigned page = (pc >> 10) & 2047u;
        if (s.pc == pc && s.ir == ir && s.irc == irc && s.epoch == 1u && s.gen0 == gens[page] &&
            s.gen1 == gens[(page + 1u) & 2047u])
        {
            sink += s.code;
        }
    }
    t1 = esp_cpu_get_cycle_count();
    const double fullCheckCycles = (double)(t1 - t0) / (double)kIters - loopCycles;

    // --- 4. 出口の状態書き戻し ---
    // ブロックを抜けるとき pc/ir/irc を必ず実体化する。
    // インタプリタが命令ごとに払っていたものを、ブロック単位に減らせるかが
    // JIT の利得の中心だったので、その残りを測る。
    // **ループの外へ持ち上げられないようにする。**
    //
    // 静的な 1 つの変数へ書くと、コンパイラは最後の 1 回だけ残して
    // ループから追い出す (実測で -7.09 サイクル = 空ループぶんの
    // マイナスが出た。消えている証拠)。
    // volatile にすると毎回ストアは立つが、レジスタ割り当ても妨げるので
    // 過大に出る。実際の JIT は M68kState の連続した領域へ書くので、
    // 書き先を回して「消せないが volatile でもない」形にする。
    struct StateOut
    {
        std::uint32_t pc;
        std::uint16_t ir;
        std::uint16_t irc;
    };
    auto* outs = static_cast<StateOut*>(
        heap_caps_calloc(64, sizeof(StateOut), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (outs == nullptr)
    {
        ESP_LOGW(kTag, "[c2] 出口計測の領域を確保できない");
        heap_caps_free(slots);
        heap_caps_free(gens);
        return;
    }
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        StateOut& o = outs[static_cast<unsigned>(i) & 63u];
        o.pc = 0xFF0000u + static_cast<std::uint32_t>(i);
        o.ir = static_cast<std::uint16_t>(i);
        o.irc = static_cast<std::uint16_t>(i * 3u);
    }
    t1 = esp_cpu_get_cycle_count();
    const double commitCycles = (double)(t1 - t0) / (double)kIters - loopCycles;
    // 最適化で消えていないことを確かめる (消えていれば 0 に近い値になる)。
    sink += outs[0].pc + outs[0].ir + outs[0].irc;

    ESP_LOGI(kTag, "[c2] 内部 SRAM 空き %u / 最大ブロック %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_LOGI(kTag, "[c2] 空ループ %.2f サイクル (以下は差し引き済み)", loopCycles);
    ESP_LOGI(kTag, "[c2] 検索 (ハッシュ+タグ)            %.2f", lookupCycles);
    ESP_LOGI(kTag, "[c2] + 鍵の照合 (ir/irc/epoch)       %.2f", keyCycles);
    ESP_LOGI(kTag, "[c2] + 世代検査 2 ページ             %.2f", fullCheckCycles);
    ESP_LOGI(kTag, "[c2] 出口の状態書き戻し (pc/ir/irc)  %.2f", commitCycles);
    ESP_LOGI(kTag, "[c2] ブロック 1 本あたり = %.2f + ゲートウェイ 6.7 = %.2f サイクル",
             fullCheckCycles + commitCycles, fullCheckCycles + commitCycles + 6.7);
    ESP_LOGI(kTag, "[c2] sink=%u", static_cast<unsigned>(sink));

    heap_caps_free(slots);
    heap_caps_free(gens);
    heap_caps_free(outs);
}

// 段 0-C3: ネイティブ命令本体の実費を測る。**恒久的な機能ではない。**
//
// 0-C2 でブロックの固定費が出た。残る未知は「ネイティブの命令本体が
// インタプリタ比で何倍になるか」。**4 倍なら Go、2 倍なら No-Go。**
//
// エミッタは書かない。C++ で「JIT が吐くであろう形」を手書きし、
// -O2 の生成コードを実測する。Xtensa のエンコーディングを自分で
// 組み立てるのは 2 度失敗しているので、コンパイラに吐かせる。
//
// **これは上限寄りの値。** 実際のエミッタはレジスタ割り当てが
// これより下手になる。下回ったら No-Go の材料になる。
[[maybe_unused]] void probeNativeBody()
{
    // ゲストのレジスタファイル。JIT も同じ形で持つ。
    static std::uint32_t d[8];
    static std::uint32_t a[8];
    static std::uint16_t sr;
    static std::uint8_t ram[65536];
    for (int i = 0; i < 8; ++i)
    {
        d[i] = 0x12345678u + static_cast<std::uint32_t>(i);
        a[i] = 0x1000u + static_cast<std::uint32_t>(i) * 4u;
    }

    constexpr int kIters = 2000000;
    std::uint32_t sink = 0;

    // --- 空ループ ---
    std::uint32_t t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        asm volatile("" ::: "memory");
        sink += static_cast<std::uint32_t>(i);
    }
    std::uint32_t t1 = esp_cpu_get_cycle_count();
    const double loop = (double)(t1 - t0) / (double)kIters;

    // --- 1. MOVE.w D0,D1 + N/Z フラグ ---
    // 最も単純な形。JIT が最も得意とする命令。
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        // **転送元を毎回変える。** 固定だと結果が不変になり、
        // ループ外へ持ち上げられる (実測で負の値が出た)。
        d[0] = d[0] + static_cast<std::uint32_t>(i);
        const std::uint32_t v = d[0] & 0xFFFFu;
        d[1] = (d[1] & 0xFFFF0000u) | v;
        std::uint16_t s = static_cast<std::uint16_t>(sr & 0xFFF0u);
        if (v == 0)
        {
            s |= 0x0004u;
        }
        if ((v & 0x8000u) != 0)
        {
            s |= 0x0008u;
        }
        sr = s;
        sink += v;
    }
    t1 = esp_cpu_get_cycle_count();
    const double moveReg = (double)(t1 - t0) / (double)kIters - loop;

    // --- 2. MOVE.w (A0),D1 : fast RAM 読み + 境界検査 + ビッグエンディアン組立 ---
    // 実効アドレスが窓に収まるかを実行時に見る (Codex の言う runtime guard)。
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        // **アドレスを毎回変える。** 定数だと境界検査もアドレス計算も
        // ループ外へ持ち上げられ、実測が無意味になる。
        const std::uint32_t ea = (a[0] + static_cast<std::uint32_t>(i) * 2u) & 0xFFFEu;
        if ((ea & 1u) == 0 && ea + 1u < sizeof(ram))
        {
            const std::uint32_t v = static_cast<std::uint32_t>((ram[ea] << 8) | ram[ea + 1]);
            d[1] = (d[1] & 0xFFFF0000u) | v;
            std::uint16_t s = static_cast<std::uint16_t>(sr & 0xFFF0u);
            if (v == 0)
            {
                s |= 0x0004u;
            }
            if ((v & 0x8000u) != 0)
            {
                s |= 0x0008u;
            }
            sr = s;
            sink += v;
        }
    }
    t1 = esp_cpu_get_cycle_count();
    const double moveLoad = (double)(t1 - t0) / (double)kIters - loop;

    // --- 3. MOVE.w D1,(A0) : fast RAM 書き + 世代の更新 ---
    static std::uint16_t gens[64];
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        const std::uint32_t ea = (a[0] + static_cast<std::uint32_t>(i) * 2u) & 0xFFFEu;
        if ((ea & 1u) == 0 && ea + 1u < sizeof(ram))
        {
            const std::uint32_t v = d[1] & 0xFFFFu;
            ram[ea] = static_cast<std::uint8_t>(v >> 8);
            ram[ea + 1] = static_cast<std::uint8_t>(v);
            // CodeGenMap::touch 相当 (飽和つき)
            const unsigned page = (ea >> 10) & 63u;
            const std::uint16_t cur = gens[page];
            gens[page] = cur == 0xFFFFu ? 0xFFFFu : static_cast<std::uint16_t>(cur + 1);
            sink += v;
        }
    }
    t1 = esp_cpu_get_cycle_count();
    const double moveStore = (double)(t1 - t0) / (double)kIters - loop;

    // --- 4. Bcc の条件評価 + 期限判定 ---
    // ブロック内の各命令の後に debt を進めて期限を見る。
    static std::int32_t debt = -1000000;
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < kIters; ++i)
    {
        // **sr を毎回変える。** 固定だと条件が定数畳み込みされ、
        // 分岐そのものが消える (実測で負の値が出た)。
        sr = static_cast<std::uint16_t>(sink & 0x001Fu);
        const bool taken = (sr & 0x0004u) != 0;
        debt += 8;
        if (debt >= 0)
        {
            debt = -1000000;
        }
        sink += taken ? 1u : 3u;
    }
    t1 = esp_cpu_get_cycle_count();
    const double bccDebt = (double)(t1 - t0) / (double)kIters - loop;

    constexpr double kInterp = 85.0;
    ESP_LOGI(kTag, "[c3] 空ループ %.2f (以下は差し引き済み)", loop);
    ESP_LOGI(kTag, "[c3] MOVE.w D0,D1 + NZ            %.2f (インタプリタ比 %.1f 倍)", moveReg,
             kInterp / (moveReg > 0.01 ? moveReg : 0.01));
    ESP_LOGI(kTag, "[c3] MOVE.w (A0),D1 + guard + NZ  %.2f (%.1f 倍)", moveLoad,
             kInterp / (moveLoad > 0.01 ? moveLoad : 0.01));
    ESP_LOGI(kTag, "[c3] MOVE.w D1,(A0) + guard + gen %.2f (%.1f 倍)", moveStore,
             kInterp / (moveStore > 0.01 ? moveStore : 0.01));
    ESP_LOGI(kTag, "[c3] Bcc + debt 更新              %.2f (%.1f 倍)", bccDebt,
             kInterp / (bccDebt > 0.01 ? bccDebt : 0.01));
    // 実行分布で重み付けした平均 (レジスタ間 30% / 読み 30% / 書き 20% / 分岐 20%)
    const double weighted = moveReg * 0.3 + moveLoad * 0.3 + moveStore * 0.2 + bccDebt * 0.2;
    ESP_LOGI(kTag, "[c3] 分布で重み付けした本体 %.2f サイクル (インタプリタ比 %.2f 倍)", weighted,
             kInterp / (weighted > 0.01 ? weighted : 0.01));
    // **負の値は測定が壊れている合図。** 差分を取る計測では、
    // 最適化で消えた側が空ループぶんのマイナスとして現れる。
    const bool anyNegative = moveReg < 0.0 || moveLoad < 0.0 || moveStore < 0.0 || bccDebt < 0.0;
    if (anyNegative)
    {
        ESP_LOGW(kTag, "[c3] **負の値がある = 最適化で消えている。この結果は無効**");
    }
    ESP_LOGI(kTag, "[c3] sink=%u", static_cast<unsigned>(sink));
}

[[maybe_unused]] void probeNativeCeiling()
{
    constexpr std::size_t kBytes = 256;
    auto* code =
        static_cast<std::uint8_t*>(heap_caps_malloc(kBytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT));
    if (code == nullptr)
    {
        ESP_LOGW(kTag, "[native] 実行可能メモリを確保できない");
        return;
    }

    // 生成するのは `int loop(int n) { while (n--) ; return 0; }` に
    // ゲスト命令 1 つぶんの仕事を混ぜたもの。
    //
    // ゲストの MOVE.L D0,D1 に相当する最小の仕事:
    //   - レジスタ配列から 1 ワード読む
    //   - 別の位置へ書く
    //   - フラグを更新する (N/Z の 2 ビット)
    // Xtensa なら l32i + s32i + 数命令。**4-8 サイクル**が床になる。
    //
    // エンコーディングは推測しない。probeJitFeasibility のコメントに
    // ある通り、自分で組み立てて 2 回落ちている。ここでは
    // 「entry / 単純ループ / retw.n」だけに留め、既に検証済みの
    // バイト列を組み合わせる。
    //
    //   004136  entry a1, 32
    //   a20c    movi.n a2, 10      (戻り値)
    //   f01d    retw.n
    //
    // ループは入れず、**呼び出しのコストだけ**を測る。
    // 1 回の callx8 + entry + retw が何 ns かが分かれば、
    // 「1 命令ごとにネイティブへ飛ぶ」形の下限が出る。
    auto* word = reinterpret_cast<volatile std::uint32_t*>(code);
    word[0] = 0x0C004136u;
    word[1] = 0x00F01DA2u;
    asm volatile("isync" ::: "memory");

    using Fn = int (*)();
    auto fn = reinterpret_cast<Fn>(code);

    // 1000 万回呼んで実時間を測る。
    constexpr int kIters = 10000000;
    const std::int64_t t0 = esp_timer_get_time();
    int sink = 0;
    for (int i = 0; i < kIters; ++i)
    {
        sink += fn();
    }
    const std::int64_t t1 = esp_timer_get_time();
    const double nsPerCall = (double)(t1 - t0) * 1000.0 / (double)kIters;
    ESP_LOGI(kTag, "[native] 呼び出し 1 回 %.1f ns (%.1f CPU サイクル) sink=%d", nsPerCall,
             nsPerCall * 0.24, sink);
    ESP_LOGI(kTag, "[native] 参考: インタプリタは 1 ゲスト命令 約 85 CPU サイクル");

    // ここまでは windowed ABI (entry / retw.n / callx8) の値。
    //
    // **床が呼び出しそのもののコストなのか、レジスタウィンドウの
    // 回転コストなのかを切り分ける。** JIT が出すコードは call0 ABI を
    // 選べるので、ウィンドウ回転が主因なら床は下がる。
    //
    //   ret.n  = 0xF00D
    // entry も retw も使わない。call0 は戻り番地を a0 に置くだけなので、
    // 何もしない関数は ret.n 1 命令で足りる。
    word[0] = 0x0000F00Du;
    asm volatile("isync" ::: "memory");

    const std::int64_t t2 = esp_timer_get_time();
    // callx0 は a0 を壊す。clobber に入れて退避を任せる。
    for (int i = 0; i < kIters; ++i)
    {
        asm volatile("callx0 %0" ::"r"(code) : "a0", "memory");
    }
    const std::int64_t t3 = esp_timer_get_time();
    const double ns0 = (double)(t3 - t2) * 1000.0 / (double)kIters;
    ESP_LOGI(kTag, "[native] call0 呼び出し 1 回 %.1f ns (%.1f CPU サイクル)", ns0, ns0 * 0.24);

    // **空ループそのもののコストを引く。** 上の 2 つは「ループ 1 周 +
    // 呼び出し 1 回」を測っている。呼び出しの実費を知るには、同じ形の
    // ループで呼び出しだけ抜いた値が要る。
    const std::int64_t t4 = esp_timer_get_time();
    for (int i = 0; i < kIters; ++i)
    {
        asm volatile("" ::: "memory");
    }
    const std::int64_t t5 = esp_timer_get_time();
    const double nsLoop = (double)(t5 - t4) * 1000.0 / (double)kIters;
    ESP_LOGI(kTag, "[native] 空ループ 1 周 %.1f ns (%.1f CPU サイクル)", nsLoop, nsLoop * 0.24);
    ESP_LOGI(kTag, "[native] → 呼び出しの実費: windowed %.1f / call0 %.1f CPU サイクル",
             (nsPerCall - nsLoop) * 0.24, (ns0 - nsLoop) * 0.24);

    heap_caps_free(code);
}

[[maybe_unused]] void probeJitFeasibility()
{
    constexpr std::size_t kProbeBytes = 64;
    // 取り方を何通りか試す。どれで取れるかが JIT の実現性そのもの。
    struct Attempt
    {
        const char* name;
        std::uint32_t caps;
    };
    const Attempt attempts[] = {
        {"EXEC|32BIT", MALLOC_CAP_EXEC | MALLOC_CAP_32BIT},
        {"EXEC", MALLOC_CAP_EXEC},
        {"EXEC|INTERNAL", MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL},
        {"IRAM_8BIT", MALLOC_CAP_IRAM_8BIT},
    };
    std::uint8_t* code = nullptr;
    const char* usedName = nullptr;
    for (const Attempt& a : attempts)
    {
        code = static_cast<std::uint8_t*>(heap_caps_malloc(kProbeBytes, a.caps));
        ESP_LOGI(kTag, "[jit] %s -> %p", a.name, static_cast<void*>(code));
        if (code != nullptr)
        {
            usedName = a.name;
            break;
        }
    }
    if (code == nullptr)
    {
        ESP_LOGW(kTag, "[jit] 実行可能メモリをどの方法でも確保できない");
        return;
    }
    ESP_LOGI(kTag, "[jit] %s で確保", usedName);

    // 生成するのは `int probe(void) { return 10; }` と同じコード。
    //
    // バイト列はアセンブラの出力をそのまま使う。自分でビットを組み立てて
    // 2 回落とした (MOVI.N のフィールド配置を取り違えて
    // InstrFetchProhibited、その前にバイト書き込みで LoadStoreError)。
    // エンコーディングは推測せず、`xtensa-esp32s3-elf-gcc -O2` の出力を
    // objdump/objcopy で確かめた値を置く:
    //
    //   004136  entry a1, 32     -> 36 41 00
    //   a20c    movi.n a2, 10    -> 0c a2
    //   f01d    retw.n           -> 1d f0
    //
    // entry / retw.n はウィンドウ ABI の対で、通常の関数ポインタ呼び出し
    // (callx8) と噛み合う。
    //
    // IRAM は 32bit 単位でしかアクセスできないので、ワードで書く。
    constexpr std::uint8_t kImm = 10;
    auto* word = reinterpret_cast<volatile std::uint32_t*>(code);
    word[0] = 0x0C004136u;  // 36 41 00 0c
    word[1] = 0x00F01DA2u;  // a2 1d f0 --

    // IRAM は直接実行されるが、書いた直後は命令パイプラインが古い内容を
    // 持ちうる。isync で同期する。
    asm volatile("isync" ::: "memory");

    using ProbeFn = int (*)();
    ProbeFn fn = nullptr;
    std::memcpy(&fn, &code, sizeof(fn));
    const int result = fn();

    ESP_LOGI(kTag, "[jit] 実行可能メモリ %p / 生成コードの戻り値 = %d (期待 %d)", code, result,
             static_cast<int>(kImm));
    heap_caps_free(code);
}

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

// コードページの世代。2MB / 1KB = 2048 ページ = 4KB。
//
// Why 静的配列にしないか: 内部 SRAM の .bss を膨らませると、すぐ下で
// IPL-ROM 128KB を内部 SRAM へ置く処理が失敗する (既存のコメント参照)。
// 確保に失敗したら未配線のままにする。**未配線でも壊れない**ように
// 照合側で kAlwaysStale を弾く契約にしてある。
constexpr x68k::u32 kCodeGenPages = x68k::kMainRamSize / x68k::CodeGenMap::kPageSize;
std::uint16_t* g_codeGen = nullptr;

// JIT のブロックキャッシュ。
//
// スロット数は 2 の冪 (slotIndex がマスクで畳む)。256 スロット x 112 バイト
// = 28,672 バイト。実測の内部 SRAM 空き 48KB に対して、世代配列 4KB と
// 合わせて 32KB。**PSRAM には置かない** (散らばったアクセスで実機が止まる)。
constexpr x68k::u32 kJitSlots = 256;
x68k::jit::BlockSlot* g_jitSlots = nullptr;
x68k::jit::ExecMemory g_jitCode;
x68k::jit::BlockRunner g_jitRunner;
// 実行可能メモリの要求量。実測で 21KB 取れる。
constexpr std::size_t kJitCodeBytes = 16 * 1024;

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

    // JIT のブロックキャッシュと実行可能メモリ。
    //
    // **どちらも失敗してよい。** 取れなければ JIT を教わらないので、
    // 現行インタプリタのまま動く (挙動は 1 ビットも変わらない)。
    g_jitSlots = static_cast<x68k::jit::BlockSlot*>(heap_caps_calloc(
        kJitSlots, sizeof(x68k::jit::BlockSlot), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (g_jitSlots == nullptr)
    {
        ESP_LOGW(kTag, "JIT のスロットを内部 SRAM に置けません (JIT は無効)");
    }
    if (!g_jitCode.acquire(kJitCodeBytes))
    {
        ESP_LOGW(kTag, "実行可能メモリを確保できません (JIT は無効)");
    }

    // コードページの世代は内部 SRAM に置く。ブロックの入口で毎回引くので、
    // PSRAM への散らばったアクセスは避ける (実機を止めた実績がある)。
    g_codeGen = static_cast<std::uint16_t*>(heap_caps_calloc(
        kCodeGenPages, sizeof(std::uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (g_codeGen == nullptr)
    {
        ESP_LOGW(kTag, "コードページの世代を内部 SRAM に置けません (JIT は無効のまま動きます)");
    }

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

    // フロッピーは任意。無くても SASI から起動できるので、開けなくても
    // 警告に留める (HDD と同じ扱い)。
    {
        const char* const paths[x68k::Fdc::kDriveCount] = {x68k_platform::kFd0Path,
                                                           x68k_platform::kFd1Path};
        for (x68k::u32 d = 0; d < x68k::Fdc::kDriveCount; ++d)
        {
            if (!g_floppy[d].open(paths[d]))
            {
                ESP_LOGI(kTag, "FDD%u にディスクを入れません: %s", d, paths[d]);
                continue;
            }
            g_machine.setFloppyDisk(d, &g_floppy[d]);
        }
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

    // ゲスト RAM の書き換えを追う世代マップを配線する。
    //
    // **配線しないと「常に古い」が「常に有効」に化ける。** 未配線だと
    // pageCount_ = 0 なので generation() が全アドレスに kAlwaysStale を
    // 返し、素朴な照合 (控え == 現在) が 0xFFFF == 0xFFFF で通ってしまう。
    // 照合側でも kAlwaysStale を弾くが、ここで配線しておけば
    // 通常のページは正しく世代で判定できる。
    //
    // 2MB / 1KB = 2048 ページ = 4KB。内部 SRAM に置く (PSRAM への
    // 散らばったアクセスは実機を止めた実績がある)。
    g_machine.cpu().codeGenMap().setStorage(g_codeGen, kCodeGenPages);

    return true;
}

// エミュレーションコアから使うが、定義は下にある。
void followCursor();
void dumpScreen();

// シリアルコンソール (SerialConsole) から使うが、定義は下にある。
// FSM の決定を周辺へ反映する。
void applyModeTransition(const x68k_platform::ModeTransition& transition);

// 顔を 1 枚描いて LCD へ送る。
void drawFaceNow();
// 顔の状態 (まぶた・表情) をシリアルへ出す。
void logFaceState();
// サーボを LEDC の実装へ差し替える。
void enableLedcServo();
// 首を 1 段階動かす。key は 'h'/'l'/'k'/'j'。
void moveHead(char key);

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
        // 最適化スイッチが切り替わったら Machine へ写す。
        //
        // スライスの切れ目で writes ので、run の途中で経路が変わることは
        // 無い。毎スライス atomic を 1 回読むだけなので、計測している
        // ホットループの中には何も足さない。
        const bool wantFastTick = g_fastTickEnabled.load(std::memory_order_relaxed);
        if (wantFastTick != g_fastTickApplied)
        {
            x68k::PerfSwitch sw;
            sw.inlineRtcTick = wantFastTick;
            sw.inlineCrtcTick = wantFastTick;
            sw.inlineMfpTimer = wantFastTick;
            g_machine.setPerfSwitch(sw);
            g_fastTickApplied = wantFastTick;
        }

        // イベント駆動の切り替えも同じ形で写す。run() の入口で 1 回だけ
        // 読まれるので、ホットループの中には何も足さない。
        const bool wantEventDriven = g_eventDrivenEnabled.load(std::memory_order_relaxed);
        if (wantEventDriven != g_eventDrivenApplied)
        {
            g_machine.setEventDriven(wantEventDriven);
            g_eventDrivenApplied = wantEventDriven;
        }

        const x68k::u32 sliceCycles = g_allowedSliceCycles.load();
        if (sliceCycles > 0)
        {
            // JIT の上限計測モードでは命令を実行せず、ループ運営と
            // デバイスの時間だけを回す (jit_probe.h)。
            const std::int64_t runT0 = esp_timer_get_time();
            if (g_nullExecProbe.load(std::memory_order_relaxed))
            {
                g_machine.runNullExec(sliceCycles);
            }
            else
            {
                g_machine.run(sliceCycles);
            }
            g_runUs += esp_timer_get_time() - runT0;
            ++g_runCount;
            totalCycles += sliceCycles;
        }

        // 音を 1 ブロック合成してリングへ積む。
        //
        // Machine (OPM のレジスタと ADPCM の FIFO) を触るのはこのコアだけ。
        // 表示と同じ切り分けで、できたサンプルだけを Core0 へ渡す。
        //
        // Why not 「1 スライスにつき必ず 1 ブロック」にしないか: 1 ブロックは
        // 512 サンプル = 32.8ms ぶんの音だが、1 スライス (20000 サイクル) の
        // 実時間は 6.1ms しかない。毎スライス積むと 5 倍の速さで作ることに
        // なり、リングはすぐ満杯になって捨てるだけになる。逆にゲストの
        // サイクル数で刻むのも合わない。実効クロックが実機の 32% なので、
        // ゲスト時間で 32.8ms ぶんを作る頃には実時間で 100ms 経っており、
        // スピーカーの DMA が先に枯れる。
        //
        // リングに溜まっている数を見て、足りないときだけ作る。消費側
        // (音声タスク) が実時間で引いていくので、これだけで実時間に
        // 追従する。段数から 1 枚ぶん余裕を残すのは、次の 1 枚を書ける
        // 空きを常に確保して writeBlock の空振りを減らすため。
        // テスト音を求められていたらここで鳴らす。Opm の所有者はこのコア。
        //
        // 一定時間で自動的に止める。押しっぱなしにすると、以後ずっと
        // 合成が走って「待機中は振幅ゼロ」の対比が取れなくなる。
        static std::uint32_t toneUntilMs = 0;
        if (g_testToneRequested.exchange(false))
        {
            playTestTone(g_machine);
            toneUntilMs = xTaskGetTickCount() * portTICK_PERIOD_MS + 3000;
            ESP_LOGI(kTag, "テスト音: OPM ch0 キーオン (3 秒)");
        }
        const bool isToneDue =
            toneUntilMs != 0 && xTaskGetTickCount() * portTICK_PERIOD_MS >= toneUntilMs;
        if (isToneDue)
        {
            stopTestTone(g_machine);
            toneUntilMs = 0;
            ESP_LOGI(kTag, "テスト音: キーオフ");
        }

        const bool isAudioOn = g_audioEnabled.load(std::memory_order_relaxed);
        if (isAudioOn && g_audio.pending() < x68k_platform::AudioChannel::kBlockCount - 2)
        {
            static_cast<void>(x68k_platform::pumpAudio(g_machine, g_audio));
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
#if X68K_MEASURE_DISK
        const std::int64_t renderT0 = esp_timer_get_time();
#endif
        const bool rendered = g_display.renderTo(g_machine, g_textVram, g_frames.writeBuffer());
#if X68K_MEASURE_DISK
        g_renderUs += esp_timer_get_time() - renderT0;
        if (rendered)
        {
            ++g_renderCount;
        }
#endif
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
#if X68K_MEASURE_DISK
            // スライスの実時間の内訳。ディスクだけでは説明が付かないので、
            // Machine::run そのものに何 ms かかっているかも並べて出す。
            {
                // run の実時間は下の無条件ブロックが出す。ここでは描画だけ。
                // 両方で g_runUs を読むと、先に読んだ側がリセットして
                // もう一方が 0 を見る (実際に踏んだ)。
                const std::int64_t renderUs = g_renderUs;
                const std::uint32_t renders = g_renderCount;
                g_renderUs = 0;
                g_renderCount = 0;
                ESP_LOGI(kTag, "[render] %lldus 描画=%u 回 (5 秒)", renderUs, renders);
            }
            // ディスクに費やした実時間を実効クロックと並べて出す。
            // ディスクに触るスライスは全体の 1% 未満なので、平均の落ち込みが
            // 本当にディスク由来かは、逆算ではなくこの数字でしか分からない。
            {
                const std::int64_t readUs = x68k_platform::g_diskReadUs;
                const std::int64_t seekUs = x68k_platform::g_diskSeekUs;
                const std::uint32_t reqs = x68k_platform::g_diskReadCount;
                x68k_platform::g_diskReadUs = 0;
                x68k_platform::g_diskSeekUs = 0;
                x68k_platform::g_diskReadCount = 0;
                ESP_LOGI(kTag, "[disk] read=%lldus seek=%lldus req=%u (この 5 秒間)", readUs,
                         seekUs, reqs);
            }
#endif
            // イベント駆動が実際にどれだけ飛べているか。
            // 実効クロックだけでは縮退している区間が見えない。
            {
                const std::int64_t runUs = g_runUs;
                const std::uint32_t runs = g_runCount;
                g_runUs = 0;
                g_runCount = 0;
                ESP_LOGI(kTag, "[slice] run=%lldus n=%u (5 秒 = 5000000us)", runUs, runs);
            }
            {
                const auto& st = g_machine.schedulerStats();
                const unsigned long long far = st.armedFar;
                const unsigned long long avg = far != 0 ? st.spanSum / far : 0;
                ESP_LOGI(kTag,
                         "[sched] 遅い側=%llu 期限=%llu(平均%llucyc) 近すぎ=%llu 保留=%llu "
                         "rearm=%llu wake=%llu",
                         st.reaches, far, avg, st.armedNear, st.heldPending,
                         static_cast<unsigned long long>(st.rearms),
                         static_cast<unsigned long long>(st.wakes));
                g_machine.resetSchedulerStats();
            }
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

// リングから取り出してスピーカーへ流すタスク。Core0 で回す。
//
// Why Core0 か: Core1 はエミュレーションが 100% 使い切っている
// (実測でスライスの 80% が Machine::run)。ここを Core1 に置くと、
// 8 スライスに 1 回の vTaskDelay まで順番が回らず、その間 I2S の DMA が
// 枯れて音が途切れる。issue #8 が引く stackchan-dapan の知見
// (Wi-Fi/lwIP を Core0 へ pin しないと音が途切れる) と裏表の話で、
// 「音に関わるタスクを、詰まっているコアから追い出す」のが要点。
//
// Why not 表示ループ (app_main の while) に混ぜないか: あちらは
// 16ms ごとに 150KB を SPI へ流して待つ。その待ちの間はリングを
// 引けないので、1 ブロック 32.8ms に対して転送時間が無視できない。
// 別タスクなら転送中でも優先度で割り込める。
//
// Why not Machine を触らないか: 触らない。ここへ来るのは完成した
// サンプルだけで、frame_channel と同じ切り分けになっている。
void audioTask(void* /*arg*/)
{
    // 振幅を報告する間隔。実機では音を耳で確かめられないので、
    // 「キーオンしたら非ゼロ、待機中はゼロ」をログの数字で見る。
    //
    // Why 1 秒か: テスト音 ('!') は 3 秒しか鳴らない。5 秒間隔だと
    // 鳴っている区間と鳴っていない区間が同じ報告に混ざり、
    // 「待機中はゼロ」が確かめられない。
    constexpr std::uint32_t kReportIntervalMs = 1000;

    std::uint32_t lastReportMs = 0;
    std::int32_t peakSinceReport = 0;
    std::uint32_t blocksSinceReport = 0;

    while (true)
    {
        // リングにあるぶんを全部流す。
        //
        // ここで数えるために drainAudio ではなく自分で回す。振幅は
        // sink へ渡す前にしか見られない (playRaw から戻った後の
        // バッファは、次のブロックで上書きされうる)。
        while (const std::int16_t* block = g_audio.pop())
        {
            const std::int32_t peak =
                x68k_platform::peakAmplitude(block, x68k_platform::AudioChannel::kBlockFrames);
            if (peak > peakSinceReport)
            {
                peakSinceReport = peak;
            }
            ++blocksSinceReport;

            g_speaker.write(block, x68k_platform::AudioChannel::kBlockFrames);
        }

        const std::uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const bool isReportDue = now - lastReportMs >= kReportIntervalMs;
        if (isReportDue)
        {
            // ブロック数は実時間に追従しているかの検算になる。
            // 1 ブロック 512 サンプル / 15625Hz なので、正常なら 1 秒あたり
            // 約 30.5 ブロック。桁違いに多ければ playRaw が実際には
            // 何もしていない (speaker_m5.h の isEnabled 判定を見よ)。
            ESP_LOGI(kTag, "音声 %u ブロック/秒 (期待 30) 最大振幅 %d 取りこぼし %u",
                     static_cast<unsigned>(blocksSinceReport), static_cast<int>(peakSinceReport),
                     static_cast<unsigned>(g_audio.droppedBlocks()));
            lastReportMs = now;
            peakSinceReport = 0;
            blocksSinceReport = 0;
        }

        // 1 ブロックは 32.8ms ぶんの音。その 1/4 の周期で見に行けば、
        // Speaker が握れる 2 枚を切らさずに継ぎ足せる。
        //
        // Why not ブロックが積まれるまでブロッキングで待たないか:
        // セマフォを 1 本増やすことになるが、得られるのは 8ms ごとの
        // 空振り (リングを見て何も無ければ寝る) を省くことだけ。
        // 空振りのコストは atomic の読み 2 回で、測るまでもなく小さい。
        vTaskDelay(pdMS_TO_TICKS(8));
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

        // '|' で音源の ON/OFF を切り替える。
        //
        // Why not 焼き直して比べないか: 「音を足すとどれだけ遅くなるか」は
        // 同じ実行の中で比べないと、SD の中身や PSRAM の割り付けといった
        // 起動ごとの差が混ざる。1 文字で切り替えられれば、実効クロックの
        // 報告 (5 秒ごと) を挟んで前後を直接読み比べられる。
        //
        // Why '|' か: '~' '<' '>' と同じく Human68k のコマンドラインで
        // 使い道が薄く、取り上げても困らない。
        const bool isAudioToggle = c == '|';
        if (isAudioToggle)
        {
            const bool enabled = !g_audioEnabled.load();
            g_audioEnabled = enabled;
            ESP_LOGI(kTag, "音源: %s", enabled ? "ON" : "OFF");
            return;
        }

        // '&' で毎命令通る経路の最適化を切り替える。
        //
        // 音源の '|' と同じ理由で、焼き直さずに同じ起動の中で比べるため
        // にある。切り替えたら 5 秒ごとの「実効 NNNN kHz」を 20 回ほど
        // 読んで、収束後の平均を前後で比べる (起動直後は低めに出るので
        // 最初の数回は捨てる)。
        //
        // Why not 'p' のような文字か: 無条件に横取りする口 ('~' '|' '!'
        // '<' '>') は全て記号にしてある。英字を無条件で取ると Human68k へ
        // 打てなくなる (顔モード限定の 'e' 'S' 'h' は isFaceMode で
        // 守られているが、これは X68K モードでも使いたい)。
        const bool isPerfToggle = c == '&';
        if (isPerfToggle)
        {
            const bool enabled = !g_fastTickEnabled.load();
            g_fastTickEnabled = enabled;
            ESP_LOGI(kTag, "毎命令経路の最適化: %s", enabled ? "ON" : "OFF");
            return;
        }

        // '$' でイベント駆動を切り替える。
        //
        // '&' と同じく、焼き直さずに同じ起動の中で前後を比べるため。
        // **Human68k を A> まで起動させてから**測ること。ディスク無しだと
        // 同じ変更が +3.17% と +6.40% で倍違った実測がある。
        //
        // 切り替えても状態遷移は変わらない (ホストの同値テストが第 4 の軸
        // として固定している) ので、走らせたまま何度でも往復してよい。
        const bool isEventDrivenToggle = c == '$';
        if (isEventDrivenToggle)
        {
            const bool enabled = !g_eventDrivenEnabled.load();
            g_eventDrivenEnabled = enabled;
            ESP_LOGI(kTag, "イベント駆動: %s", enabled ? "ON" : "OFF");
            return;
        }

        // '%' で JIT の上限計測モードを切り替える (jit_probe.h)。
        //
        // 命令の実行を空回しにするので、**ゲストは止まって見える**。
        // 実効クロックの報告だけが意味を持つ。JIT に着手するかどうかを
        // 決めるための実測用で、恒久的な機能ではない。
        const bool isNullExecToggle = c == '%';
        if (isNullExecToggle)
        {
            const bool enabled = !g_nullExecProbe.load();
            g_nullExecProbe = enabled;
            ESP_LOGI(kTag, "JIT 上限計測モード: %s (ゲストは進みません)", enabled ? "ON" : "OFF");
            return;
        }

        // '_' で、上限計測モードの段を 1 つ進める。段 0-3 が全体の内訳
        // (tickDevices / 割り込み判定 / 床)、段 4-7 が tickDevices の内訳。
        //
        // 上限計測モード ('%') が ON のときだけ意味がある。
        const bool isSkipDevicesToggle = c == '_';
        if (isSkipDevicesToggle)
        {
            const int stage = (g_nullExecStage.load() + 1) % x68k::Machine::kNullExecStageCount;
            applyNullExecStage(stage);
            return;
        }

        // '#' で段 4 (MFP だけ外す) へ直接飛ぶ。
        //
        // Why not '_' の巡回だけで済ませないか: 段 4-7 は 5 秒ごとの報告を
        // 数回ぶん見てから次へ進めたい。巡回だけだと段 6 (CRTC) へ行くのに
        // '_' を 7 回押すことになり、その間に温度と SD の状態が変わる。
        // 内訳は同一起動の中で連続して測らないと差が揺れに埋もれる
        // (焼き直しの揺れ 3711 vs 3630 kHz を一度踏んでいる)。
        const bool isBreakdownJump = c == '#';
        if (isBreakdownJump)
        {
            applyNullExecStage(4);
            return;
        }

        // '@' で MFP タイマの設定を出す。
        //
        // イベント駆動の設計は「次に状態が変わるまで何サイクルあるか」で
        // 決まるので、ゲストが実際にどの分周でタイマを回しているかが
        // そのまま効果を左右する。推測せずに実機から読む。
        const bool isMfpDump = c == '@';
        if (isMfpDump)
        {
            const auto& m = g_machine.mfp();
            ESP_LOGI(kTag, "MFP TACR=%02X TBCR=%02X TCDCR=%02X", m.peek(x68k::Mfp::kTacr),
                     m.peek(x68k::Mfp::kTbcr), m.peek(x68k::Mfp::kTcdcr));
            ESP_LOGI(kTag, "MFP TADR=%02X TBDR=%02X TCDR=%02X TDDR=%02X", m.peek(x68k::Mfp::kTadr),
                     m.peek(x68k::Mfp::kTbdr), m.peek(x68k::Mfp::kTcdr), m.peek(x68k::Mfp::kTddr));
            ESP_LOGI(kTag, "MFP IERA=%02X IERB=%02X IMRA=%02X IMRB=%02X", m.peek(x68k::Mfp::kIera),
                     m.peek(x68k::Mfp::kIerb), m.peek(x68k::Mfp::kImra), m.peek(x68k::Mfp::kImrb));

            // SRAM の起動デバイスと、CPU が今どこを走っているか。
            //
            // ホストで起動するイメージが実機で起動しないとき、SRAM の
            // 設定 (SD に永続化される) と PC の居場所で切り分けられる。
            const auto& sr = g_machine.sram();
            ESP_LOGI(kTag, "SRAM 起動デバイス=%04X 画面モード=%02X マジック有効=%d",
                     static_cast<unsigned>(sr.read16(x68k::Sram::kOffsetBootDevice)),
                     sr.read8(x68k::Sram::kOffsetScreenMode),
                     g_machine.sram().hasValidMagic() ? 1 : 0);
            ESP_LOGI(kTag, "CPU PC=%08X SR=%04X halted=%d", g_machine.cpu().state().pc,
                     g_machine.cpu().state().sr, g_machine.isHalted() ? 1 : 0);

            // 画面モードと表示許可。描画が 1 回 23ms かかる原因が
            // グラフィック合成かテキストのみかを切り分けるために出す。
            // $E82600 の bit5 がテキスト、bit4-0 がグラフィックの表示許可。
            const auto& v = g_machine.video();
            ESP_LOGI(kTag, "VIDEO 画面モード=%04X 表示制御=%04X プライオリティ=%04X",
                     v.screenMode(), v.displayControl(), v.priority());
            return;
        }

        // '=' でイベント駆動のまま命令実行だけを空回しにする (計測用)。
        // ゲストは止まって見える。恒久的な機能ではない。
        const bool isEventNullExec = c == '=';
        if (isEventNullExec)
        {
            const bool on = !g_eventNullExec.load();
            g_eventNullExec = on;
            g_machine.setNullExecInEvent(on);
            ESP_LOGI(kTag, "イベント駆動のまま命令を空回し: %s", on ? "ON" : "OFF");
            return;
        }

        // 'n' でネイティブ発行の上限を測る。恒久機能ではない。
        //
        // 「命令の意味を最速で実行したら何 ns か」を知りたい。
        // 生成したネイティブコードを実行可能メモリに置いて、
        // ゲスト 1 命令ぶんに相当する仕事 (レジスタ間 MOVE) を
        // 大量に回して測る。インタプリタの 85 CPU サイクル/命令と
        // 比べれば、ネイティブ化で何倍になりうるかが出る。
        // 'C' で段 0-C2 (キャッシュ経路の費用) を測る。恒久機能ではない。
        // 'B' で段 0-C3 (ネイティブ命令本体の実費) を測る。恒久機能ではない。
        // 'J' で JIT を切り替える。**同じ起動の中で往復して測れる**ので、
        // ビルド差やキャッシュの温まり方が結果に混じらない。
        const bool isJitToggle = c == 'J';
        if (isJitToggle)
        {
            const bool on = !g_machine.cpu().hasNativeExec();
            if (on)
            {
                if (g_jitSlots == nullptr || !g_jitCode.isReady())
                {
                    ESP_LOGW(kTag, "JIT: 置き場が無いので有効にできません");
                    return;
                }
                g_jitRunner.setStorage(g_jitSlots, kJitSlots, &g_jitCode);
                g_jitRunner.reset();
                g_machine.cpu().setNativeExec(g_jitRunner.exec());
                // **JIT はイベント駆動の経路にしか無い。**
                // 毎命令 tick のまま JIT を ON にしても何も起きず、
                // 「ON にしたのに統計が 0」という紛らわしい状態になる。
                // 沈黙の無効化を作らないよう、ここで一緒に ON にする。
                if (!g_eventDrivenEnabled.load(std::memory_order_relaxed))
                {
                    g_eventDrivenEnabled = true;
                    ESP_LOGI(kTag, "JIT: イベント駆動も ON にしました");
                }
            }
            else
            {
                g_machine.cpu().setNativeExec(x68k::NativeExec{});
            }
            ESP_LOGI(kTag, "JIT: %s", on ? "ON" : "OFF");
            return;
        }

        // 'K' で JIT の統計を出す。
        const bool isJitStats = c == 'K';
        if (isJitStats)
        {
            const x68k::NativeStats* st = g_machine.cpu().nativeStats();
            if (st == nullptr)
            {
                ESP_LOGI(kTag, "JIT: 教わっていません");
                return;
            }
            ESP_LOGI(kTag, "[jit] ブロック %llu 本 / 命令 %llu", (unsigned long long)st->blocksRun,
                     (unsigned long long)st->insnsRun);
            ESP_LOGI(kTag, "[jit] 落とした: 割込 %llu / 未対応 %llu / 翻訳失敗 %llu",
                     (unsigned long long)st->deferInterrupt,
                     (unsigned long long)st->deferUnsupported,
                     (unsigned long long)st->translateFail);
            ESP_LOGI(kTag, "[jit] 鍵外れ: タグ %llu / 写像 %llu / 世代 %llu / 飽和 %llu",
                     (unsigned long long)st->keyMissTag, (unsigned long long)st->keyMissEpoch,
                     (unsigned long long)st->keyMissGen, (unsigned long long)st->keyMissStale);
            // **統計が 0 のときに自己診断できるようにする。**
            // 経路が違えば tryNative は呼ばれないので、統計は 0 のまま。
            ESP_LOGI(kTag, "[jit] 経路: イベント駆動 %s / JIT %s",
                     g_machine.eventDriven() ? "ON" : "OFF",
                     g_machine.cpu().hasNativeExec() ? "ON" : "OFF");
            ESP_LOGI(kTag, "[jit] 実行可能メモリ %u / %u バイト", (unsigned)g_jitCode.used(),
                     (unsigned)g_jitCode.capacity());
            return;
        }

        // 'L' で「JIT の経路だけ」を最小コードで試す。恒久機能ではない。
        //
        // 生成コードを疑うか、経路 (実行可能メモリ / ゲートウェイ / isync) を
        // 疑うかを切り分ける。**ret.n 1 命令だけ**を置いて呼ぶ。
        // これが落ちるなら経路が悪い。通るなら生成コードが悪い。
        const bool isMinimalBlockProbe = c == 'L';
        if (isMinimalBlockProbe)
        {
            if (!g_jitCode.isReady())
            {
                ESP_LOGW(kTag, "[probe] 実行可能メモリが無い");
                return;
            }
            // **段 0 のプローブと同じ形でその場で確保する。**
            // 起動時に確保した ExecMemory と、その場で確保したものを
            // 比べれば「いつ確保したか」が効くかが分かる。
            auto* fresh = static_cast<std::uint8_t*>(
                heap_caps_malloc(64, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT));
            ESP_LOGI(kTag, "[probe] 起動時=%p その場=%p", (void*)g_jitCode.base(), (void*)fresh);
            if (fresh == nullptr)
            {
                ESP_LOGW(kTag, "[probe] その場で確保できない");
                return;
            }
            std::uint8_t* p = fresh;
            auto* dst = reinterpret_cast<volatile std::uint32_t*>(p);

            // --- A: windowed で書いて普通の関数ポインタで呼ぶ (段 0 と同じ) ---
            // entry a1,32 / movi.n a2,10 / retw.n
            dst[0] = 0x0C004136u;
            dst[1] = 0x00F01DA2u;
            __asm__ __volatile__("isync" ::: "memory");
            using FnW = int (*)();
            const int rw = reinterpret_cast<FnW>(p)();
            ESP_LOGI(kTag, "[probe] A windowed = %d (10 なら成功)", rw);

            // --- B: call0 で書いて runBlock で呼ぶ ---
            alignas(4) std::uint8_t stage[8] = {};
            std::size_t n = x68k::jit::movi(stage, 2, 4);
            n += x68k::jit::retN(stage + n);
            const auto* srcw = reinterpret_cast<const std::uint32_t*>(stage);
            dst[0] = srcw[0];
            dst[1] = srcw[1];
            __asm__ __volatile__("isync" ::: "memory");
            ESP_LOGI(kTag, "[probe] B call0 %u バイト。まず素の callx0 で呼ぶ", (unsigned)n);

            // --- B1: 退避なしの素の callx0 ---
            // ゲートウェイの退避が悪いのか、callx0 そのものが悪いのかを分ける。
            {
                register std::uint32_t arg __asm__("a2") = 0;
                register const void* tgt __asm__("a3") = p;
                __asm__ __volatile__("callx0 %1" : "+r"(arg) : "r"(tgt) : "a0", "memory");
                ESP_LOGI(kTag, "[probe] B1 素の callx0 = %u (4 なら成功)", (unsigned)arg);
            }

            // --- B2: runBlock 経由 ---
            // **static にする。** 84 バイトの M68kState をこのハンドラの
            // スタックへ置くと、シリアルタスクのスタックが足りない疑いがある。
            // B2 の直前に、B1 と同じ形をもう一度だけ実行する。
            // 「1 回目は通るが 2 回目で落ちる」なら isync / キャッシュの話。
            {
                register std::uint32_t arg __asm__("a2") = 0;
                register const void* tgt __asm__("a3") = p;
                __asm__ __volatile__("callx0 %1" : "+r"(arg) : "r"(tgt) : "a0", "memory");
                ESP_LOGI(kTag, "[probe] B1b 2 回目 = %u", (unsigned)arg);
            }

            static x68k::M68kState dummy{};
            const std::uint32_t r = x68k::jit::runBlock(&dummy, p);
            ESP_LOGI(kTag, "[probe] B2 runBlock = %u (4 なら成功)", (unsigned)r);
            return;
        }

        const bool isNativeBodyProbe = c == 'B';
        if (isNativeBodyProbe)
        {
            probeNativeBody();
            return;
        }

        const bool isCachePathProbe = c == 'C';
        if (isCachePathProbe)
        {
            probeCachePathCost();
            return;
        }

        const bool isNativeProbe = c == 'n';
        if (isNativeProbe)
        {
            probeNativeCeiling();
            return;
        }

        // 'j' で JIT の実現性を確かめる。恒久機能ではない。
        //
        // 記録には「CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y の間は
        // MALLOC_CAP_EXEC が必ず失敗する」とあるが、現在の sdkconfig では
        // **その設定は無効になっている**。前提が変わっているので測り直す。
        const bool isJitProbe = c == 'j';
        if (isJitProbe)
        {
            probeJitFeasibility();
            // どれだけ取れるかも見る。コードキャッシュの設計はここで決まる。
            {
                const std::size_t largest =
                    heap_caps_get_largest_free_block(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
                const std::size_t total =
                    heap_caps_get_free_size(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
                ESP_LOGI(kTag, "[jit] 実行可能メモリ 空き=%u 最大連続=%u",
                         static_cast<unsigned>(total), static_cast<unsigned>(largest));
            }
            return;
        }

        // 'c' でキャッシュのアクセス数とミス数を出す。恒久機能ではない。
        //
        // 88 CPU サイクル/命令が「1 つの重い処理」なのか「合算」なのかは、
        // ミス率を見れば分かる。I-cache のミスが多ければコード配置、
        // PSRAM のミスが多ければメインメモリのアクセスが律速。
        //
        // Why not 顔モードの 'e' 等と衝突しないか: あちらは isFaceMode で
        // 守られている。ここは X68K モードでも使いたいので、記号ではなく
        // 英字だが、Human68k のコマンドラインで 'c' 単独を打つ機会は
        // 実質無い (打ちたければ切ってから測る)。
        const bool isCacheStats = c == 'c';
        if (isCacheStats)
        {
            const uint32_t ibusAcs = REG_READ(EXTMEM_IBUS_ACS_CNT_REG);
            const uint32_t ibusMiss = REG_READ(EXTMEM_IBUS_ACS_MISS_CNT_REG);
            const uint32_t dbusAcs = REG_READ(EXTMEM_DBUS_ACS_CNT_REG);
            const uint32_t dbusFlashMiss = REG_READ(EXTMEM_DBUS_ACS_FLASH_MISS_CNT_REG);
            const uint32_t dbusRamMiss = REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG);
            ESP_LOGI(kTag, "[cache] ibus acs=%lu miss=%lu (%.2f%%)", (unsigned long)ibusAcs,
                     (unsigned long)ibusMiss,
                     ibusAcs != 0 ? 100.0 * (double)ibusMiss / (double)ibusAcs : 0.0);
            ESP_LOGI(kTag, "[cache] dbus acs=%lu flashMiss=%lu spiramMiss=%lu (%.2f%%)",
                     (unsigned long)dbusAcs, (unsigned long)dbusFlashMiss,
                     (unsigned long)dbusRamMiss,
                     dbusAcs != 0 ? 100.0 * (double)(dbusFlashMiss + dbusRamMiss) / (double)dbusAcs
                                  : 0.0);
            // 次の測定のために 0 へ戻す。
            REG_WRITE(EXTMEM_CACHE_ACS_CNT_CLR_REG, 0x3);
            REG_WRITE(EXTMEM_CACHE_ACS_CNT_CLR_REG, 0x0);
            return;
        }

        // '!' で OPM のテスト音を 3 秒鳴らす。
        //
        // 実機では音を耳で確かめられないので、これと 5 秒ごとの
        // 「最大振幅」のログを組にして、鳴っていることを数字で見る。
        // 実際に鳴らすのはエミュレーションコア (Opm の所有者)。
        const bool isTestTone = c == '!';
        if (isTestTone)
        {
            g_testToneRequested = true;
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

        // ここから下は顔モード専用の操作。
        //
        // Why X68K モードでは受け付けないか: X68K モードで打った文字は
        // Human68k へ流れる。'e' を横取りすると `dir e*` のような
        // コマンドが打てなくなる。顔モードなら打った文字の行き先が
        // 無いので、取り上げても失うものが無い。
        const bool isFaceMode = g_mode.mode() == x68k_platform::AppMode::Face;

        // 'e' で表情を順に送る。実機の画面を直接見られないので、
        // 表情が変わったことはログ (logFaceState) で確かめる。
        const bool isExpressionCycle = isFaceMode && c == 'e';
        if (isExpressionCycle)
        {
            const auto next = static_cast<x68k_platform::FaceExpression>(
                (static_cast<std::size_t>(g_avatar.expression()) + 1) %
                x68k_platform::kFaceExpressionCount);
            g_avatar.setExpression(next);
            // 表情はまばたきと違って自分では変わらない。tick が
            // 「変化なし」を返すので、ここで描き直しておかないと
            // 次のまばたきまで新しい表情が画面に出ない。
            drawFaceNow();
            logFaceState();
            return;
        }

        // 'S' でサーボ (LEDC) を張る。既定は NullServo。
        const bool isServoEnable = isFaceMode && c == 'S';
        if (isServoEnable)
        {
            enableLedcServo();
            return;
        }

        // 'h'/'l'/'k'/'j' で首を振る。vi の向きに合わせてある。
        //
        // Why 手で振れるようにするか: サーボが繋がっているかは
        // ソフトウェアからは分からない (servo.h の冒頭)。動いたことを
        // 確かめる手立ては目で見るしかないので、任意の角度を
        // 出せる口が要る。
        const bool isHeadMove = isFaceMode && (c == 'h' || c == 'l' || c == 'k' || c == 'j');
        if (isHeadMove)
        {
            moveHead(c);
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

// 顔を 1 枚描いて LCD へ送る。表示コアから呼ぶ。
//
// Why DisplayLcd::pushFrame を通すか: 顔のスプライトは PSRAM にある。
// M5GFX へ生のポインタを渡す経路は、入力も出力も RGB565 だと
// 「変換不要」と判断して PSRAM のアドレスをそのまま DMA descriptor へ
// 入れる。ESP32-S3 は CPU キャッシュと PSRAM の同期をハードウェアで
// 保証しないので、転送しても画面が変わらない
// (docs/knowledge/cores3-emulator-runtime.md の 5 節)。
// pushFrame は setSwapBytes(true) を前提に組んであり、この罠を既に
// 解いてある。X68000 の画面と同じ口を通せば踏み直さずに済む。
void drawFaceNow()
{
    if (!g_avatar.render())
    {
        // スプライトが取れなかった。顔は出せないので、そうと分かる
        // 表示を出す。黒画面のまま放置すると「固まった」と読めてしまう。
        x68k_platform::DisplayLcd::showMessage("FACE", "no sprite buffer");
        return;
    }
    g_display.pushFrame(g_avatar.spriteBuffer());
}

// 顔の状態をシリアルへ出す。
//
// Why 要るか: 実機の LCD は直接見られない。顔が動いているかを確かめる
// 手立てが「人に写真を撮ってもらう」しか無いと、まばたきが止まっていても
// 気付けない。まぶたの開き具合と表情をログに出しておけば、テキスト画面を
// text_scrape で読み戻すのと同じように、シリアルだけで挙動を判定できる。
//
// Why not 毎コマ出さないか: まばたき中は 16ms ごとに変化するので、
// 全部出すとログがそれで埋まり、エミュレーションの進捗報告が読めなくなる。
// 段階の変わり目だけ出せば、まばたきの回数と間隔は再構成できる。
void logFaceState()
{
    static x68k_platform::BlinkPhase lastPhase = x68k_platform::BlinkPhase::Open;
    static x68k_platform::FaceExpression lastExpression = x68k_platform::FaceExpression::Neutral;

    const x68k_platform::BlinkPhase phase = g_avatar.blinkPhase();
    const x68k_platform::FaceExpression expression = g_avatar.expression();

    const bool hasChanged = phase != lastPhase || expression != lastExpression;
    if (!hasChanged)
    {
        return;
    }
    lastPhase = phase;
    lastExpression = expression;

    const char* phaseName = "開";
    if (phase == x68k_platform::BlinkPhase::Closing)
    {
        phaseName = "閉じ中";
    }
    else if (phase == x68k_platform::BlinkPhase::Opening)
    {
        phaseName = "開き中";
    }

    static const char* const kExpressionNames[] = {"素", "笑", "眠", "驚"};

    ESP_LOGI(kTag, "顔: まぶた=%s(%u) 表情=%s まばたき=%u コマ=%u 次まで=%ums", phaseName,
             static_cast<unsigned>(g_avatar.lidClosure()),
             kExpressionNames[static_cast<std::size_t>(expression)],
             static_cast<unsigned>(g_avatar.blinkCount()),
             static_cast<unsigned>(g_avatar.frameCount()),
             static_cast<unsigned>(g_avatar.msUntilBlink()));
}

// サーボを LEDC の実装へ差し替える。
//
// 一度張ったら戻さない。戻す口を作ると、LEDC を解放してからもう一度
// 張る手順が要り、失敗したときにどちらでもない状態が残る。
void enableLedcServo()
{
    const bool isAlreadyEnabled = g_servo == &g_ledcServo;
    if (isAlreadyEnabled)
    {
        ESP_LOGI(kTag, "サーボは既に有効です");
        return;
    }

    // CoreS3 の Port.A。スタックチャンの作例が最も多く使う配線。
    g_ledcServo.setPins(1, 2);
    if (!g_ledcServo.begin())
    {
        ESP_LOGE(kTag, "サーボを有効にできません。NullServo のままにします");
        return;
    }

    g_servo = &g_ledcServo;
    // 正面へ向ける。付いていれば首が中央に来る。
    g_servo->setPose({});
    ESP_LOGI(kTag, "サーボ有効。h/l で左右、k/j で上下 (pan duty=%u tilt duty=%u)",
             static_cast<unsigned>(g_ledcServo.lastPanDuty()),
             static_cast<unsigned>(g_ledcServo.lastTiltDuty()));
}

// 首を 1 段階動かす。
void moveHead(char key)
{
    // 1 回あたりの角度。
    //
    // Why 15 度か: 可動域が ±45 度なので、端まで 3 回で届く。
    // 細かくすると端へ持っていくのに何度も打つことになり、
    // 動いたかどうかを見るための操作としては使いにくい。
    constexpr float kStepDeg = 15.0F;

    // 今の指令値から動かす。
    //
    // Why not 絶対角を打たせないか: 実機で確かめたいのは「打つたびに
    // 首が動くか」で、相対の方が操作が短い。
    x68k_platform::HeadPose pose =
        g_servo == &g_ledcServo ? g_ledcServo.lastPose() : g_nullServo.lastPose();

    if (key == 'h')
    {
        pose.pan -= kStepDeg;
    }
    else if (key == 'l')
    {
        pose.pan += kStepDeg;
    }
    else if (key == 'k')
    {
        pose.tilt += kStepDeg;
    }
    else if (key == 'j')
    {
        pose.tilt -= kStepDeg;
    }

    g_servo->setPose(pose);

    // 何を出したかをログへ出す。
    //
    // 【重要】これは「指令」であって「動いた証拠」ではない。サーボの
    // 信号線は応答を返さないので、繋がっているかも回ったかも
    // ソフトウェアからは分からない (servo.h の冒頭)。
    if (g_servo == &g_ledcServo)
    {
        ESP_LOGI(kTag, "首: pan=%d 度 tilt=%d 度 duty pan=%u tilt=%u (指令のみ。動作は未確認)",
                 static_cast<int>(g_ledcServo.lastPose().pan),
                 static_cast<int>(g_ledcServo.lastPose().tilt),
                 static_cast<unsigned>(g_ledcServo.lastPanDuty()),
                 static_cast<unsigned>(g_ledcServo.lastTiltDuty()));
        return;
    }
    ESP_LOGI(kTag, "首: pan=%d 度 tilt=%d 度 (サーボ無効。指示は捨てられた)",
             static_cast<int>(g_nullServo.lastPose().pan),
             static_cast<int>(g_nullServo.lastPose().tilt));
}

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
        // 顔を 1 枚描いて出す。以後は表示ループが毎コマ更新する。
        //
        // ここで 1 回描くのは、切り替えた瞬間に X68000 の画面が残る間を
        // 作らないため。表示ループの周期 (約 16ms) を待つと、その 1 コマ
        // ぶん前のモードの画面が見える。
        drawFaceNow();
        // 正面へ戻す。NullServo は捨てる。
        g_servo->setPose({});
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
        g_servo->detach();
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

    // スピーカーを開く。
    //
    // 開けなくても起動する。音が出ないだけで X68000 は動くので、
    // ここで止める理由がない (グラフィック VRAM と同じ扱い)。
    g_speakerReady = g_speaker.begin();
    if (g_speakerReady)
    {
        ESP_LOGI(kTag, "スピーカー: %u Hz (音源は %s。'|' で切り替え)",
                 static_cast<unsigned>(x68k_platform::M5SpeakerSink::kSampleRate),
                 g_audioEnabled.load() ? "ON" : "OFF");
    }
    else
    {
        ESP_LOGW(kTag, "スピーカーを開けません。音は出ませんが X68000 は動きます");
        // 合成しても捨てるだけなので、エミュレーションコアの手間を省く。
        g_audioEnabled = false;
    }

    // 顔 ⇄ X68000 の切り替えを用意する。
    //
    // 既定は X68K モード (app_mode.h)。起動直後に見たいのは Human68k が
    // 立ち上がったかどうかで、顔から始めると確かめるのに切り替えが要る。
    //
    // ここで applyModeTransition を通すのは、g_allowedSliceCycles を
    // 初期化するため。0 のまま残すとエミュレーションが 1 命令も進まず、
    // 利用者にはフリーズとしか見えない。
    // 起動直後に押さえたスプライトを渡す。
    //
    // nullptr なら顔は出せない。起動を止めるほどではない (X68000 は動く)
    // ので、そうと分かるログを出して続ける。
    g_avatar.setSpriteBuffer(g_avatarSprite);
    if (!g_avatar.hasSpriteBuffer())
    {
        ESP_LOGW(kTag, "顔のバッファがありません。顔モードは絵を出しません");
    }

    // サーボは既定で NullServo (何もしない)。'S' を打つと LEDC を張る。
    //
    // Why not 起動時に張らないか: Port.A (GPIO 1/2) は M5Unified が
    // 外部 I2C にも使う。Port.A に I2C の Unit を挿している環境で
    // 勝手に PWM を出すと、I2C のバスへ矩形波を流し込むことになる
    // (servo.h の冒頭)。使う人が明示的に選んだときだけ張る。
    g_servo->begin();
    ESP_LOGI(kTag, "サーボ: 既定は無効 (NullServo)。'S' で LEDC を張ります");

    applyModeTransition(g_mode.request(x68k_platform::ModeRequest::ToX68k));
    ESP_LOGI(kTag, "Tab で顔 ⇄ X68000、'e' で表情、'S' でサーボ");

    // シリアルからキーを拾えるようにする。
    //
    // ログ出力は VFS 経由のままにして、入力だけドライバから直接読む。
    // 両方をドライバに寄せると ESP_LOG の行が混ざって読めなくなる。
    usb_serial_jtag_driver_config_t usbCfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const bool isConsoleReady = usb_serial_jtag_driver_install(&usbCfg) == ESP_OK;
    if (isConsoleReady)
    {
        ESP_LOGI(kTag,
                 "シリアルコンソール: 文字を打つと X68000 へ、'~' で画面をダンプ、"
                 "'|' で音源の ON/OFF、'&' で毎命令経路の最適化の ON/OFF、"
                 "'$' でイベント駆動の ON/OFF");
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

    // 音声は Core0 で流す (合成は Core1。audioTask の冒頭を見よ)。
    //
    // 優先度は表示ループ (app_main、既定 1) より高い 4 にする。表示は
    // 16ms ごとに 150KB を SPI へ流すので、同じなら転送の間 DMA の補充が
    // 止まる。エミュレーション (5) より低くするのは、音のために 68000 の
    // 実行を止めないため。1 ブロック 32.8ms に対して、このタスクの
    // 実際の仕事は playRaw を数回呼ぶだけで、8 スライスに 1 回の
    // vTaskDelay(1) で十分に順番が回ってくる。
    //
    // スタック 4096 は playRaw が再帰も大きなローカルも持たないため
    // (Speaker_Class.cpp の _set_next_wav は wav_info_t 1 個)。
    if (g_speakerReady)
    {
        const BaseType_t audioCreated =
            xTaskCreatePinnedToCore(audioTask, "x68k_audio", 4096, nullptr, 4, nullptr, 0);
        if (audioCreated != pdPASS)
        {
            // 起動は続ける。音が出ないだけで X68000 は動く。
            // 合成しても引き取り手がいないので、そちらも止める。
            ESP_LOGW(kTag, "音声タスクを作れません。音は出ませんが X68000 は動きます");
            g_audioEnabled = false;
            g_speakerReady = false;
        }
    }

    // Core0 は画面と入力を担当する。
    bool isHalted = false;
    // 顔のアニメーションに渡す実時間の基準。
    std::int64_t lastTickUs = esp_timer_get_time();
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

        // 顔を動かす。
        //
        // Why 実時間を渡すか: このループは vTaskDelay(16) を挟むが、
        // フレームの転送 (320x240 の SPI) やモード切り替えで実際の間隔は
        // 揺れる。コマ数で数えるとまばたきの速さが負荷に引きずられる。
        // 経過した実時間を渡せば、コマが飛んでも間隔は保たれる
        // (avatar.h の tick のコメントを見よ)。
        const std::int64_t nowUs = esp_timer_get_time();
        const std::uint32_t elapsedMs = static_cast<std::uint32_t>((nowUs - lastTickUs) / 1000);
        if (elapsedMs > 0)
        {
            lastTickUs = nowUs;
            const bool isFaceVisible = g_mode.mode() == x68k_platform::AppMode::Face;
            const bool hasChanged = g_avatar.tick(elapsedMs);
            // 顔モードのときだけ描く。
            //
            // Why not 顔モード以外でも tick を回すか: 回している。まばたきの
            // 位相を進めておけば、切り替えた瞬間に「ずっと目を開けたまま
            // だった顔」から始まらない。描画と転送だけを顔モードに絞る。
            if (isFaceVisible && hasChanged)
            {
                drawFaceNow();
                logFaceState();
            }
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
