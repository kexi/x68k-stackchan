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

    bool readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount) override
    {
        constexpr x68k::u32 kSectorSize = 256;
        const std::size_t offset = static_cast<std::size_t>(lba) * kSectorSize;
        const std::size_t length = static_cast<std::size_t>(sectorCount) * kSectorSize;
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
        "  --trace-from A  指定アドレスに到達してからトレースを始める\n"
        "\n"
        "ROM はライセンス上リポジトリに含まれない。NOTICE.md を参照。\n");
}

}  // namespace

int main(int argc, char** argv)
{
    std::string iplromPath;
    std::string cgromPath;
    std::string hddPath;
    std::string ppmPath;
    x68k::u32 cycleLimit = 20000000;
    bool trace = false;
    x68k::u32 traceFrom = 0;
    bool hasTraceFrom = false;

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
            cycleLimit = static_cast<x68k::u32>(std::strtoul(argv[++i], nullptr, 0));
        }
        else if (arg == "--trace")
        {
            trace = true;
        }
        else if (arg == "--trace-from" && hasNext)
        {
            traceFrom = static_cast<x68k::u32>(std::strtoul(argv[++i], nullptr, 16));
            hasTraceFrom = true;
            trace = true;
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
    machine.setDisk(&disk);

    machine.reset();

    std::printf("[reset] SSP=%08X PC=%08X\n", machine.cpu().state().a[7],
                machine.cpu().state().pc - 4);

    // 実行。
    x68k::u32 spent = 0;
    bool tracing = trace && !hasTraceFrom;
    x68k::u64 instructions = 0;

    while (spent < cycleLimit)
    {
        const x68k::u32 pc = machine.cpu().state().pc - 4;

        if (hasTraceFrom && !tracing && pc == traceFrom)
        {
            tracing = true;
        }
        if (tracing)
        {
            const auto& s = machine.cpu().state();
            std::printf("%08X: %04X  D0=%08X A0=%08X A7=%08X SR=%04X\n", pc, s.ir, s.d[0], s.a[0],
                        s.a[7], s.sr);
        }

        const x68k::u32 used = machine.step();
        if (used == 0)
        {
            break;
        }
        spent += used;
        ++instructions;
    }

    std::printf("[done] %llu 命令 / %u サイクル実行\n",
                static_cast<unsigned long long>(instructions), spent);

    if (machine.isHalted())
    {
        const auto& s = machine.cpu().state();
        std::printf("[halt] 未実装命令 %04X @ PC=%08X\n", machine.haltedOpcode(), s.pc - 4);
        std::printf("       この命令を実装してから再実行してください。\n");
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
