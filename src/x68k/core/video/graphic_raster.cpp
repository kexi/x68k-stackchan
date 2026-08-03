// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "graphic_raster.h"

namespace x68k
{
namespace
{

// G-VRAM の 1 ワードを取り出す。実 VRAM は 512 ライン x 512 ワード。
//
// 68000 はビッグエンディアンなので、ワードの上位バイトが先に来る。
// ホストのエンディアンに依存しないよう、バイト 2 つを明示的に組む。
// Why not u16 のポインタへキャストしないか: ESP32-S3 はリトルエンディアンで、
// キャストするとバイトが入れ替わる。加えて G-VRAM は 4 バイト境界に
// 揃っている保証がなく、非境界アクセスは Xtensa では例外になる。
inline u16 readWord(const u8* vram, u32 wordIndex)
{
    const u32 byteOffset = wordIndex * 2u;
    return static_cast<u16>((static_cast<u16>(vram[byteOffset]) << 8) | vram[byteOffset + 1]);
}

// 512x512 モードの座標からワード番号を求める。
inline u32 wordIndexOf(u32 x, u32 y)
{
    return (y * kGvramPageWidth) + x;
}

// 実画面の大きさ。範囲外の座標を弾くのに使う。
struct ScreenSize
{
    u32 width;
    u32 height;
};

inline ScreenSize screenSizeOf(const VideoController& video)
{
    const bool isLarge = video.graphicColorMode() == VideoController::GraphicColorMode::k16Color &&
                         video.isGraphic1024();
    if (isLarge)
    {
        return {kGvramLargeWidth, kGvramLargeHeight};
    }
    return {kGvramPageWidth, kGvramPageHeight};
}

// 表示が許可されたページのうち、最も手前のものを返す。見つからなければ -1。
//
// 手前かどうかは $E82500 の GP3-GP0 が決める (値が小さいほど手前)。
// 同じ順位が複数のページに設定されていたら、番号の小さいページを手前とする。
//
// Why not ページ番号の小さい順で決め打ちにしないか: GP3-GP0 は
// ページごとの順位を保持していて、プログラムはこれを書き換えるだけで
// VRAM を触らずに表示ページを入れ替えられる。SX-Window はこの方法で
// ウィンドウの重なりと裏画面の切り替えを行うので、番号順に固定すると
// 常に同じページが見えたままになる。
//
// Why not 4 ページを重ね合わせて 1 枚にしないか: 実機の 16 色モードでは
// 4 ページが独立した画面で、$E82600 の GS3-GS0 で個別に表示を切り替える。
// 透明ドットの位置だけ後ろのページが見える。プレーンを合成して 1 つの
// 色番号にする (テキスト画面のやり方) とまったく違う絵になる。
inline int frontmostEnabledPage(const VideoController& video)
{
    int front = -1;
    u8 frontPriority = 0;

    for (u32 page = 0; page < 4; ++page)
    {
        if (!video.graphicPageEnabled(page))
        {
            continue;
        }

        const u8 pagePriority = video.graphicPagePriority(page);
        const bool isFrontmost = front < 0 || pagePriority < frontPriority;
        if (isFrontmost)
        {
            front = static_cast<int>(page);
            frontPriority = pagePriority;
        }
    }

    return front;
}

}  // namespace

// --- ドット単位のアクセス ---------------------------------------------------

u8 GraphicRaster::pixelIndex(const u8* vram, VideoController::GraphicColorMode mode, u32 page,
                             u32 x, u32 y)
{
    if (vram == nullptr || x >= kGvramPageWidth || y >= kGvramPageHeight)
    {
        return 0;
    }

    const u16 word = readWord(vram, wordIndexOf(x, y));

    switch (mode)
    {
        case VideoController::GraphicColorMode::k16Color:
            // 1 ワードに 4 ページぶんの 4bit。ページ 0 が最下位ニブル。
            return static_cast<u8>((word >> ((page & 3u) * 4u)) & 0x0Fu);

        case VideoController::GraphicColorMode::k256Color:
            // 1 ワードに 2 ページぶんの 8bit。ページ 0 が下位バイト。
            return static_cast<u8>((word >> ((page & 1u) * 8u)) & 0xFFu);

        case VideoController::GraphicColorMode::kReserved:
        case VideoController::GraphicColorMode::k65536Color:
        default:
            // 色そのものが並ぶので番号としての意味はない。
            return static_cast<u8>(word & 0xFFu);
    }
}

u8 GraphicRaster::pixelIndexLarge(const u8* vram, u32 x, u32 y)
{
    if (vram == nullptr || x >= kGvramLargeWidth || y >= kGvramLargeHeight)
    {
        return 0;
    }

    // 4 ページを 2x2 に並べる。左上がページ 0、右上が 1、左下が 2、右下が 3。
    //
    // Why not 横 2 ページを繋げて 1024x512 とし、それを 2 段重ねるか:
    // 結果は同じ並びになるが、ページ番号を「上下 x 左右」で組み立てるほうが
    // 実機の資料の書き方に近く、ページ単位の表示制御と対応が取りやすい。
    const u32 page = ((y >= kGvramPageHeight) ? 2u : 0u) | ((x >= kGvramPageWidth) ? 1u : 0u);
    const u32 localX = x & (kGvramPageWidth - 1u);
    const u32 localY = y & (kGvramPageHeight - 1u);

    const u16 word = readWord(vram, wordIndexOf(localX, localY));
    return static_cast<u8>((word >> (page * 4u)) & 0x0Fu);
}

u16 GraphicRaster::pixelColor(const u8* vram, const VideoController& video, u32 page, u32 x, u32 y)
{
    if (vram == nullptr)
    {
        return 0;
    }

    const VideoController::GraphicColorMode mode = video.graphicColorMode();

    const bool isDirectColor = mode == VideoController::GraphicColorMode::k65536Color ||
                               mode == VideoController::GraphicColorMode::kReserved;
    if (isDirectColor)
    {
        if (x >= kGvramPageWidth || y >= kGvramPageHeight)
        {
            return 0;
        }
        // パレットを介さず、ワードがそのまま GGGGGRRRRRBBBBBI の色になる。
        return readWord(vram, wordIndexOf(x, y));
    }

    const bool isLarge =
        mode == VideoController::GraphicColorMode::k16Color && video.isGraphic1024();
    const u8 index = isLarge ? pixelIndexLarge(vram, x, y) : pixelIndex(vram, mode, page, x, y);

    return video.graphicPalette(index);
}

// --- 矩形の変換 -------------------------------------------------------------

void GraphicRaster::render(const u8* vram, const VideoController& video, u32 srcX, u32 srcY,
                           u32 width, u32 height, u16* out, u32 outStride)
{
    if (vram == nullptr || out == nullptr)
    {
        return;
    }

    const VideoController::GraphicColorMode mode = video.graphicColorMode();
    const ScreenSize screen = screenSizeOf(video);

    // パレットを先に RGB565 へ変換しておく。
    //
    // ドットごとに toRgb565() を呼ぶと、512x512 で 26 万回の変換になる。
    // 256 エントリを一度だけ作れば済む。スタックに置くのは
    // ESP32 のラスタ経路でヒープを触らないため (512 バイト)。
    u16 palette[VideoController::kGraphicPaletteCount];
    const bool isDirectColor = mode == VideoController::GraphicColorMode::k65536Color ||
                               mode == VideoController::GraphicColorMode::kReserved;
    if (!isDirectColor)
    {
        for (u32 i = 0; i < VideoController::kGraphicPaletteCount; ++i)
        {
            palette[i] = VideoController::toRgb565(video.graphicPalette(i));
        }
    }

    const bool isLarge =
        mode == VideoController::GraphicColorMode::k16Color && video.isGraphic1024();

    // 表示が許可された最も手前のページを描く。
    //
    // Why not ページ 0 を決め打ちにしないか: Human68k も SX-Window も
    // ページを切り替えて裏画面を作る。$E82600 を無視すると、描き途中の
    // 裏画面が見えてちらつく。
    //
    // Why not 16 色モードだけ調べないか: 256 色モードも 2 ページあり、
    // GS3-GS0 は 2bit ずつで同じように表示を切り替えられる。1024x1024 は
    // 4 ページを 1 枚として使うのでページ選択そのものが無い。
    int page = 0;
    if (!isLarge)
    {
        page = frontmostEnabledPage(video);
        if (page < 0)
        {
            // どのページも表示が許可されていない。全面が透明。
            return;
        }
    }
    const u32 pageIndex = static_cast<u32>(page);

    for (u32 y = 0; y < height; ++y)
    {
        const u32 vy = srcY + y;
        if (vy >= screen.height)
        {
            break;
        }

        u16* row = out + static_cast<std::size_t>(y) * outStride;

        for (u32 x = 0; x < width; ++x)
        {
            const u32 vx = srcX + x;
            if (vx >= screen.width)
            {
                break;
            }

            if (isDirectColor)
            {
                // 65536 色モードは色コードが直接並ぶ。$0000 を透明とみなす。
                const u16 color = readWord(vram, wordIndexOf(vx, vy));
                if (color != 0)
                {
                    row[x] = VideoController::toRgb565(color);
                }
                continue;
            }

            // pixelIndex() を呼ばずここで展開する。1 ドットごとの関数呼び出しは
            // 変換全体の支配的なコストになる (text_raster.cpp と同じ理由)。
            u32 index;
            if (isLarge)
            {
                const u32 largePage =
                    ((vy >= kGvramPageHeight) ? 2u : 0u) | ((vx >= kGvramPageWidth) ? 1u : 0u);
                const u16 word = readWord(
                    vram, wordIndexOf(vx & (kGvramPageWidth - 1u), vy & (kGvramPageHeight - 1u)));
                index = (word >> (largePage * 4u)) & 0x0Fu;
            }
            else if (mode == VideoController::GraphicColorMode::k16Color)
            {
                const u16 word = readWord(vram, wordIndexOf(vx, vy));
                index = (word >> (pageIndex * 4u)) & 0x0Fu;
            }
            else
            {
                const u16 word = readWord(vram, wordIndexOf(vx, vy));
                index = (word >> ((pageIndex & 1u) * 8u)) & 0xFFu;
            }

            if (index != kTransparentIndex)
            {
                row[x] = palette[index];
            }
        }
    }
}

// --- 重ね合わせ -------------------------------------------------------------

void GraphicRaster::renderTextOver(const u8* vram, const VideoController& video, u32 srcX, u32 srcY,
                                   u32 width, u32 height, u16* out, u32 outStride)
{
    if (vram == nullptr || out == nullptr)
    {
        return;
    }

    u16 palette[VideoController::kTextPaletteCount];
    for (u32 i = 0; i < VideoController::kTextPaletteCount; ++i)
    {
        palette[i] = VideoController::toRgb565(video.textPalette(i));
    }

    for (u32 y = 0; y < height; ++y)
    {
        const u32 vy = srcY + y;
        if (vy >= 1024)
        {
            break;  // テキスト画面は 1024 ライン
        }

        u16* row = out + static_cast<std::size_t>(y) * outStride;
        const u32 lineBase = vy * kTvramBytesPerLine;

        for (u32 x = 0; x < width; ++x)
        {
            const u32 vx = srcX + x;
            if (vx >= 1024)
            {
                break;
            }

            // 4 プレーンの同じビット位置を集めて 4bit にする。
            // text_raster.cpp と同じくインライン展開する。
            const u32 byteOffset = lineBase + (vx >> 3);
            const u8 mask = static_cast<u8>(1u << (7u - (vx & 7u)));

            u32 index = 0;
            if ((vram[byteOffset] & mask) != 0)
            {
                index |= 1u;
            }
            if ((vram[kTvramPlaneSize + byteOffset] & mask) != 0)
            {
                index |= 2u;
            }
            if ((vram[2 * kTvramPlaneSize + byteOffset] & mask) != 0)
            {
                index |= 4u;
            }
            if ((vram[3 * kTvramPlaneSize + byteOffset] & mask) != 0)
            {
                index |= 8u;
            }

            if (index != kTransparentIndex)
            {
                row[x] = palette[index];
            }
        }
    }
}

void GraphicRaster::composite(const u8* graphicVram, const u8* textVram,
                              const VideoController& video, u32 srcX, u32 srcY, u32 width,
                              u32 height, u16* out, u32 outStride)
{
    if (out == nullptr)
    {
        return;
    }

    // 背景を黒で埋めてから奥のプレーンを描く。
    //
    // Why not 埋めずに済ませないか: render() は透明ドットへ何も書かないので、
    // out の初期値が残ってしまう。呼ぶ側のバッファは前フレームの内容を
    // 持っていることが多く、消えたはずの絵が残る。
    for (u32 y = 0; y < height; ++y)
    {
        u16* row = out + static_cast<std::size_t>(y) * outStride;
        for (u32 x = 0; x < width; ++x)
        {
            row[x] = 0;
        }
    }

    const bool showGraphic = graphicVram != nullptr && video.graphicEnabled();
    const bool showText = textVram != nullptr && video.textEnabled();

    // プライオリティの値が大きいほど奥。奥から順に描けば、手前のプレーンの
    // 透明でないドットが上書きする形で重なる。
    //
    // Why not 手前から描いて「まだ書かれていない位置だけ埋める」方式にしないか:
    // 「書かれていない」ことを表す値が要る。黒 ($0000) を未書き込みとみなすと
    // 手前のプレーンの黒いドットが背後を透かしてしまい、黒で塗り潰した
    // ウィンドウの下に別の絵が浮く。奥から描けば余計な状態を持たずに済む。
    const bool graphicIsBehind = video.graphicPriority() >= video.textPriority();

    if (graphicIsBehind)
    {
        if (showGraphic)
        {
            render(graphicVram, video, srcX, srcY, width, height, out, outStride);
        }
        if (showText)
        {
            renderTextOver(textVram, video, srcX, srcY, width, height, out, outStride);
        }
        return;
    }

    if (showText)
    {
        renderTextOver(textVram, video, srcX, srcY, width, height, out, outStride);
    }
    if (showGraphic)
    {
        render(graphicVram, video, srcX, srcY, width, height, out, outStride);
    }
}

}  // namespace x68k
