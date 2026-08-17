// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// ホスト (Mac/Linux) でエミュレータを走らせるフロントエンド。
//
// core/ は ESP32 非依存なので、実機に焼かずにここで IPL-ROM と Human68k の
// 起動をデバッグできる。実機はシリアルログしか見えず 1 サイクル 30 秒かかるのに
// 対し、ここなら 1 秒で回せてデバッガも使える。これが開発速度を決める。
//
// 使い方:
//   x68k-run --iplrom rom/iplrom.dat [--hdd rom/hdd0.hdf] [--fd0 disk.xdf]
//            [--ppm out.ppm] [--trace] [--cycles N]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "io/ascii_keymap.h"
#include "gui_demo.h"
#include "machine.h"
#include "video/cgrom_fallback.h"
#include "video/graphic_raster.h"
#include "video/text_raster.h"
#include "video/text_scrape.h"

namespace
{

// ファイル全体をメモリへ読む。
std::vector<x68k::u8> readFile(const std::string& path, bool* ok)
{
    *ok = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(f);
        return {};
    }
    std::vector<x68k::u8> data(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (read != data.size())
    {
        return {};
    }
    *ok = true;
    return data;
}

// ディスクイメージをメモリ上に持つ実装。
// ホストではファイル全体を読み込んでしまう方が単純で速い。
class FileDisk final : public x68k::DiskImage
{
public:
    bool load(const std::string& path)
    {
        bool ok = false;
        data_ = readFile(path, &ok);
        return ok;
    }

    // どのセクタが要求されたかを出す。起動しないディスクイメージを
    // 作ったとき、IPL-ROM がどこまで読めたのかが分からないと直せない。
    void setTrace(bool on)
    {
        trace_ = on;
    }

    bool readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount) override
    {
        constexpr x68k::u32 kSectorSize = 256;
        const std::size_t offset = static_cast<std::size_t>(lba) * kSectorSize;
        const std::size_t length = static_cast<std::size_t>(sectorCount) * kSectorSize;
        if (trace_)
        {
            std::printf("[disk] read lba=%u count=%u%s\n", lba, sectorCount,
                        offset + length > data_.size() ? " (範囲外)" : "");
        }
        if (offset + length > data_.size())
        {
            return false;
        }
        std::memcpy(buffer, data_.data() + offset, length);
        return true;
    }

    bool writeSector(x68k::u32 lba, const x68k::u8* buffer, x68k::u32 sectorCount) override
    {
        constexpr x68k::u32 kSectorSize = 256;
        const std::size_t offset = static_cast<std::size_t>(lba) * kSectorSize;
        const std::size_t length = static_cast<std::size_t>(sectorCount) * kSectorSize;
        if (offset + length > data_.size())
        {
            return false;
        }
        std::memcpy(data_.data() + offset, buffer, length);
        return true;
    }

    [[nodiscard]] bool isPresent() const override
    {
        return !data_.empty();
    }

private:
    std::vector<x68k::u8> data_;
    bool trace_ = false;
};

// フロッピーイメージをメモリ上に持つ実装。
//
// 受け付ける形式は XDF (ヘッダ無しの生セクタダンプ) と DIM (256 バイトの
// ヘッダ付き)。どちらもファイル長からジオメトリを引く
// (detectFloppyFormat)。拡張子は見ない。手元のイメージは拡張子が
// 実態と食い違っていることがあり、長さの方が確実に当たる。
class FileFloppy final : public x68k::FloppyImage
{
public:
    bool load(const std::string& path)
    {
        bool ok = false;
        std::vector<x68k::u8> raw = readFile(path, &ok);
        if (!ok)
        {
            return false;
        }

        const auto size = static_cast<x68k::u32>(raw.size());
        x68k::u32 offset = 0;
        format_ = x68k::detectFloppyFormat(size, &offset, &geometry_);
        if (format_ == x68k::FloppyFormat::Unknown)
        {
            // 長さが分からないイメージは受け付けない。
            //
            // 切り上げて「とりあえず 2HD」として読ませると、末尾の
            // トラックが読めないまま「ディスクはある」と見えてしまう。
            // Human68k は FAT を読んだ時点でおかしくなるが、そこまで
            // 症状が離れると原因に辿り着けない。
            return false;
        }

        // ヘッダを落として中身だけを持つ。以後 CHS の換算にオフセットが
        // 混ざらない。
        data_.assign(raw.begin() + offset, raw.end());
        return true;
    }

    void setWriteProtected(bool on)
    {
        writeProtected_ = on;
    }

    void setTrace(bool on)
    {
        trace_ = on;
    }

    [[nodiscard]] x68k::FloppyFormat format() const
    {
        return format_;
    }

    bool readSector(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record, x68k::u8* buffer) override
    {
        std::size_t offset = 0;
        const bool ok = locate(cylinder, head, record, &offset);
        if (trace_)
        {
            std::printf("[fd] read c=%u h=%u r=%u%s\n", cylinder, head, record,
                        ok ? "" : " (範囲外)");
        }
        if (!ok)
        {
            return false;
        }
        std::memcpy(buffer, data_.data() + offset, geometry_.sectorSize);
        return true;
    }

