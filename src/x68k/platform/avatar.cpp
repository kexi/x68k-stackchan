// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 顔を RGB565 の 1 枚へ描く。M5GFX には触らない (avatar.h の冒頭を見よ)。

#include "avatar.h"

namespace x68k_platform
{
namespace
{

// 色。RGB565。
//
// Why 白と黒だけか: スタックチャンの標準の顔は黒地に白。中間色を使うと、
// 目の縁で背景と混ざった画素ができ、テストが「白か黒か」で判定できなくなる。
// 見た目の面でも、2 インチの LCD では階調よりも輪郭のはっきりさが効く。
constexpr std::uint16_t kColorBackground = 0x0000;  // 黒
constexpr std::uint16_t kColorFace = 0xFFFF;        // 白

// 目の配置。320x240 の中で顔の重心がやや上に来るようにする。
//
// Why 目を中央より上に置くか: 口を下に置く余地が要る。中央に目を置くと
// 口が画面の下端に貼り付き、顔ではなく記号に見える。
constexpr std::int32_t kEyeCenterY = 96;
constexpr std::int32_t kEyeLeftX = 104;
constexpr std::int32_t kEyeRightX = 216;

// 目の大きさ (全開のとき)。横半径 x 縦半径。
//
// Why 縦を横より大きくするか: M5Stack-Avatar の目は縦長の角丸。真円だと
// 顔が平板に見え、まばたきで潰れる幅も稼げない。
constexpr std::int32_t kEyeRadiusX = 26;
constexpr std::int32_t kEyeRadiusY = 32;

// 口の配置と大きさ。
constexpr std::int32_t kMouthCenterX = 160;
constexpr std::int32_t kMouthCenterY = 176;

// まぶたが降りきったときに残す縦半径。
//
// Why 0 にしないか: 0 にすると目が完全に消え、瞬きの底で「顔から目が
// 無くなった」ように見える。細い線を残すと、まぶたが閉じたと読める。
constexpr std::int32_t kClosedEyeRadiusY = 2;

// 画素を 1 つ置く。範囲外は捨てる。
void putPixel(std::uint16_t* buffer, std::int32_t x, std::int32_t y, std::uint16_t color)
{
    const bool isOutside = x < 0 || y < 0 || x >= static_cast<std::int32_t>(Avatar::kWidth) ||
                           y >= static_cast<std::int32_t>(Avatar::kHeight);
    if (isOutside)
    {
        return;
    }
    buffer[static_cast<std::size_t>(y) * Avatar::kWidth + static_cast<std::size_t>(x)] = color;
}

// 横一列を塗る。範囲外は切る。
void fillSpan(std::uint16_t* buffer, std::int32_t x0, std::int32_t x1, std::int32_t y,
              std::uint16_t color)
{
    const bool isOffScreen = y < 0 || y >= static_cast<std::int32_t>(Avatar::kHeight);
    if (isOffScreen)
    {
        return;
    }
    const std::int32_t left = x0 < 0 ? 0 : x0;
    const std::int32_t right = x1 >= static_cast<std::int32_t>(Avatar::kWidth)
                                   ? static_cast<std::int32_t>(Avatar::kWidth) - 1
                                   : x1;
    for (std::int32_t x = left; x <= right; ++x)
    {
        putPixel(buffer, x, y, color);
    }
}

// 塗り潰した楕円。中心 (cx, cy)、半径 (rx, ry)。
//
// Why not 円 (fillCircle 相当) で済ませないか: まばたきは縦半径だけを
// 縮める操作で表す。真円しか描けないと、閉じる途中の目が小さい丸に
// なってしまい、まぶたが降りているようには見えない。
void fillEllipse(std::uint16_t* buffer, std::int32_t cx, std::int32_t cy, std::int32_t rx,
                 std::int32_t ry, std::uint16_t color)
{
    const bool isDegenerate = rx <= 0 || ry <= 0;
    if (isDegenerate)
    {
        return;
    }

    // 各行について x の広がりを (x/rx)^2 + (y/ry)^2 <= 1 から解く。
    //
    // Why not 平方根を使わないか: 実機の FPU は単精度で、sqrt 自体は
    // 速い。ただ整数のまま比較すれば丸めの向きが実装に依らず決まるので、
    // ホストと実機で同じ画素になる。テストがホストの結果を根拠にできる。
    for (std::int32_t dy = -ry; dy <= ry; ++dy)
    {
        // dx の上限 = rx * sqrt(1 - (dy/ry)^2)。両辺を二乗して整数で回す。
        const std::int64_t limit =
            static_cast<std::int64_t>(rx) * rx *
            (static_cast<std::int64_t>(ry) * ry - static_cast<std::int64_t>(dy) * dy);
        std::int32_t dx = 0;
        while (static_cast<std::int64_t>(dx + 1) * (dx + 1) * ry * ry <= limit)
        {
            ++dx;
        }
        fillSpan(buffer, cx - dx, cx + dx, cy + dy, color);
    }
}

// 塗り潰した長方形。
void fillRect(std::uint16_t* buffer, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
              std::uint16_t color)
{
    for (std::int32_t dy = 0; dy < h; ++dy)
    {
        fillSpan(buffer, x, x + w - 1, y + dy, color);
    }
}

// 上に凸の弧を太さ thickness で描く。笑い目と笑い口に使う。
//
// 楕円の下半分を塗ってから、一回り小さい楕円で内側を背景色に戻す。
//
// Why not 曲線を 1 画素ずつ追わないか: 太さを持たせるときに端の処理が
// 要る (線分の法線方向へ広げる) 割に、結果は同じ形になる。塗り潰しを
// 2 回する方が短く、太さがそのまま半径の差で表せる。
void fillArcUp(std::uint16_t* buffer, std::int32_t cx, std::int32_t cy, std::int32_t rx,
               std::int32_t ry, std::int32_t thickness, std::uint16_t color)
{
    fillEllipse(buffer, cx, cy, rx, ry, color);
    fillEllipse(buffer, cx, cy, rx - thickness, ry - thickness, kColorBackground);
    // 上半分を消す。残るのは下側の弧で、画面では「∪」に見える。
    fillRect(buffer, cx - rx, cy - ry, rx * 2 + 1, ry, kColorBackground);
}

// 下に凸の弧。∩ の形。笑い目に使う。
void fillArcDown(std::uint16_t* buffer, std::int32_t cx, std::int32_t cy, std::int32_t rx,
                 std::int32_t ry, std::int32_t thickness, std::uint16_t color)
{
    fillEllipse(buffer, cx, cy, rx, ry, color);
    fillEllipse(buffer, cx, cy, rx - thickness, ry - thickness, kColorBackground);
    // 下半分を消す。残るのは上側の弧。
    fillRect(buffer, cx - rx, cy + 1, rx * 2 + 1, ry, kColorBackground);
}

// 表情ごとの目の縦半径 (まばたきを掛ける前)。
//
// Sleepy だけ縮める。上まぶたが半分降りた状態を、目そのものを平たく
// することで表す。
std::int32_t baseEyeRadiusY(FaceExpression expression)
{
    switch (expression)
    {
        case FaceExpression::Sleepy:
            return kEyeRadiusY / 3;
        case FaceExpression::Surprised:
            // 見開く。横も広げたいところだが、広げると左右の目が寄って
            // 見えるので縦だけにする。
            return kEyeRadiusY + 6;
        case FaceExpression::Neutral:
        case FaceExpression::Happy:
            break;
    }
    return kEyeRadiusY;
}

// まばたきを掛けた後の縦半径。
//
// closure は 0 (全開) から 255 (閉じきり)。
std::int32_t applyBlink(std::int32_t baseRy, std::uint8_t closure)
{
    const std::int32_t range = baseRy - kClosedEyeRadiusY;
    const bool isAlreadyThin = range <= 0;
    if (isAlreadyThin)
    {
        return baseRy;
    }
    const std::int32_t shrink = range * static_cast<std::int32_t>(closure) / 255;
    return baseRy - shrink;
}

// 片目を描く。
void drawEye(std::uint16_t* buffer, std::int32_t cx, FaceExpression expression,
             std::uint8_t closure)
{
    // 笑い目は弧。ただし瞬きの途中は弧のまま潰すと形が壊れるので、
    // 閉じ始めたら通常の目と同じ扱いにする。
    //
    // Why not 笑ったまま瞬かせないか: 笑い目 (∩) は既に「閉じた目」の
    // 形をしている。そこへまぶたを降ろしても違いが出ず、瞬いたことが
    // 分からない。閉じる間だけ塗り潰しの目に戻せば、潰れる動きが見える。
    const bool isSmilingEye = expression == FaceExpression::Happy && closure == 0;
    if (isSmilingEye)
    {
        fillArcDown(buffer, cx, kEyeCenterY + kEyeRadiusY / 2, kEyeRadiusX, kEyeRadiusY / 2 + 4, 8,
                    kColorFace);
        return;
    }

    const std::int32_t ry = applyBlink(baseEyeRadiusY(expression), closure);
    fillEllipse(buffer, cx, kEyeCenterY, kEyeRadiusX, ry, kColorFace);
}

// 口を描く。表情ごとに形が変わる。
void drawMouth(std::uint16_t* buffer, FaceExpression expression)
{
    switch (expression)
    {
        case FaceExpression::Neutral:
            // 横一文字。角を丸めるために両端へ小さい円を置く。
            fillRect(buffer, kMouthCenterX - 30, kMouthCenterY - 4, 61, 9, kColorFace);
            fillEllipse(buffer, kMouthCenterX - 30, kMouthCenterY, 4, 4, kColorFace);
            fillEllipse(buffer, kMouthCenterX + 30, kMouthCenterY, 4, 4, kColorFace);
            return;

        case FaceExpression::Happy:
            // 大きく開けて笑う。下向きの弧 (∪) を太く描く。
            fillArcUp(buffer, kMouthCenterX, kMouthCenterY - 12, 40, 26, 10, kColorFace);
            return;

        case FaceExpression::Sleepy:
            // 小さくすぼめる。眠いときに口を大きく開けていると、
            // 目が細いこととちぐはぐに見える。
            fillEllipse(buffer, kMouthCenterX, kMouthCenterY, 10, 5, kColorFace);
            return;

        case FaceExpression::Surprised:
            // 丸く開ける。縦長の楕円で「お」の口。
            fillEllipse(buffer, kMouthCenterX, kMouthCenterY, 16, 22, kColorFace);
            return;
    }
}

}  // namespace

void Avatar::setSpriteBuffer(std::uint16_t* buffer)
{
    sprite_ = buffer;
}

void Avatar::setExpression(FaceExpression expression)
{
    expression_ = expression;
}

std::uint32_t Avatar::nextRandom()
{
    // Numerical Recipes の係数。周期 2^32 で、下位ビットの偏りは
    // 上位を使うことで避ける。
    rng_ = rng_ * 1664525U + 1013904223U;
    return rng_ >> 16;
}

void Avatar::scheduleNextBlink()
{
    constexpr std::uint32_t span = kBlinkIntervalMaxMs - kBlinkIntervalMinMs;
    nextBlinkMs_ = kBlinkIntervalMinMs + nextRandom() % (span + 1);
}

bool Avatar::tick(std::uint32_t elapsedMs)
{
    const std::uint8_t previousClosure = lidClosure_;

    // 与えられた時間を、段階の境界をまたぐたびに食い潰していく。
    //
    // Why ループにするか: 呼ばれる間隔 (約 16ms) に対して閉じる段階は
    // 60ms しかない。フレームが飛んで 200ms 進むと、閉じる段階を通り
    // 越して開く段階まで到達する。1 回の呼び出しで 1 段階しか進めない
    // 実装だと、コマ落ちのたびにまばたきが間延びし、最悪まぶたが
    // 閉じたまま数コマ止まる。余りを持ち越して回せば、どれだけ飛んでも
    // 実時間どおりの位置に着く。
    //
    // Why not 無限に回らないか: Open 以外の段階は必ず有限の時間で
    // 終わり、Open へ戻ったところで残りが nextBlinkMs_ に満たなければ
    // 抜ける。elapsedMs は毎周必ず減るので停止する。
    std::uint32_t remainingMs = elapsedMs;
    while (remainingMs > 0)
    {
        if (blinkPhase_ == BlinkPhase::Open)
        {
            const bool isBlinkDue = remainingMs >= nextBlinkMs_;
            if (!isBlinkDue)
            {
                nextBlinkMs_ -= remainingMs;
                break;
            }
            remainingMs -= nextBlinkMs_;
            nextBlinkMs_ = 0;
            blinkPhase_ = BlinkPhase::Closing;
            phaseElapsedMs_ = 0;
            continue;
        }

        if (blinkPhase_ == BlinkPhase::Closing)
        {
            const std::uint32_t leftInPhase = kBlinkClosingMs - phaseElapsedMs_;
            const bool isClosed = remainingMs >= leftInPhase;
            if (!isClosed)
            {
                phaseElapsedMs_ += remainingMs;
                lidClosure_ = static_cast<std::uint8_t>(phaseElapsedMs_ * 255U / kBlinkClosingMs);
                break;
            }
            remainingMs -= leftInPhase;
            lidClosure_ = 255;
            blinkPhase_ = BlinkPhase::Opening;
            phaseElapsedMs_ = 0;

            // 閉じきった状態を 1 コマ見せてから開き始める。
            //
            // Why 要るか: 閉じる 60ms に対して呼ばれる間隔は約 16ms しか
            // なく、境界にちょうど乗ることはまずない。ここで continue して
            // 開く段階の計算まで一気に進めると、その周の lidClosure_ は
            // 244 のような中途半端な値になり、「完全に閉じた目」が 1 度も
            // 描かれない。実測 (ホストで 16ms 刻みに回した結果) では
            // 0 → 68 → 136 → 204 → 244 → 199 と、255 を跨がずに折り返して
            // いた。まばたきの底が無いと、目が細くなって戻るだけの
            // 半端な動きに見える。
            //
            // 残り時間は phaseElapsedMs_ に積まずに持ち越さず捨てる…
            // のではなく、次の呼び出しで開く段階として消化される。
            // ここで抜けても remainingMs は次周には引き継がないので、
            // まばたき 1 回あたり最大 1 コマ (16ms) 長くなる。
            //
            // Why それを許すか: 16ms はまばたき全体 150ms の 1 割で、
            // 目で見て分かる差ではない。一方「底が描かれない」は
            // はっきり分かる。ただしコマ落ちで大きく飛んだとき
            // (remainingMs が 1 コマぶんを大きく超えるとき) は、
            // 底を見せる価値より実時間へ追いつく方が優先なので続行する。
            constexpr std::uint32_t kOneFrameMs = 20;
            const bool canShowClosedFrame = remainingMs <= kOneFrameMs;
            if (canShowClosedFrame)
            {
                break;
            }
            continue;
        }

        // BlinkPhase::Opening
        const std::uint32_t leftInPhase = kBlinkOpeningMs - phaseElapsedMs_;
        const bool isOpen = remainingMs >= leftInPhase;
        if (!isOpen)
        {
            phaseElapsedMs_ += remainingMs;
            lidClosure_ =
                static_cast<std::uint8_t>(255U - phaseElapsedMs_ * 255U / kBlinkOpeningMs);
            break;
        }
        remainingMs -= leftInPhase;
        lidClosure_ = 0;
        blinkPhase_ = BlinkPhase::Open;
        phaseElapsedMs_ = 0;
        ++blinkCount_;
        scheduleNextBlink();
    }

    return lidClosure_ != previousClosure;
}

bool Avatar::render()
{
    if (sprite_ == nullptr)
    {
        return false;
    }

    // 全面を塗ってから顔を置く。
    //
    // Why not 変わったところだけ描かないか: このバッファは顔専用で、
    // 直前の内容は 1 コマ前の顔。目だけ描き直すには前の目の位置を
    // 覚えて消す必要があり、表情が変わったコマでは口も動く。
    // 320x240 の塗り潰しは PSRAM でも 1ms 台で終わる (実測は
    // 顔モードのログの frame 間隔を見よ) ので、単純な方を選ぶ。
    for (std::size_t i = 0; i < static_cast<std::size_t>(kWidth) * kHeight; ++i)
    {
        sprite_[i] = kColorBackground;
    }

    drawEye(sprite_, kEyeLeftX, expression_, lidClosure_);
    drawEye(sprite_, kEyeRightX, expression_, lidClosure_);

    // 口はまばたきの影響を受けない。
    //
    // Why not 口も動かさないか: 人間は瞬きで口の形を変えない。連動させると
    // 顔全体が痙攣しているように見える。
    drawMouth(sprite_, expression_);

    ++frameCount_;
    return true;
}

}  // namespace x68k_platform
