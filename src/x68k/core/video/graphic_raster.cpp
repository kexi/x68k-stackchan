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

// 表示が許可されたページを手前から順に並べたもの。
//
// 実機の 16 色モードは 4 ページが独立した画面で、$E82600 の GS3-GS0 で
// 個別に表示を切り替える。手前のページの透明ドット (パレット番号 0) の
// 位置だけ、その後ろのページが見える。だから「最も手前の 1 ページ」ではなく
// 順番付きの一覧が要る。
//
// Why not プレーンを合成して 1 つの色番号にしないか (テキスト画面のやり方):
// テキスト画面は 4 プレーンの同じビットを集めて 1 ドット 4bit を作るが、
// グラフィック画面のページは合成対象ではなく独立した画面で、
// 各ページがそれ自身で 4bit の色番号を持つ。集めるとまったく違う絵になる。
struct PageOrder
{
    // 手前から順のページ番号。count 個だけ有効。
    u8 pages[4];
    u32 count;
};

// 表示が許可されたページを手前から順に集める。
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
// Why not std::sort を使わないか: 要素は最大 4 個で、ESP32-S3 では
// <algorithm> を引き込むコード量のほうが効く。挿入ソートなら比較は
// 最大 6 回で済み、安定 (同順位なら番号順) という要件もそのまま満たせる。
inline PageOrder enabledPagesFrontToBack(const VideoController& video)
{
    PageOrder order{{0, 0, 0, 0}, 0};
    u8 priorities[4] = {0, 0, 0, 0};

    for (u32 page = 0; page < 4; ++page)
    {
        if (!video.graphicPageEnabled(page))
        {
            continue;
        }

        const u8 pagePriority = video.graphicPagePriority(page);

        // 手前 (順位が小さい) ほど前に来るよう挿入する。
        u32 slot = order.count;
        while (slot > 0 && priorities[slot - 1] > pagePriority)
        {
            order.pages[slot] = order.pages[slot - 1];
            priorities[slot] = priorities[slot - 1];
            --slot;
        }
        order.pages[slot] = static_cast<u8>(page);
        priorities[slot] = pagePriority;
        ++order.count;
    }

    return order;
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

    // 表示が許可されたページを手前から順に重ねる。
    //
    // Why not ページ 0 を決め打ちにしないか: Human68k も SX-Window も
    // ページを切り替えて裏画面を作る。$E82600 を無視すると、描き途中の
    // 裏画面が見えてちらつく。
    //
    // Why not 16 色モードだけ調べないか: 256 色モードも 2 ページあり、
    // GS3-GS0 は 2bit ずつで同じように表示を切り替えられる。1024x1024 は
    // 4 ページを 1 枚として使うのでページ選択そのものが無い。
    //
    // ページ番号ではなくビットシフト量に畳んでおく。16 色は 4bit ずつ、
    // 256 色は 8bit ずつで、ループの中では「ワードを何ビット右へずらすか」
    // しか要らない。色数モードの分岐をここで済ませておけば、ドットごとに
    // mode を見比べずに済む。
    u32 shifts[4] = {0, 0, 0, 0};
    u32 pageCount = 1;
    u32 indexMask = 0x0Fu;

    if (!isLarge)
    {
        const PageOrder order = enabledPagesFrontToBack(video);
        if (order.count == 0)
        {
            // どのページも表示が許可されていない。全面が透明。
            return;
        }

        const bool is16Color = mode == VideoController::GraphicColorMode::k16Color;
        const u32 bitsPerPage = is16Color ? 4u : 8u;
        indexMask = is16Color ? 0x0Fu : 0xFFu;
        pageCount = order.count;
        for (u32 i = 0; i < order.count; ++i)
        {
            const u32 page = is16Color ? (order.pages[i] & 3u) : (order.pages[i] & 1u);
            shifts[i] = page * bitsPerPage;
        }
    }

    // 表示ページが 1 枚だけなら重ね合わせの走査そのものが要らない。
    //
    // Why not 常に一般経路へ通さないか: 実際に動くソフトのほとんどは
    // 1 ページだけを表示していて (Human68k のコンソールも 256 色ソフトも
    // そうなる)、ESP32-S3 の実効 3MHz ではドットあたりの追加分岐が
    // そのままフレームレートに効く。1 枚のときの経路は元のまま残す。
    const bool isSinglePage = isLarge || pageCount == 1;
    const u32 singleShift = shifts[0];

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
                //
                // Why not ここでもページを重ねないか: 65536 色モードの表示
                // ページは 1 枚しかない (IPL-ROM は $FFB30C で GS3-GS0 を
                // moveq #$F と一括で立てる)。重ねる相手が存在しない。
                const u16 color = readWord(vram, wordIndexOf(vx, vy));
                if (color != 0)
                {
                    row[x] = VideoController::toRgb565(color);
                }
                continue;
            }

            // pixelIndex() を呼ばずここで展開する。1 ドットごとの関数呼び出しは
            // 変換全体の支配的なコストになる (text_raster.cpp と同じ理由)。
            if (isLarge)
            {
                const u32 largePage =
                    ((vy >= kGvramPageHeight) ? 2u : 0u) | ((vx >= kGvramPageWidth) ? 1u : 0u);
                const u16 word = readWord(
                    vram, wordIndexOf(vx & (kGvramPageWidth - 1u), vy & (kGvramPageHeight - 1u)));
                const u32 index = (word >> (largePage * 4u)) & 0x0Fu;
                if (index != kTransparentIndex)
                {
                    row[x] = palette[index];
                }
                continue;
            }

            // 全ページのドットが同じワードに詰まっているので、VRAM の読みは
            // ページ数によらず 1 回で済む。あとはシフトを変えて取り出すだけ。
            const u16 word = readWord(vram, wordIndexOf(vx, vy));

            if (isSinglePage)
            {
                const u32 index = (word >> singleShift) & indexMask;
                if (index != kTransparentIndex)
                {
                    row[x] = palette[index];
                }
                continue;
            }

            // 手前から見て最初の不透明ドットが勝つ。
            //
            // Why not 奥から手前へ順に書かないか (composite() の面の重ね方):
            // 面どうしと違い、ここは同じ 1 ワードから取り出す複数の値なので、
            // 手前から見て最初に見つかった時点で残りは覆い隠されて確定する。
            // 奥から書くと透明でないドットのぶんだけ palette 引きと
            // ストアが増え、ESP32-S3 では出力バッファへの無駄な書き込みになる。
            for (u32 i = 0; i < pageCount; ++i)
            {
                const u32 index = (word >> shifts[i]) & indexMask;
                if (index != kTransparentIndex)
                {
                    row[x] = palette[index];
                    break;
                }
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