    bool writeSector(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record,
                     const x68k::u8* buffer) override
    {
        if (writeProtected_)
        {
            return false;
        }
        std::size_t offset = 0;
        const bool ok = locate(cylinder, head, record, &offset);
        if (trace_)
        {
            std::printf("[fd] write c=%u h=%u r=%u%s\n", cylinder, head, record,
                        ok ? "" : " (範囲外)");
        }
        if (!ok)
        {
            return false;
        }
        std::memcpy(data_.data() + offset, buffer, geometry_.sectorSize);
        return true;
    }

    [[nodiscard]] bool isPresent() const override
    {
        return !data_.empty();
    }

    [[nodiscard]] bool isWriteProtected() const override
    {
        return writeProtected_;
    }

    [[nodiscard]] const x68k::FloppyGeometry& geometry() const override
    {
        return geometry_;
    }

private:
    // CHS からイメージ内のオフセットを引く。R は 1 起点。
    bool locate(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record, std::size_t* offset) const
    {
        const bool inRange = cylinder < geometry_.cylinders && head < geometry_.heads &&
                             record >= 1 && record <= geometry_.sectorsPerTrack;
        if (!inRange)
        {
            return false;
        }
        const x68k::u32 lba =
            ((cylinder * geometry_.heads) + head) * geometry_.sectorsPerTrack + (record - 1);
        *offset = static_cast<std::size_t>(lba) * geometry_.sectorSize;
        return *offset + geometry_.sectorSize <= data_.size();
    }

    std::vector<x68k::u8> data_;
    x68k::FloppyGeometry geometry_{};
    x68k::FloppyFormat format_ = x68k::FloppyFormat::Unknown;
    bool writeProtected_ = false;
    bool trace_ = false;
};

// テキスト画面を PPM (P6) で書き出す。
//
// PPM を選んだ理由: ヘッダが数行のテキストで、圧縮も無く、依存ライブラリも要らない。
// 画像として見たいだけなので PNG にする必要がない。
bool writePpm(const std::string& path, const x68k::u16* pixels, x68k::u32 width, x68k::u32 height)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
    {
        return false;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (x68k::u32 i = 0; i < width * height; ++i)
    {
        const x68k::u16 c = pixels[i];
        // RGB565 → RGB888。上位ビットを下位へ複製して階調を伸ばす。
        const x68k::u8 r = static_cast<x68k::u8>(((c >> 11) & 0x1F) * 255 / 31);
        const x68k::u8 g = static_cast<x68k::u8>(((c >> 5) & 0x3F) * 255 / 63);
        const x68k::u8 b = static_cast<x68k::u8>((c & 0x1F) * 255 / 31);
        std::fputc(r, f);
        std::fputc(g, f);
        std::fputc(b, f);
    }

    // 書けたか確かめてから閉じる。
    //
    // fputc の戻り値を毎回見るのは煩雑なので、ferror でまとめて判定する。
    // fclose もバッファを吐き出すので失敗しうる。どちらも見ないと、
    // ディスクが一杯でも「書けました」と報告してしまう。
    const bool hadError = std::ferror(f) != 0;
    const bool closeFailed = std::fclose(f) != 0;
    return !hadError && !closeFailed;
}

// 台本で与えるマウスの 1 イベント。
//
// 実機のタッチと同じく相対移動で表す。X68000 のマウスは絶対座標を持たない
// ので、絶対座標で書ける記法にすると「今カーソルがどこにあるか」を
// ホスト側が知っている前提になり、実機で再現できない台本ができてしまう。
struct MouseEvent
{
    // 何サイクル目に送るか。キー入力と同じく、起動が落ち着いてから
    // 送らないと IOCS のマウス初期化前に捨てられる。
    x68k::u64 atCycle = 0;
    int dx = 0;
    int dy = 0;
    bool leftButton = false;
    bool rightButton = false;
};

// --mouse の引数を解く。
//
// 書式: CYCLE:DX:DY[:BUTTONS] をカンマで並べる。BUTTONS は L / R の並び。
//   例: --mouse 400000000:10:0:L,401000000:0:0:
//       (4 億サイクル目に左ボタンを押しながら右へ 10、その後ボタンを離す)
//
// Why not キーの --keys のように「文字列を等間隔で流す」形にしないか:
// マウスは押下・移動・解放の順序と間隔そのものが試したい対象で、
// ドラッグの途中でボタンが離れるかどうかが SX-Window の挙動を分ける。
// 等間隔で流す形だと、その組み立てを表現できない。
bool parseMouseScript(const std::string& text, std::vector<MouseEvent>* out)
{
    std::size_t pos = 0;
    while (pos <= text.size())
    {
        const std::size_t comma = text.find(',', pos);
        const std::string item =
            text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);

        if (!item.empty())
        {
            MouseEvent event;
            // 手で書く台本なので、区切りを数えて素直に切り出す。
            std::size_t field = 0;
            std::size_t fieldStart = 0;
            for (std::size_t i = 0; i <= item.size(); ++i)
            {
                const bool isEnd = i == item.size();
                if (!isEnd && item[i] != ':')
                {
                    continue;
                }
                const std::string value = item.substr(fieldStart, i - fieldStart);
                if (field == 0)
                {
                    event.atCycle = std::strtoull(value.c_str(), nullptr, 0);
                }
                else if (field == 1)
                {
                    event.dx = static_cast<int>(std::strtol(value.c_str(), nullptr, 0));
                }
                else if (field == 2)
                {
                    event.dy = static_cast<int>(std::strtol(value.c_str(), nullptr, 0));
                }
                else if (field == 3)
                {
                    event.leftButton = value.find('L') != std::string::npos ||
                                       value.find('l') != std::string::npos;
                    event.rightButton = value.find('R') != std::string::npos ||
                                        value.find('r') != std::string::npos;
                }
                else
                {
                    return false;  // 余分なフィールド
                }
                ++field;
                fieldStart = i + 1;
            }

            // サイクル・dx・dy は必須。ボタンは省略できる。
            if (field < 3)
            {
                return false;
            }
            out->push_back(event);
        }

        if (comma == std::string::npos)
        {
            break;
        }
        pos = comma + 1;
    }

    // 送る順に並べる。台本を時刻順に書かなくてよいようにする。
    for (std::size_t i = 1; i < out->size(); ++i)
    {
        MouseEvent key = (*out)[i];
        std::size_t j = i;
        while (j > 0 && (*out)[j - 1].atCycle > key.atCycle)
        {
            (*out)[j] = (*out)[j - 1];
            --j;
        }
        (*out)[j] = key;
    }
    return true;
}

