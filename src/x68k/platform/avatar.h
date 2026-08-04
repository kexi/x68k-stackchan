// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// スタックチャンの顔。まばたきと表情を持つ。
//
// 描くのは 320x240 の RGB565 1 枚で、内容は「黒地に白い目と口」。
// スタックチャンの顔は M5Stack-Avatar が定めた見た目 (角の丸い白い目、
// 表情で変わる口、一定間隔のまばたき) がそのまま「らしさ」なので、
// 形と間隔をそこへ寄せてある。
//
// 【描画先の方針】画素は自前で置く。M5GFX の図形 API を使わない。
//
// Why not M5.Display.fillCircle 等で直接描かないか (3 つある):
//
// 1. 顔の見た目をホストのテストで検査できなくなる。M5GFX は実機の
//    パネルドライバに繋がっており、ホストには持ち込めない。目の位置も
//    まばたきの開き具合も「実機に焼いて目で見る」しか確かめようが
//    なくなり、実機の画面を直接見られないこのプロジェクトでは
//    事実上検査できないのと同じになる。RGB565 の配列へ置く形にすれば、
//    画素を読んで「左目が閉じている」を機械的に判定できる。
//
// 2. LCD へ直接描くとちらつく。まばたきは 1 コマごとに目を描き直す。
//    直接描くと「消す→描く」の間が画面に出るので、瞬きのたびに
//    顔が明滅する。1 枚を作ってから送れば、途中の状態は出ない。
//
// 3. 転送経路を X68000 の画面と揃えられる。この顔は 320x240 の
//    RGB565 で、DisplayLcd::pushFrame がそのまま送れる形になっている。
//    PSRAM のバッファを DMA へ直接渡す罠 (docs/knowledge/
//    cores3-emulator-runtime.md の 5 節) は pushFrame 側で既に
//    解いてあるので、同じ口を通せば新しく踏み直さずに済む。
//
// Why not M5Stack-Avatar をそのまま持ってこないか (元からある理由、今も有効):
//
// 1. Avatar は自前で描画タスクを立てる。M5Stack-Avatar の Avatar::init は
//    FreeRTOS のタスクを起こし、そのタスクが M5.Display を直接叩き続ける。
//    このプロジェクトの表示は既に「エミュレーションコアが作った RGB565 を
//    表示コアが送る」という所有権で組んであり (frame_channel.h)、
//    3 つ目の書き手が同じ LCD を触ると SPI の操作が競合する。
//
// 2. 依存を増やさずに済む。要るのは目と口とまばたきだけで、
//    M5Stack-Avatar が持つ吹き出し・バッテリ表示・色テーマは使わない。

#ifndef X68K_PLATFORM_AVATAR_H
#define X68K_PLATFORM_AVATAR_H

#include <cstddef>
#include <cstdint>

namespace x68k_platform
{

// 顔の表情。
//
// 目と口の両方を変える。
//
// Why not 口だけ変えないか: 2 インチの画面では口が小さく、口だけの差は
// 判別できない。目の形まで変えて初めて「笑っている」と分かる。
enum class FaceExpression : std::uint8_t
{
    // 素の顔。丸い目と横一文字の口。
    Neutral,
    // 笑い。目が上に凸の弧 (笑い目) になり、口が大きく開く。
    Happy,
    // 眠い。上まぶたが半分降り、口が小さくなる。
    Sleepy,
    // 驚き。目を見開き、口が丸く開く。
    Surprised,
};

// 表情の数。テストが全表情を回すために使う。
inline constexpr std::size_t kFaceExpressionCount = 4;

// まばたきの段階。
//
// ログに出して実機の挙動を追うために公開する。実機の画面は直接見られない
// ので、「今まぶたが降りている」をシリアルで確かめられる必要がある。
enum class BlinkPhase : std::uint8_t
{
    // 目は開いている。次のまばたきを待っている。
    Open,
    // まぶたが降りている途中。
    Closing,
    // まぶたが上がっている途中。
    Opening,
};

class Avatar
{
public:
    // 顔の大きさ。LCD と同じ 320x240。
    //
    // Why not DisplayLcd::kScreenWidth を参照しないか: display_lcd.h は
    // machine.h を引き込み、machine.h は core/ のほぼ全部を引き込む。
    // 顔は X68000 のエミュレーションを 1 バイトも知らなくてよいので、
    // ここで切っておけばホストのテストが core/ をリンクせずに済む。
    // 値がずれると転送の大きさが合わないため、main.cpp が static_assert で
    // 食い違いを検出する。
    static constexpr std::uint32_t kWidth = 320;
    static constexpr std::uint32_t kHeight = 240;

    // 顔に使うスプライトの枠。320x240 の RGB565 で 150KB。
    //
    // Why 全画面ぶん取るか: 顔は画面いっぱいに描く。部分更新にすると
    // まばたきのたびに背景との合成が要り、ちらつきを避けるには結局
    // 画面ぶんのバッファが要る。フレームバッファ (frame_channel) と
    // 同じ大きさなので、見積もりも揃う。
    //
    // Why not フレームバッファを使い回さないか: 使い回すと、顔モードの
    // 間 X68000 の変換先が無くなる。KeepRunning を既定にした以上
    // (app_mode.h の EmulationPolicy を見よ)、顔を出している間も
    // エミュレーションは画面を作り続ける。作れないと、戻ったときに
    // 全画面を作り直すまで古い画面が残る。
    static constexpr std::size_t kSpriteBytes =
        static_cast<std::size_t>(kWidth) * kHeight * sizeof(std::uint16_t);

