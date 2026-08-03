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
//   x68k-run --iplrom rom/iplrom.dat [--hdd rom/hdd0.hdf] [--ppm out.ppm]
//            [--trace] [--cycles N]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "machine.h"
#include "video/text_raster.h"

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
    std::fclose(f);
    return true;
}

// ASCII から X68000 のキーボードスキャンコードへの対応。
//
// 起動確認に要るのは英数字と改行だけなので、その範囲に絞る。
// X68000 のキーボードは押下でスキャンコード、離すと bit7 を立てた値を送る。
x68k::u8 scanCodeFor(char c)
{
    // 数字列とアルファベットは並びが連続していないので表で持つ。
    static const char* kRow1 = "1234567890-^\\";
    static const char* kRow2 = "qwertyuiop@[";
    static const char* kRow3 = "asdfghjkl;:]";
    static const char* kRow4 = "zxcvbnm,./";

    if (c >= 'A' && c <= 'Z')
    {
        c = static_cast<char>(c - 'A' + 'a');
    }

    if (const char* p = std::strchr(kRow1, c); p != nullptr && c != '\0')
    {
        return static_cast<x68k::u8>(0x02 + (p - kRow1));
    }
    if (const char* p = std::strchr(kRow2, c); p != nullptr && c != '\0')
    {
        return static_cast<x68k::u8>(0x10 + (p - kRow2));
    }
    if (const char* p = std::strchr(kRow3, c); p != nullptr && c != '\0')
    {
        return static_cast<x68k::u8>(0x1E + (p - kRow3));
    }
    if (const char* p = std::strchr(kRow4, c); p != nullptr && c != '\0')
    {
        return static_cast<x68k::u8>(0x2A + (p - kRow4));
    }
    if (c == ' ')
    {
        return 0x35;
    }
    if (c == '\n' || c == '\r')
    {
        return 0x1D;  // CR
    }
    return 0;
}

void printUsage()
{
    std::printf(
        "使い方: x68k-run --iplrom PATH [オプション]\n"
        "\n"
        "  --iplrom PATH   IPL-ROM (128KB)。必須\n"
        "  --cgrom PATH    CGROM (768KB)。省略可\n"
        "  --hdd PATH      SASI ハードディスクイメージ\n"
        "  --cycles N      実行する CPU サイクル数 (既定 20000000)\n"
        "  --ppm PATH      終了時にテキスト画面を PPM で書き出す\n"
        "  --trace         実行した命令を標準出力へ出す (大量)\n"
        "  --trace-disk    ディスクへのセクタ要求を出す\n"
        "  --keys TEXT     起動後にこの文字列をキーボードから打ち込む\n"
        "  --watch ADDR    そのアドレスへの書き込みを報告する (16 進)\n"
        "  --trace-from A  指定アドレスに到達してからトレースを始める\n"
        "  --trace-last N  停止直前の N 命令だけを出す (既定 0 = 出さない)\n"
        "  --stats         実行した命令の内訳を最後に出す\n"
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

}  // namespace

int main(int argc, char** argv)
{
    std::string iplromPath;
    std::string cgromPath;
    std::string hddPath;
    std::string ppmPath;
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
        else if (arg == "--ppm" && hasNext)
        {
            ppmPath = argv[++i];
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

    FileDisk disk;
    if (!hddPath.empty() && !disk.load(hddPath))
    {
        std::fprintf(stderr, "ディスクイメージを読めません: %s\n", hddPath.c_str());
        return 1;
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

        // キーを 1 つずつ打つ。押下と解放を交互に送る。
        //
        const bool hasKeyLeft = keyIndex < keys.size();
        if (hasKeyLeft && spent >= nextKeyCycle)
        {
            const x68k::u8 code = scanCodeFor(keys[keyIndex]);
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

    if (showStats)
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

    if (!ppmPath.empty())
    {
        // Human68k の標準コンソールは 768x512。
        constexpr x68k::u32 kWidth = 768;
        constexpr x68k::u32 kHeight = 512;
        std::vector<x68k::u16> pixels(static_cast<std::size_t>(kWidth) * kHeight, 0);
        x68k::TextRaster::render(textVram.data(), machine.video(), 0, 0, kWidth, kHeight,
                                 pixels.data(), kWidth);
        if (writePpm(ppmPath, pixels.data(), kWidth, kHeight))
        {
            std::printf("[ppm] %s に書き出しました (%ux%u)\n", ppmPath.c_str(), kWidth, kHeight);
        }
        else
        {
            std::fprintf(stderr, "PPM を書けません: %s\n", ppmPath.c_str());
            return 1;
        }
    }

    return machine.isHalted() ? 2 : 0;
}