void printUsage()
{
    std::printf(
        "使い方: x68k-run --iplrom PATH [オプション]\n"
        "\n"
        "  --iplrom PATH   IPL-ROM (128KB)。必須\n"
        "  --cgrom PATH    CGROM (768KB)。省略時は IPL-ROM 内蔵 6x12 ANK で代替\n"
        "  --hdd PATH      SASI ハードディスクイメージ\n"
        "  --fd0 PATH      FDD0 に入れるフロッピーイメージ (XDF / DIM)\n"
        "  --fd1 PATH      FDD1 に入れるフロッピーイメージ\n"
        "  --fd0-readonly  FDD0 をライトプロテクトする (--fd1-readonly も同様)\n"
        "  --cycles N      実行する CPU サイクル数 (既定 20000000)\n"
        "  --ppm PATH      終了時に画面 (テキスト+グラフィック合成) を PPM で書き出す\n"
        "  --text-only     --ppm でグラフィック面を合成せずテキストだけを出す\n"
        "  --gui-demo PATH GUI 経路のデモを走らせて PPM を書く (IPL-ROM 不要)\n"
        "                  SX-Window と同じ手順で G-VRAM に矩形を重ねて描く。\n"
        "                  SX-Window の起動を示すものではなく、依存する\n"
        "                  ハードウェア経路が繋がっていることの目視確認\n"
        "  --dump-text     終了時にテキスト画面を ASCII で標準出力へ出す\n"
        "  --trace         実行した命令を標準出力へ出す (大量)\n"
        "  --trace-disk    ディスクへのセクタ要求を出す\n"
        "  --keys TEXT     起動後にこの文字列をキーボードから打ち込む\n"
        "  --mouse SCRIPT  マウスを動かす。CYCLE:DX:DY[:LR] をカンマ区切りで並べる\n"
        "                  (例: 400000000:10:0:L,401000000:0:0:)\n"
        "                  DX/DY は相対量。X68000 のマウスは絶対座標を持たない\n"
        "                  IOCS がマウスを有効化するまでレポートは積まれない。\n"
        "                  起動直後の時刻に置くと全部捨てられるので、届かない\n"
        "                  ときは --trace-disk の (有効=N 受理=N) を見る\n"
        "  --watch ADDR    そのアドレスへの書き込みを報告する (16 進)\n"
        "  --trace-from A  指定アドレスに到達してからトレースを始める\n"
        "  --trace-last N  停止直前の N 命令だけを出す (既定 0 = 出さない)\n"
        "  --stats         実行した命令の内訳を最後に出す\n"
        "  --no-fast-tick  毎命令通る経路の最適化を切って走らせる\n"
        "                  付けた側と付けない側で最終状態が一致するはず\n"
        "\n"
        "ROM はライセンス上リポジトリに含まれない。NOTICE.md を参照。\n");
}

// 停止直前の命令を保持する輪状バッファ。
//
// トレースを全部出すと数百万行になって使いものにならない。
// 一方で「なぜ止まったか」を知るには直前の流れが要る。
// 直近 N 命令だけを覚えておいて、止まったときに吐く。
class TraceRing
{
public:
    struct Entry
    {
        x68k::u32 pc;
        x68k::u16 opcode;
        x68k::u32 d0;
        x68k::u32 a0;
        x68k::u32 sp;
        x68k::u16 sr;
    };

    explicit TraceRing(std::size_t capacity) : entries_(capacity) {}

    void push(const Entry& e)
    {
        if (entries_.empty())
        {
            return;
        }
        entries_[next_] = e;
        next_ = (next_ + 1) % entries_.size();
        if (count_ < entries_.size())
        {
            ++count_;
        }
    }

    void dump() const
    {
        if (count_ == 0)
        {
            return;
        }
        std::printf("\n--- 停止直前の %zu 命令 ---\n", count_);
        const std::size_t start = (next_ + entries_.size() - count_) % entries_.size();
        for (std::size_t i = 0; i < count_; ++i)
        {
            const Entry& e = entries_[(start + i) % entries_.size()];
            std::printf("%08X: %04X  D0=%08X A0=%08X SP=%08X SR=%04X\n", e.pc, e.opcode, e.d0, e.a0,
                        e.sp, e.sr);
        }
    }

private:
    std::vector<Entry> entries_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

// 実行した命令の内訳を数える。
//
// IPL-ROM がどの命令を使うかが分かれば、実装の優先順位を決められる。
// 「未実装命令で止まった」の一歩手前で、何が足りないかの見当がつく。
class OpcodeStats
{
public:
    void record(x68k::u16 opcode)
    {
        // 上位 4bit のグループごとに数える。命令語そのものを数えると
        // 65536 通りになって傾向が見えない。
        ++groupCount_[opcode >> 12];
        ++total_;
    }