    // 顔を描くバッファを受け取る。nullptr でもよい。
    //
    // nullptr のときは描画できない (render が false を返す)。呼ぶ側は
    // 顔の代わりに別の表示を出す。
    void setSpriteBuffer(std::uint16_t* buffer);

    [[nodiscard]] bool hasSpriteBuffer() const
    {
        return sprite_ != nullptr;
    }

    [[nodiscard]] std::uint16_t* spriteBuffer() const
    {
        return sprite_;
    }

    // 表情を変える。同じ表情なら何もしない。
    void setExpression(FaceExpression expression);

    [[nodiscard]] FaceExpression expression() const
    {
        return expression_;
    }

    // 時間を進める。elapsedMs は前回の呼び出しからの実時間 (ミリ秒)。
    //
    // 見た目が変わったなら true を返す。呼ぶ側は true のときだけ
    // render() と転送を行えばよい。
    //
    // Why not フレーム数で数えないか: 表示コアのループは vTaskDelay(16) を
    // 挟んでいるが、モード切り替えやフレームの受け渡しで実際の間隔は
    // 揺れる。フレーム数で数えると、負荷が上がったときにまばたきが
    // 遅くなる。実時間で数えれば、コマ落ちしても間隔は保たれる。
    //
    // Why not 内部で時刻を読まないか: 時刻の取得は ESP-IDF
    // (esp_timer_get_time) に依存する。外から渡せば、ホストのテストが
    // 任意の時間を刻めて、まばたきの間隔を実時間を待たずに検査できる。
    bool tick(std::uint32_t elapsedMs);

    // 今の状態で顔を 1 枚描く。バッファが無ければ false。
    //
    // tick が false を返した後でも呼べる。モード切り替え直後のように、
    // 状態が変わっていなくても描き直したい場面がある。
    bool render();

    // --- 状態の覗き口 (テストとシリアルログのため) ---

    [[nodiscard]] BlinkPhase blinkPhase() const
    {
        return blinkPhase_;
    }

    // まぶたの降り具合。0 = 全開、255 = 完全に閉じている。
    //
    // Why not 0.0-1.0 の float にしないか: ログへ出す値で、実機の printf は
    // 既定で浮動小数を落とすことがある (CONFIG_NEWLIB_NANO_FORMAT)。
    // 整数なら書式で悩まない。
    [[nodiscard]] std::uint8_t lidClosure() const
    {
        return lidClosure_;
    }

    // まばたきを終えた回数。実機のログで「まばたきが動いているか」を数える。
    [[nodiscard]] std::uint32_t blinkCount() const
    {
        return blinkCount_;
    }

    // render() を呼んだ回数。実機のログでコマ数を数える。
    [[nodiscard]] std::uint32_t frameCount() const
    {
        return frameCount_;
    }

    // 次のまばたきまでの残り (ミリ秒)。Open の間だけ意味を持つ。
    [[nodiscard]] std::uint32_t msUntilBlink() const
    {
        return nextBlinkMs_;
    }

    // --- まばたきの時間 ---
    //
    // 人間のまばたきは 100-150ms、間隔は 2-6 秒。スタックチャンの作例も
    // この範囲に収まる。ここから外すと機械仕掛けに見える。

    // まぶたが降りきるまで / 上がりきるまで。
    //
    // Why 降りる方を速くするか: 実際のまばたきも閉じる方が速い。
    // 同じ時間にすると、ゆっくり目を閉じる「眠そうな」動きになる。
    static constexpr std::uint32_t kBlinkClosingMs = 60;
    static constexpr std::uint32_t kBlinkOpeningMs = 90;

    // まばたきの間隔。この範囲から擬似乱数で選ぶ。
    //
    // Why not 固定間隔にしないか: 等間隔で瞬くとメトロノームに見える。
    // 間隔をばらけさせるだけで生き物らしさが出る。
    static constexpr std::uint32_t kBlinkIntervalMinMs = 2200;
    static constexpr std::uint32_t kBlinkIntervalMaxMs = 5600;

private:
    // 次のまばたきまでの時間を決める。
    void scheduleNextBlink();

    // 擬似乱数。線形合同法。
    //
    // Why not std::rand を使わないか: std::rand はグローバルな状態を持ち、
    // ほかの誰かが srand を呼ぶとまばたきの並びが変わる。テストで
    // 「同じ種なら同じ並び」を前提にできなくなる。合同法なら状態が
    // このインスタンスに閉じる。
    std::uint32_t nextRandom();

    std::uint16_t* sprite_ = nullptr;
    FaceExpression expression_ = FaceExpression::Neutral;

    BlinkPhase blinkPhase_ = BlinkPhase::Open;
    // 今の段階に入ってからの経過 (ミリ秒)。Closing / Opening で使う。
    std::uint32_t phaseElapsedMs_ = 0;
    // 次のまばたきまでの残り (ミリ秒)。Open の間だけ減る。
    std::uint32_t nextBlinkMs_ = kBlinkIntervalMinMs;
    std::uint8_t lidClosure_ = 0;

    std::uint32_t blinkCount_ = 0;
    std::uint32_t frameCount_ = 0;

    // 擬似乱数の状態。種は固定。
    //
    // Why 固定でよいか: まばたきの並びが毎回同じでも、見る人には分からない
    // (電源を入れ直したときの並びを覚えている人はいない)。固定なら
    // テストが実際の並びを検査できる。
    std::uint32_t rng_ = 0x1234ABCDU;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_AVATAR_H
