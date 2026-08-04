// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "sprite_raster.h"

namespace x68k
{
namespace
{

// スプライト / BG のセルは 16x16。
constexpr u32 kCell = Sprite::kBgCellSize;

// PCG のニブルを取り出す。1 バイトに 2 ドットで、上位ニブルが左のドット。
//
// Why not 下位ニブルを左にしないか: X68000 の 4bpp のパターンは、
// テキスト画面のビットマップと同じく「左のドットほど上位」に並ぶ。
// 逆に取ると 8x8 のパターンが 2 ドットずつ入れ替わった絵になる。
inline u8 nibbleAt(u8 byte, u32 x)
{
    return static_cast<u8>((x & 1u) == 0u ? (byte >> 4) : (byte & 0x0Fu));
}

// 8x8 パターンの 1 ドット。1 行 4 バイト、1 パターン 32 バイト。
inline u8 pixel8(const u8* vram, u32 pattern, u32 x, u32 y)
{
    const u32 base = pattern * Sprite::kPcg8Bytes;
    const u32 offset = base + y * 4u + (x >> 1);
    if (offset >= Sprite::kVramSize)
    {
        return 0;
    }
    return nibbleAt(vram[offset], x);
}

// スプライト 1 個ぶんの、あるラインに掛かる情報。
//
// ライン単位で作り直す。ドットごとのループでは、ここに詰めた値だけを
// 見れば済むようにしておく (レジスタの読み解きをドットごとに繰り返さない)。
struct LineSprite
{
    int screenX;   // 画面上の左端
    u32 pattern;   // 16x16 パターン番号
    u32 patternY;  // パターン内の行 (反転を適用済み)
    bool flipH;    // 水平反転
    u8 priority;   // 1-3。大きいほど手前
};

}  // namespace

// --- PCG のドット取り出し ----------------------------------------------------

u8 SpriteRaster::pcgPixel8(const u8* vram, u32 pattern, u32 x, u32 y)
{
    if (vram == nullptr || x >= 8 || y >= 8)
    {
        return 0;
    }
    return pixel8(vram, pattern, x, y);
}

u8 SpriteRaster::pcgPixel(const u8* vram, u32 pattern, u32 x, u32 y)
{
    if (vram == nullptr || x >= kCell || y >= kCell)
    {
        return 0;
    }

    // 16x16 は 8x8 を 4 つ並べたもの。左上 → 右上 → 左下 → 右下。
    // 16x16 パターン番号 n の 4 つ組は、8x8 の番号でいうと n*4 から始まる
    // (16x16 が 128 バイト = 8x8 の 4 個ぶん)。
    const u32 quadrant = ((y >= 8) ? 2u : 0u) | ((x >= 8) ? 1u : 0u);
    const u32 pattern8 = pattern * 4u + quadrant;
    return pixel8(vram, pattern8, x & 7u, y & 7u);
}

// --- 早期判定 ----------------------------------------------------------------

bool SpriteRaster::hasVisibleContent(const Sprite& sprite, const VideoController& video)
{
    // $E82600 の bit6 と $EB0808 の bit9 は AND で効く。IOCS は必ず対で
    // 操作する (SP_ON / SP_OFF) ので、片方だけでは出ない。
    if (!video.spriteEnabled() || !sprite.displayEnabled())
    {
        return false;
    }
    return sprite.anySpriteVisible() || sprite.anyBgEnabled();
}

// --- BG ----------------------------------------------------------------------

void SpriteRaster::renderBg(const Sprite& sprite, const VideoController& video, u32 plane, u32 srcX,
                            u32 srcY, u32 width, u32 height, u16* out, u32 outStride)
{
    if (out == nullptr || plane >= 2)
    {
        return;
    }
    if (!video.spriteEnabled() || !sprite.displayEnabled() || !sprite.bgEnabled(plane))
    {
        return;
    }

    const u8* vram = sprite.vram();

    // パレットを先に RGB565 へ変換しておく (graphic_raster.cpp と同じ理由)。
    // 16 エントリしかないので、ドットごとに toRgb565() を呼ぶ理由がない。
    u16 palette[VideoController::kTextPaletteCount];
    for (u32 i = 0; i < VideoController::kTextPaletteCount; ++i)
    {
        palette[i] = VideoController::toRgb565(video.textPalette(i));
    }

    // ネームテーブルの先頭。$EBC000 / $EBE000 のどちらを使うかは
    // BG 制御の 3bit フィールドの bit1 が決める。
    const u32 nameBase =
        sprite.bgTextArea(plane) == 0 ? Sprite::kBg0NameOffset : Sprite::kBg1NameOffset;

    // スクロール量。BG 面は 64x64 セル = 1024x1024 ドットで、端で巻き戻る。
    //
    // Why not 巻き戻さずに範囲外を透明にしないか: BG は横スクロールの
    // 背景として使われるもので、実機はネームテーブルを繰り返して隙間なく
    // 描く。巻き戻さないと、スクロールし続けたときに背景が途切れる。
    const u32 scrollX = sprite.bgScrollX(plane);
    const u32 scrollY = sprite.bgScrollY(plane);

    constexpr u32 kBgPixelsX = Sprite::kBgCellsX * kCell;  // 1024
    constexpr u32 kBgPixelsY = Sprite::kBgCellsY * kCell;

    for (u32 y = 0; y < height; ++y)
    {
        u16* row = out + static_cast<std::size_t>(y) * outStride;

        // BG 面での行。スクロールを足して巻き戻す。
        const u32 bgY = (srcY + y + scrollY) & (kBgPixelsY - 1u);
        const u32 cellY = bgY / kCell;
        const u32 inCellY = bgY & (kCell - 1u);

        for (u32 x = 0; x < width; ++x)
        {
            const u32 bgX = (srcX + x + scrollX) & (kBgPixelsX - 1u);
            const u32 cellX = bgX / kCell;

            // ネームテーブルの 1 セルは 1 ワード。X が 2 バイト刻み、
            // Y が 128 バイト刻み (BGTEXTST $FFC2A2 の計算と同じ)。
            const u32 nameOffset = nameBase + cellY * (Sprite::kBgCellsX * 2u) + cellX * 2u;
            if (nameOffset + 1 >= Sprite::kVramSize)
            {
                continue;
            }

            // ネームテーブルのワードはスプライトの属性ワードと同じ形。
            const u16 name =
                static_cast<u16>((static_cast<u16>(vram[nameOffset]) << 8) | vram[nameOffset + 1]);
            const u32 pattern = name & 0x00FFu;
            const bool flipH = (name & 0x0100u) != 0;
            const bool flipV = (name & 0x0200u) != 0;

            const u32 inCellX = bgX & (kCell - 1u);
            const u32 px = flipH ? (kCell - 1u - inCellX) : inCellX;
            const u32 py = flipV ? (kCell - 1u - inCellY) : inCellY;

            const u8 index = pcgPixel(vram, pattern, px, py);
            if (index != kTransparentIndex)
            {
                row[x] = palette[index];
            }
        }
    }
}

// --- スプライト --------------------------------------------------------------

void SpriteRaster::renderSprites(const Sprite& sprite, const VideoController& video, u32 srcX,
                                 u32 srcY, u32 width, u32 height, u16* out, u32 outStride)
{
    if (out == nullptr)
    {
        return;
    }

    // 出ていないなら、ここで丸ごと諦める。
    //
    // Human68k のコンソールはスプライトを一切使わないので、実際の運用では
    // ほぼ常にこの経路で返る。anySpriteVisible() はレジスタ書き込み時に
    // 更新した数を見るだけなので、128 個の走査は起きない。
    const bool visible =
        video.spriteEnabled() && sprite.displayEnabled() && sprite.anySpriteVisible();
    if (!visible)
    {
        return;
    }

    const u8* vram = sprite.vram();

    u16 palette[VideoController::kTextPaletteCount];
    for (u32 i = 0; i < VideoController::kTextPaletteCount; ++i)
    {
        palette[i] = VideoController::toRgb565(video.textPalette(i));
    }

    for (u32 y = 0; y < height; ++y)
    {
        const int lineY = static_cast<int>(srcY + y);

        // このラインに掛かるスプライトだけを集める。
        //
        // Why not ドットごとに 128 個を調べないか: 320x240 の 1 フレームで
        // 128 個 x 76800 ドット = 980 万回の矩形判定になり、ESP32-S3 では
        // 1 フレームに何秒もかかる。ラインごとに集めれば矩形判定は
        // 128 個 x 240 ライン = 3 万回で済み、ドットごとのループは
        // 実際に掛かっている数 (多くの場面で 0-数個) しか回らない。
        //
        // Why not フレームの先頭で全ラインぶんの一覧を作らないか: 240 ライン x
        // 32 個ぶんの配列は 7680 要素になり、ESP32 の内部 SRAM を無視できない
        // 量だけ食う。ラインごとに作り直せばスタックの 32 要素で済み、
        // 走査の総回数も変わらない。
        LineSprite line[kMaxSpritesPerLine];
        u32 lineCount = 0;

        for (u32 i = 0; i < Sprite::kSpriteCount && lineCount < kMaxSpritesPerLine; ++i)
        {
            const u8 priority = sprite.spritePriority(i);
            if (priority == 0)
            {
                continue;  // 非表示
            }

            const int spriteY = sprite.spriteY(i);
            const int dy = lineY - spriteY;
            const bool onThisLine = dy >= 0 && dy < static_cast<int>(kCell);
            if (!onThisLine)
            {
                continue;
            }

            const int spriteX = sprite.spriteX(i);
            // 横方向で切り出し範囲を完全に外れているものも落としておく。
            // ドットごとのループへ持ち込まないほうが速い。
            const int right = spriteX + static_cast<int>(kCell);
            const bool onScreen =
                right > static_cast<int>(srcX) && spriteX < static_cast<int>(srcX + width);
            if (!onScreen)
            {
                continue;
            }

            const bool flipV = sprite.spriteFlipV(i);
            const u32 patternY = flipV ? (kCell - 1u - static_cast<u32>(dy)) : static_cast<u32>(dy);

            line[lineCount].screenX = spriteX;
            line[lineCount].pattern = sprite.spritePattern(i);
            line[lineCount].patternY = patternY;
            line[lineCount].flipH = sprite.spriteFlipH(i);
            line[lineCount].priority = priority;
            ++lineCount;
        }

        if (lineCount == 0)
        {
            continue;
        }

        u16* row = out + static_cast<std::size_t>(y) * outStride;

        // 奥から手前へ描く。手前のスプライトの不透明ドットが上書きする。
        //
        // 実機のスプライトどうしの前後関係は、プライオリティ (1-3、大きいほど
        // 手前) が第一で、同値ならレジスタ番号の小さいほうが手前とされる。
        // ここでは priority を 1 → 2 → 3 の順に 3 巡し、各巡の中では
        // 番号の大きいほうから描く。こうすると「同値なら番号が小さいほど手前」
        // がそのまま出る。
        //
        // Why not 一覧をソートしてから 1 巡にしないか: 要素は最大 32 個で、
        // ソートの比較回数 (挿入ソートで最悪 496 回) より 3 巡の走査
        // (96 回) のほうが少ない。ソートに要る一時領域も避けられる。
        for (u8 pass = 1; pass <= 3; ++pass)
        {
            for (u32 k = lineCount; k > 0; --k)
            {
                const LineSprite& s = line[k - 1];
                if (s.priority != pass)
                {
                    continue;
                }

                for (u32 px = 0; px < kCell; ++px)
                {
                    const int screenX = s.screenX + static_cast<int>(px);
                    const int outX = screenX - static_cast<int>(srcX);
                    const bool inRange = outX >= 0 && outX < static_cast<int>(width);
                    if (!inRange)
                    {
                        continue;
                    }

                    const u32 patternX = s.flipH ? (kCell - 1u - px) : px;
                    const u8 index = pcgPixel(vram, s.pattern, patternX, s.patternY);
                    if (index != kTransparentIndex)
                    {
                        row[static_cast<u32>(outX)] = palette[index];
                    }
                }
            }
        }
    }
}

// --- 面全体 ------------------------------------------------------------------

void SpriteRaster::renderPlane(const Sprite& sprite, const VideoController& video, u32 srcX,
                               u32 srcY, u32 width, u32 height, u16* out, u32 outStride)
{
    if (out == nullptr)
    {
        return;
    }
    if (!video.spriteEnabled() || !sprite.displayEnabled())
    {
        return;
    }

    // BG1 → BG0 → スプライトの順で、奥から重ねる。
    //
    // Why not BG0 を BG1 より奥にしないか: 実機では BG0 が手前とされる。
    // BGCTRLST の引数 (plane 番号) は 0 が BG0 で、IOCS の使い方でも
    // BG0 が主たる背景、BG1 が遠景として扱われる。逆にすると
    // 遠景が主背景を覆う。
    //
    // Why not スプライトを BG より奥にも置けるようにしないか: スプライトの
    // プライオリティ 2bit が BG との前後も兼ねるという説明を見かけるが、
    // IPL-ROM はスプライトも BG も一切表示しないので、この対応を
    // ROM から確かめる手立てがない。確かめられない規則を実装して
    // 「テストは通るが実機と違う」を作るより、最も一般的な
    // 「スプライトは BG より手前」に固定して、その旨を書き残す。
    renderBg(sprite, video, 1, srcX, srcY, width, height, out, outStride);
    renderBg(sprite, video, 0, srcX, srcY, width, height, out, outStride);
    renderSprites(sprite, video, srcX, srcY, width, height, out, outStride);
}

}  // namespace x68k