    void dump() const
    {
        static const char* kGroupNames[16] = {
            "0000 即値/ビット操作", "0001 MOVE.b",  "0010 MOVE.l",  "0011 MOVE.w", "0100 misc",
            "0101 ADDQ等",          "0110 分岐",    "0111 MOVEQ",   "1000 OR/DIV", "1001 SUB",
            "1010 A-line",          "1011 CMP/EOR", "1100 AND/MUL", "1101 ADD",    "1110 シフト",
            "1111 F-line",
        };
        std::printf("\n--- 実行した命令の内訳 (計 %llu) ---\n",
                    static_cast<unsigned long long>(total_));
        for (int g = 0; g < 16; ++g)
        {
            if (groupCount_[g] == 0)
            {
                continue;
            }
            const double percent = total_ > 0 ? 100.0 * static_cast<double>(groupCount_[g]) /
                                                    static_cast<double>(total_)
                                              : 0.0;
            std::printf("  %-22s %10llu (%5.1f%%)\n", kGroupNames[g],
                        static_cast<unsigned long long>(groupCount_[g]), percent);
        }
    }

private:
    x68k::u64 groupCount_[16] = {};
    x68k::u64 total_ = 0;
};

// --- GUI 経路のデモ ---------------------------------------------------------
//
// 実体は host/gui_demo.cpp。ここは PPM への書き出しと報告だけを持つ。
//
// なぜ生成をこのファイルから追い出したか: 同じ絵をテスト
// (test/test_gui_demo.cpp) がピクセル単位で検査する。生成が 2 箇所にあると
// 「目視した絵」と「テストが見た絵」が別物になりうる。

bool runGuiDemo(const std::string& ppmPath)
{
    std::vector<x68k::u16> pixels(
        static_cast<std::size_t>(x68k::guidemo::kScreenWidth) * x68k::guidemo::kScreenHeight, 0);

    const x68k::guidemo::Result r = x68k::guidemo::run(pixels.data());
    if (!r.ok)
    {
        std::fprintf(stderr, "[gui-demo] 失敗: %s", r.failure != nullptr ? r.failure : "不明");
        if (r.haltedOpcode != 0)
        {
            std::fprintf(stderr, " (opcode %04X)", r.haltedOpcode);
        }
        std::fputc('\n', stderr);
        return false;
    }

    std::printf("[gui-demo] %u 命令を実行しました\n", r.instructions);
    std::printf("[gui-demo] マウス: SCC 有効=%d レポート受理=%d\n", r.mouseEnabled ? 1 : 0,
                r.mouseAccepted ? 1 : 0);
    std::printf("[gui-demo] ゲストが計算したカーソル位置: (%u, %u)\n", r.cursorX, r.cursorY);

    if (!writePpm(ppmPath, pixels.data(), x68k::guidemo::kScreenWidth,
                  x68k::guidemo::kScreenHeight))
    {
        std::fprintf(stderr, "PPM を書けません: %s\n", ppmPath.c_str());
        return false;
    }
    std::printf("[gui-demo] %s に書き出しました (%ux%u)\n", ppmPath.c_str(),
                x68k::guidemo::kScreenWidth, x68k::guidemo::kScreenHeight);
    std::printf(
        "[gui-demo] 注意: これは SX-Window ではありません。SX-Window の起動を\n"
        "           示すものでもありません。SX-Window が使うのと同じハードウェア\n"
        "           経路 (G-VRAM / パレット / プライオリティ合成 / SCC マウス) を\n"
        "           自前の 68000 プログラムで叩いた結果です。\n");
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string iplromPath;
    std::string cgromPath;
    std::string hddPath;
    // FDD0 / FDD1 に入れるイメージ。X68000 の内蔵ドライブは 2 台。
    std::string fdPath[x68k::Fdc::kDriveCount];
    bool fdWriteProtect[x68k::Fdc::kDriveCount] = {};
    std::string ppmPath;
    std::string guiDemoPath;
    bool dumpText = false;
    bool textOnly = false;
    std::vector<MouseEvent> mouseScript;
    // サイクル数は 64bit で持つ。
    //
    // X68000 は 10MHz なので 32bit では 7 分ぶんしか数えられない。
    // 起動を追うだけなら足りるが、Human68k がプロンプトを出すまでに
    // どれだけかかるか分からないうちは上限に余裕が要る。溢れると
    // 「指定より早く止まった」ように見えて原因が分かりにくい。
    x68k::u64 cycleLimit = 20000000;
    bool trace = false;
    bool traceDisk = false;
    std::string keys;
    x68k::u32 watchAddr = 0;
    x68k::u32 traceFrom = 0;
    bool hasTraceFrom = false;
    std::size_t traceLast = 0;
    bool showStats = false;
    bool noFastTick = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasNext = (i + 1) < argc;

        if (arg == "--iplrom" && hasNext)
        {
            iplromPath = argv[++i];
        }
        else if (arg == "--cgrom" && hasNext)
        {
            cgromPath = argv[++i];
        }
        else if (arg == "--hdd" && hasNext)
        {
            hddPath = argv[++i];
        }
        else if (arg == "--fd0" && hasNext)
        {
            fdPath[0] = argv[++i];
        }
        else if (arg == "--fd1" && hasNext)
        {
            fdPath[1] = argv[++i];
        }
        else if (arg == "--fd0-readonly")
        {
            fdWriteProtect[0] = true;
        }
        else if (arg == "--fd1-readonly")
        {
            fdWriteProtect[1] = true;
        }
        else if (arg == "--dump-text")
        {
            dumpText = true;
        }
        else if (arg == "--ppm" && hasNext)
        {
            ppmPath = argv[++i];
        }
        else if (arg == "--gui-demo" && hasNext)
        {
            guiDemoPath = argv[++i];
        }
        else if (arg == "--cycles" && hasNext)
        {
            cycleLimit = std::strtoull(argv[++i], nullptr, 0);
        }
        else if (arg == "--trace")
        {
            trace = true;
        }
        else if (arg == "--trace-disk")
        {
            traceDisk = true;
        }
        else if (arg == "--keys" && hasNext)
        {
            keys = argv[++i];
        }
        else if (arg == "--text-only")
        {
            textOnly = true;
        }
        else if (arg == "--mouse" && hasNext)
        {
            const std::string script = argv[++i];
            if (!parseMouseScript(script, &mouseScript))
            {
                std::fprintf(stderr, "--mouse の書式が不正です: %s\n", script.c_str());
                std::fprintf(stderr, "  CYCLE:DX:DY[:LR] をカンマ区切りで並べてください\n");
                return 1;
            }
        }
        else if (arg == "--watch" && hasNext)
        {
            watchAddr = static_cast<x68k::u32>(std::strtoul(argv[++i], nullptr, 16));
        }
        else if (arg == "--trace-from" && hasNext)
        {
            traceFrom = static_cast<x68k::u32>(std::strtoul(argv[++i], nullptr, 16));
            hasTraceFrom = true;
            trace = true;
        }
        else if (arg == "--trace-last" && hasNext)
        {
            traceLast = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 0));
        }
        else if (arg == "--stats")
        {
            showStats = true;
        }
        // 毎命令通る経路の最適化を切って走らせる。実機では 1 文字で
        // 切り替えるが (main/main.cpp の 'p')、ホストでは
        // 「切った側と入れた側で最終状態が一致するか」を機械的に
        // 確かめるのに使う。一致しなければ最適化が状態を変えている。
        else if (arg == "--no-fast-tick")
        {
            noFastTick = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
        else
        {
            std::fprintf(stderr, "不明な引数: %s\n", arg.c_str());
            printUsage();
            return 1;
        }
    }

    // GUI 経路のデモは IPL-ROM を使わないので、必須チェックより前に処理する。
    //
    // Why not --iplrom を要求しないか: このデモが走らせるのは自前で組んだ
    // 68000 プログラムだけで、ROM の初期化を一切通らない。ROM を要求すると
    // 「ROM が無い環境ではグラフィック経路を確かめられない」ことになり、
    // ROM が同梱できない (NOTICE.md) 本リポジトリでは目視確認そのものが
    // できなくなる。
    if (!guiDemoPath.empty())
    {
        return runGuiDemo(guiDemoPath) ? 0 : 1;
    }

    if (iplromPath.empty())
    {
        std::fprintf(stderr, "--iplrom は必須です\n\n");
        printUsage();
        return 1;
    }

    bool ok = false;
    std::vector<x68k::u8> iplrom = readFile(iplromPath, &ok);
    if (!ok)
    {
        std::fprintf(stderr, "IPL-ROM を読めません: %s\n", iplromPath.c_str());
        return 1;
    }
    if (iplrom.size() != x68k::kIplromSize)
    {
        std::fprintf(stderr, "警告: IPL-ROM のサイズが %zu バイトです (期待値 %u)\n", iplrom.size(),
                     x68k::kIplromSize);
        iplrom.resize(x68k::kIplromSize, 0xFF);
    }

    std::vector<x68k::u8> cgrom;
    if (!cgromPath.empty())
    {
        cgrom = readFile(cgromPath, &ok);
        if (!ok)
        {
            std::fprintf(stderr, "CGROM を読めません: %s\n", cgromPath.c_str());
            return 1;
        }
        cgrom.resize(x68k::kCgromSize, 0);
    }
    else
    {
        // CGROM が無いときは IPL-ROM 内蔵の 6x12 ANK フォントで代替する。
        //
        // 何もしないと IOCS は字形として 0 以外を読み、画面がベタ塗りの矩形に
        // なって Human68k の出力が読めない。英数字だけでも読めるほうが
        // デバッグの手掛かりになる。
        cgrom.assign(x68k::kCgromSize, 0);
        x68k::buildCgromFromIplRom(iplrom.data(), cgrom.data());
        std::printf("[cgrom] CGROM が指定されていないので IPL-ROM 内蔵 6x12 ANK フォントで代替\n");
    }

    FileDisk disk;
    if (!hddPath.empty() && !disk.load(hddPath))
    {
        std::fprintf(stderr, "ディスクイメージを読めません: %s\n", hddPath.c_str());
        return 1;
    }

    // フロッピー。読めなかったら起動させずに止める。
    //
    // Why not 読めなくても続行するか: 「イメージを指定したのに入っていない」
    // まま起動すると、IPL-ROM は FD を諦めて次の起動デバイスへ行く。
    // 利用者から見ると「--fd0 を付けたのに HDD から起動した」であり、
    // 原因 (長さが未対応だった) がどこにも出ない。
    FileFloppy floppy[x68k::Fdc::kDriveCount];
    for (x68k::u32 d = 0; d < x68k::Fdc::kDriveCount; ++d)
    {
        if (fdPath[d].empty())
        {
            continue;
        }
        if (!floppy[d].load(fdPath[d]))
        {
            std::fprintf(stderr,
                         "フロッピーイメージを読めません: %s\n"
                         "  対応するのは XDF (ヘッダ無しのセクタダンプ) と "
                         "DIM (256 バイトヘッダ付き) で、\n"
                         "  長さが 2HD 1261568 / 1025024、2DD 655360 / 737280、"
                         "2D 327680 バイトのもの\n"
                         "  (DIM はこれに 256 を足した長さ)。\n",
                         fdPath[d].c_str());
            return 1;
        }
        floppy[d].setWriteProtected(fdWriteProtect[d]);
        floppy[d].setTrace(traceDisk);

        const x68k::FloppyGeometry& geo = floppy[d].geometry();
        std::printf("[fd%u] %s (%s) %uC x %uH x %uS x %uB%s\n", d, fdPath[d].c_str(),
                    floppy[d].format() == x68k::FloppyFormat::Dim ? "DIM" : "XDF", geo.cylinders,
                    geo.heads, geo.sectorsPerTrack, geo.sectorSize,
                    fdWriteProtect[d] ? " 書込禁止" : "");
    }

    // メモリ領域を確保する。
    std::vector<x68k::u8> mainRam(x68k::kMainRamSize, 0);
    std::vector<x68k::u8> textVram(x68k::kTvramSize, 0);
    std::vector<x68k::u8> graphicVram(x68k::kTvramSize, 0);

    x68k::Machine machine;
    x68k::MemoryMap memory;
    memory.mainRam = mainRam.data();
    memory.textVram = textVram.data();
    memory.graphicVram = graphicVram.data();
    memory.iplRom = iplrom.data();
    memory.cgRom = cgrom.empty() ? nullptr : cgrom.data();
    machine.setMemory(memory);
    // 書き込みウォッチ。誰がそのワークを書くのかを追う。
    if (watchAddr != 0)
    {
        machine.bus().setWriteWatch(
            watchAddr,
            [](x68k::u32 addr, x68k::u32 value, void* user)
            {
                const auto* m = static_cast<const x68k::Machine*>(user);
                std::printf("[watch] $%06X <- %X  (PC=$%06X)\n", addr, value,
                            m->cpu().state().pc - 4);
            },
            &machine);
    }

    // SASI の転送バッファ。ホストでは普通に確保してよい。
    std::vector<x68k::u8> sasiBuffer(x68k::Machine::kSasiBufferBytes, 0);
    machine.setSasiBuffer(sasiBuffer.data());

    disk.setTrace(traceDisk);
    machine.setDisk(&disk);

    // フロッピーを繋ぐ。指定の無いドライブは「ディスクが入っていない」まま。
    for (x68k::u32 d = 0; d < x68k::Fdc::kDriveCount; ++d)
    {
        if (fdPath[d].empty())
        {
            continue;
        }
        machine.setFloppyDisk(d, &floppy[d]);
    }

    if (noFastTick)
    {
        x68k::PerfSwitch off;
        off.inlineRtcTick = false;
        off.inlineCrtcTick = false;
        off.inlineMfpTimer = false;
        machine.setPerfSwitch(off);
        std::printf("[perf] 毎命令通る経路の最適化を切って走らせる\n");
    }

    machine.reset();

    std::printf("[reset] SSP=%08X PC=%08X\n", machine.cpu().state().a[7],
                machine.cpu().state().pc - 4);

    // 実行。
    x68k::u64 spent = 0;
    bool tracing = trace && !hasTraceFrom;
    x68k::u64 instructions = 0;
    TraceRing ring(traceLast);
    OpcodeStats stats;

    // キー入力の進み具合。
    //
    // 打ち込みを始めるのは起動が落ち着いてから。IPL-ROM のウェイトループ中に
    // 送っても取りこぼされる。間隔を空けるのは、Human68k が 1 文字ずつ
    // 処理し終わるのを待つため。
    constexpr x68k::u32 kKeyStartCycle = 320000000;
    constexpr x68k::u32 kKeyIntervalCycles = 2000000;
    std::size_t keyIndex = 0;
    x68k::u64 nextKeyCycle = kKeyStartCycle;
    bool keyReleased = true;

    // 台本のどこまで送ったか。
    std::size_t mouseIndex = 0;

    // トレースも統計もキー入力も要らないときは、実機と同じ run() を回す。
    //
    // Why これが要るか: 実機は run() を呼ぶ (main/main.cpp)。プロファイルを
    // 実機と違う経路で取ると、効く最適化を捨てたり効かない最適化を追ったり
    // する。実際、step() 経路のプロファイルを根拠に MFP 周りへ 4 回続けて
    // 手を入れて、いずれも実機で 0.0% だった。
    //
    // (かつて run() には 8 サイクルの quantum があり、その頃は step() 経路で
    //  tickDevices が 8 倍重く見えていた。quantum は観測可能なずれを作ると
    //  分かって撤廃したので、今は両経路の tick 粒度は同じ。それでも
    //  「実機と同じ関数を回す」という原則は変わらない。)
    const bool canUseFastRun = !trace && !hasTraceFrom && traceLast == 0 && !showStats &&
                               keys.empty() && mouseScript.empty() && watchAddr == 0;
    if (canUseFastRun)
    {
        while (spent < cycleLimit)
        {
            // 1 回の run() で回す量。大きすぎると停止の検出が遅れる。
            constexpr x68k::u32 kFastRunChunk = 100000;
            const x68k::u32 chunk =
                static_cast<x68k::u32>(std::min<x68k::u64>(kFastRunChunk, cycleLimit - spent));
            const x68k::u32 used = machine.run(chunk);
            if (used == 0)
            {
                break;
            }
            spent += used;
            // run() は命令数を返さない。サイクル数から概算する
            // (統計を出さない経路なので、正確な命令数は要らない)。
            instructions += used / 4;
        }
    }

    while (spent < cycleLimit)
    {
        const auto& s = machine.cpu().state();
        const x68k::u32 pc = s.pc - 4;

        if (hasTraceFrom && !tracing && pc == traceFrom)
        {
            tracing = true;
        }
        if (tracing)
        {
            std::printf("%08X: %04X  D0=%08X A0=%08X A7=%08X SR=%04X\n", pc, s.ir, s.d[0], s.a[0],
                        s.a[7], s.sr);
        }
        if (traceLast > 0)
        {
            ring.push({pc, s.ir, s.d[0], s.a[0], s.a[7], s.sr});
        }
        if (showStats)
        {
            stats.record(s.ir);
        }

        const x68k::u32 used = machine.step();
        if (used == 0)
        {
            break;
        }
        spent += used;
        ++instructions;

        // 台本の時刻に達したマウスイベントを送る。
        //
        // 1 ステップで複数件が期限を迎えることがある (同じサイクルに
        // 並べた場合や、1 命令で数十サイクル進んだ場合) ので while で回す。
        while (mouseIndex < mouseScript.size() && spent >= mouseScript[mouseIndex].atCycle)
        {
            const MouseEvent& event = mouseScript[mouseIndex];
            const bool accepted =
                machine.moveMouse(event.dx, event.dy, event.leftButton, event.rightButton);
            if (traceDisk)
            {
                std::printf("[mouse] %zu/%zu dx=%d dy=%d L=%d R=%d (有効=%d 受理=%d)\n",
                            mouseIndex + 1, mouseScript.size(), event.dx, event.dy,
                            event.leftButton ? 1 : 0, event.rightButton ? 1 : 0,
                            machine.scc().isMouseEnabled() ? 1 : 0, accepted ? 1 : 0);
            }
            // FIFO が埋まって断られたら台本を進めない。次のステップで送り直す。
            //
            // Why not 進めてしまわないか: SCC は FIFO に 3 バイトの空きが無いと
            // レポートを丸ごと捨てる。ここで進めると、実機側 (MouseQueue) で
            // 直したのと同じ取りこぼしがホストでだけ残る。同じサイクルに
            // 押下・移動・解放を並べると解放が消え、ゲストはボタンを
            // 押しっぱなしと見なし続ける。台本でドラッグを再現しようとして
            // 「実機では動くのにホストでは終わらない」という、原因の分かりにくい
            // 食い違いになる。
            //
            // 進めないので while はここで抜ける。FIFO はゲストが引き取らないと
            // 空かないため、同じステップで再試行しても必ずまた断られる。
            //
            // Why not 無効化中も送り直さないか: moveMouse() は「FIFO が満杯」と
            // 「マウスが無効」の両方で false を返すが、意味が違う。無効なのは
            // IOCS がまだマウスを有効化していない (起動直後) か、意図して
            // 切っている状態で、待っても解消するとは限らない。ここで止めると
            // 有効化前に置いた台本で永久に足踏みし、以降のイベントが一つも
            // 流れない。無効化中に積まないのは SCC 側の判断 (有効化した瞬間に
            // 古い動きが流れ込むのを防ぐ) なので、台本はそのまま先へ進める。
            if (!accepted)
            {
                if (machine.scc().isMouseEnabled())
                {
                    break;  // 満杯。次のステップで同じイベントを送り直す
                }
                ++mouseIndex;  // 無効。このイベントは捨てて先へ進む
                continue;
            }
            ++mouseIndex;
        }

        // キーを 1 つずつ打つ。押下と解放を交互に送る。
        //
        const bool hasKeyLeft = keyIndex < keys.size();
        if (hasKeyLeft && spent >= nextKeyCycle)
        {
            const x68k::u8 code = x68k::asciiToScanCode(keys[keyIndex]);
            if (code == 0)
            {
                ++keyIndex;  // 対応していない文字は飛ばす
            }
            else if (keyReleased)
            {
                machine.pressKey(code);
                keyReleased = false;
            }
            else
            {
                machine.pressKey(static_cast<x68k::u8>(code | 0x80u));
                keyReleased = true;
                ++keyIndex;
            }
            nextKeyCycle = spent + kKeyIntervalCycles;
            if (traceDisk)
            {
                std::printf("[key] %zu/%zu code=%02X\n", keyIndex, keys.size(), code);
            }
        }
    }

    std::printf("[done] %llu 命令 / %llu サイクル実行\n",
                static_cast<unsigned long long>(instructions),
                static_cast<unsigned long long>(spent));

    if (machine.isHalted())
    {
        const auto& s = machine.cpu().state();
        std::printf("[halt] 未実装命令 %04X @ PC=%08X\n", machine.haltedOpcode(), s.pc - 4);
        std::printf("       この命令を実装してから再実行してください。\n");
        // 止まった理由を追うには直前の流れが要る。
        ring.dump();
    }
    else if (spent >= cycleLimit)
    {
        // 止まらずにサイクル上限へ達した。無限ループの可能性がある。
        std::printf("[limit] サイクル上限に達しました。--cycles で伸ばせます。\n");
        ring.dump();
    }

    if (showStats)
    {
        stats.dump();
    }

    // SRAM と MFP の状態は --stats 無しでも出す。
    // run() 経路と step() 経路が同じ結果になることを確かめるのに使う
    // (--stats を付けると step() 経路へ落ちるので、付けた状態同士でしか
    //  比べられないと意味が無い)。
    {
        std::printf("[sram] $ED0018=%04X $ED0058=%02X $ED0000=%02X%02X%02X%02X\n",
                    machine.sram().read16(0x18), machine.sram().read8(0x58),
                    machine.sram().read8(0), machine.sram().read8(1), machine.sram().read8(2),
                    machine.sram().read8(3));
    }

    if (showStats)
    {
        // テキスト VRAM に何か書かれたか。真っ黒な PPM が出たとき、
        // 描画されていないのか、ラスタライザやパレットの側で見えなく
        // なっているのかを分ける手がかりになる。
        for (x68k::u32 plane = 0; plane < x68k::kTvramPlaneCount; ++plane)
        {
            const std::size_t base = plane * x68k::kTvramPlaneSize;
            std::size_t count = 0;
            std::size_t firstLine = 0;
            for (std::size_t i = 0; i < x68k::kTvramPlaneSize; ++i)
            {
                if (textVram[base + i] == 0)
                {
                    continue;
                }
                if (count == 0)
                {
                    firstLine = i / x68k::kTvramBytesPerLine;
                }
                ++count;
            }
            std::printf("[tvram] plane%u: %zu バイト 最初の行 %zu\n", plane, count, firstLine);
        }
        std::printf("[mfp] IERA=%02X IPRA=%02X IMRA=%02X RSR=%02X UDR=%02X SR=%04X\n",
                    machine.mfp().peek(0x03), machine.mfp().peek(0x05), machine.mfp().peek(0x09),
                    machine.mfp().peek(0x15), machine.mfp().peek(0x17), machine.cpu().state().sr);

        // テキストパレット。全部黒だと、描かれていても PPM は真っ黒になる。
        for (x68k::u32 i = 0; i < x68k::VideoController::kTextPaletteCount; ++i)
        {
            std::printf("[pal] %2u: %04X\n", i, machine.video().textPalette(i));
        }
    }

    if (dumpText)
    {
        // 実機はシリアルへ同じものを出す。ホストで先に確かめておくと、
        // 実機で読めなかったときに逆引き側の問題を切り分けられる。
        std::printf("---- テキスト画面 ----\n");
        char line[x68k::TextScrape::kColumns + 1];
        for (x68k::u32 row = 0; row < x68k::TextScrape::kRows; ++row)
        {
            x68k::TextScrape::readRow(textVram.data(), cgrom.data(), row, line);
            std::printf("%2u|%s\n", row, line);
        }
        std::printf("----------------------\n");
    }

    if (!ppmPath.empty())
    {
        // Human68k の標準コンソールは 768x512。
        constexpr x68k::u32 kWidth = 768;
        constexpr x68k::u32 kHeight = 512;
        std::vector<x68k::u16> pixels(static_cast<std::size_t>(kWidth) * kHeight, 0);

        // 既定で合成する。
        //
        // Why not テキストだけを出し続けないか: G-VRAM に描いた絵は
        // テキスト画面のどこにも現れない。合成しないと、SX-Window が
        // 正しく描けているのか描けていないのかをホストで判別できず、
        // 実機に焼くまで分からなくなる。実機に焼かずにデバッグできることが
        // このランナーの存在理由 (ファイル冒頭)。
        //
        // --text-only を残すのは切り分けのため。合成した絵が乱れたとき、
        // グラフィック面の問題かテキスト面の問題かを分けられる。
        if (textOnly)
        {
            x68k::TextRaster::render(textVram.data(), machine.video(), 0, 0, kWidth, kHeight,
                                     pixels.data(), kWidth);
        }
        else
        {
            x68k::GraphicRaster::composite(graphicVram.data(), textVram.data(), machine.video(), 0,
                                           0, kWidth, kHeight, pixels.data(), kWidth);
        }

        if (writePpm(ppmPath, pixels.data(), kWidth, kHeight))
        {
            std::printf("[ppm] %s に書き出しました (%ux%u, %s)\n", ppmPath.c_str(), kWidth, kHeight,
                        textOnly ? "テキストのみ" : "テキスト+グラフィック合成");
        }
        else
        {
            std::fprintf(stderr, "PPM を書けません: %s\n", ppmPath.c_str());
            return 1;
        }
    }

    return machine.isHalted() ? 2 : 0;
}
