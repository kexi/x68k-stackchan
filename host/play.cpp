// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// X68000 を対話的に動かす。窓に画面を出し、キーボードをゲストへ渡す。
//
// なぜ x68k-run と別にするか: x68k-run は「決まったサイクルだけ走らせて
// 結果を吐く」バッチの道具で、自動テストと切り分けの土台になっている。
// そこに窓と実時間のペース配分を混ぜると、テストの決定性を壊しかねない。
// 対話は別の入口に分ける。
//
// ここが担うのは 3 つだけ:
//   1. 1 フレームぶん走らせて合成し、テクスチャへ流す
//   2. SDL のキーイベントを X68000 のスキャンコードへ直して渡す
//   3. 実時間に合わせて待つ (速すぎると遊べない)

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dev/video.h"
#include "io/ascii_keymap.h"
#include "machine.h"
#include "memmap.h"
#include "video/compositor.h"

namespace
{

// X68000 の実画面。合成はこの大きさで行う。
constexpr x68k::u32 kSrcWidth = 768;
constexpr x68k::u32 kSrcHeight = 512;

// 窓に出す範囲。ゲームは 256x240 の座標系で描くので、その左上を切り出す。
//
// Why not 768x512 全体を出さないか: Human68k のコンソールは 768x512 だが、
// ゲームが使うのは左上だけ。全体を出すと絵が小さくなり、遊ぶには辛い。
constexpr x68k::u32 kViewWidth = 256;
constexpr x68k::u32 kViewHeight = 240;
constexpr int kZoom = 3;

// ファイルを丸ごと読むだけのディスク。書き込みはメモリ上にとどめる
// (対話中に元のイメージを壊さないため。SRAM の保存とは別の話)。
class FileDisk final : public x68k::DiskImage
{
public:
    bool load(const std::string& path);

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

    std::vector<x68k::u8> data_;
};

bool readFile(const std::string& path, std::vector<x68k::u8>& out)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// SDL のキーを X68000 のスキャンコードへ。
//
// ASCII 経由 (asciiToScanCode) を使わないのは、あれが英数字と改行しか
// 扱えないため。矢印キーやスペースを渡すには生のコードが要る。
x68k::u8 scanCodeFor(SDL_Keycode key)
{
    switch (key)
    {
        case SDLK_1:
            return 0x02;
        case SDLK_2:
            return 0x03;
        case SDLK_3:
            return 0x04;
        case SDLK_4:
            return 0x05;
        case SDLK_q:
            return 0x10;
        case SDLK_w:
            return 0x11;
        case SDLK_e:
            return 0x12;
        case SDLK_r:
            return 0x13;
        case SDLK_a:
            return 0x1E;
        case SDLK_s:
            return 0x1F;
        case SDLK_d:
            return 0x20;
        case SDLK_f:
            return 0x21;
        case SDLK_g:
            return 0x22;
        case SDLK_h:
            return 0x23;
        case SDLK_j:
            return 0x24;
        case SDLK_k:
            return 0x25;
        case SDLK_l:
            return 0x26;
        case SDLK_z:
            return 0x2C;
        case SDLK_x:
            return 0x2D;
        case SDLK_c:
            return 0x2E;
        case SDLK_v:
            return 0x2F;
        case SDLK_b:
            return 0x30;
        case SDLK_n:
            return 0x31;
        case SDLK_m:
            return 0x32;
        case SDLK_SPACE:
            return 0x35;
        case SDLK_RETURN:
            return 0x1D;
        case SDLK_BACKSPACE:
            return 0x0F;
        case SDLK_LEFT:
            return 0x3B;
        case SDLK_UP:
            return 0x3C;
        case SDLK_RIGHT:
            return 0x3D;
        case SDLK_DOWN:
            return 0x3E;
        case SDLK_PERIOD:
            return 0x33;
        case SDLK_MINUS:
            return 0x0C;
        default:
            return 0;
    }
}

void printUsage()
{
    std::printf(
        "使い方: x68k-play --iplrom PATH [オプション]\n"
        "\n"
        "  --iplrom PATH   IPL-ROM (128KB)。必須\n"
        "  --cgrom PATH    CGROM (768KB)。省略時は IPL-ROM 内蔵 6x12 ANK で代替\n"
        "  --hdd PATH      SASI ハードディスクイメージ\n"
        "  --keys TEXT     起動後にこの文字列を打ち込む (ゲームの起動コマンド用)\n"
        "  --fast-cycles N 最初の N サイクルは全速で走らせる (既定 400000000)\n"
        "                  Human68k の起動を待つ間、実時間に合わせると遅すぎるため\n"
        "  --zoom N        表示倍率 (既定 3)\n"
        "  --full          768x512 全体を出す (既定は左上 256x240)\n"
        "  --shot PATH     指定フレームの合成結果を PPM で書いて終わる\n"
        "  --shot-frame N  --shot を撮るフレーム (既定 120)\n"
        "\n"
        "終了は窓を閉じるか ESC。\n");
}

bool FileDisk::load(const std::string& path)
{
    return readFile(path, data_);
}

}  // namespace

int main(int argc, char** argv)
{
    std::string iplPath;
    std::string cgromPath;
    std::string hddPath;
    std::string keys;
    x68k::u64 fastCycles = 400000000;
    int zoom = kZoom;
    bool full = false;
    std::string shotPath;
    unsigned long shotFrame = 120;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == "--iplrom" && hasNext)
        {
            iplPath = argv[++i];
        }
        else if (arg == "--cgrom" && hasNext)
        {
            cgromPath = argv[++i];
        }
        else if (arg == "--hdd" && hasNext)
        {
            hddPath = argv[++i];
        }
        else if (arg == "--keys" && hasNext)
        {
            keys = argv[++i];
        }
        else if (arg == "--fast-cycles" && hasNext)
        {
            fastCycles = std::strtoull(argv[++i], nullptr, 0);
        }
        else if (arg == "--zoom" && hasNext)
        {
            zoom = std::atoi(argv[++i]);
        }
        else if (arg == "--shot" && hasNext)
        {
            shotPath = argv[++i];
        }
        else if (arg == "--shot-frame" && hasNext)
        {
            shotFrame = std::strtoul(argv[++i], nullptr, 0);
        }
        else if (arg == "--full")
        {
            full = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
        else
        {
            std::printf("不明な引数: %s\n\n", arg.c_str());
            printUsage();
            return 1;
        }
    }

    if (iplPath.empty())
    {
        printUsage();
        return 1;
    }

    std::vector<x68k::u8> iplrom;
    if (!readFile(iplPath, iplrom))
    {
        std::printf("IPL-ROM を読めません: %s\n", iplPath.c_str());
        return 1;
    }

    std::vector<x68k::u8> cgrom;
    if (!cgromPath.empty() && !readFile(cgromPath, cgrom))
    {
        std::printf("CGROM を読めません: %s\n", cgromPath.c_str());
        return 1;
    }

    FileDisk disk;
    if (!hddPath.empty() && !disk.load(hddPath))
    {
        std::printf("ディスクを開けません: %s\n", hddPath.c_str());
        return 1;
    }

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

    std::vector<x68k::u8> sasiBuffer(x68k::Machine::kSasiBufferBytes, 0);
    machine.setSasiBuffer(sasiBuffer.data());

    static std::vector<std::uint16_t> codeGen(x68k::kMainRamSize / x68k::CodeGenMap::kPageSize, 0);
    machine.cpu().codeGenMap().setStorage(codeGen.data(), static_cast<x68k::u32>(codeGen.size()));

    if (!hddPath.empty())
    {
        machine.setDisk(&disk);
    }
    machine.setEventDriven(true);
    machine.reset();

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::printf("SDL を初期化できません: %s\n", SDL_GetError());
        return 1;
    }

    const x68k::u32 viewW = full ? kSrcWidth : kViewWidth;
    const x68k::u32 viewH = full ? kSrcHeight : kViewHeight;

    SDL_Window* window =
        SDL_CreateWindow("x68k-play", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         static_cast<int>(viewW) * zoom, static_cast<int>(viewH) * zoom, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                          static_cast<int>(viewW), static_cast<int>(viewH));
    if (window == nullptr || renderer == nullptr || texture == nullptr)
    {
        std::printf("SDL の初期化に失敗しました: %s\n", SDL_GetError());
        return 1;
    }

    std::vector<x68k::u16> pixels(static_cast<std::size_t>(viewW) * viewH, 0);

    // 起動が済むまでは全速で走らせる。
    //
    // ホストは実機の数十倍の速さで回るので、最初から実時間に合わせると
    // Human68k の起動 (実機で約 40 秒ぶん) をそのまま待つことになる。
    std::printf("[boot] %llu サイクルぶん全速で走らせます\n",
                static_cast<unsigned long long>(fastCycles));
    x68k::u64 spent = 0;
    while (spent < fastCycles)
    {
        spent += machine.run(x68k::Crtc::kCyclesPerFrame);
    }

    // 起動コマンドを打ち込む。
    for (char c : keys)
    {
        const x68k::u8 code = x68k::asciiToScanCode(c);
        if (code == 0)
        {
            continue;
        }
        machine.pressKey(code);
        machine.run(200000);
        machine.pressKey(static_cast<x68k::u8>(code | 0x80u));
        machine.run(200000);
    }

    std::printf("[play] 窓を閉じるか ESC で終了します\n");

    bool running = true;
    unsigned long frameNo = 0;
    while (running)
    {
        const x68k::u32 frameStart = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
            {
                running = false;
            }
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
            {
                if (e.key.keysym.sym == SDLK_ESCAPE)
                {
                    running = false;
                    continue;
                }
                // キーリピートは無視する。X68000 のキーボードは
                // 押しっぱなしで同じコードを送り続けないので、
                // そのまま流すとゲスト側の押下状態が乱れる。
                if (e.key.repeat != 0)
                {
                    continue;
                }
                const x68k::u8 code = scanCodeFor(e.key.keysym.sym);
                if (code == 0)
                {
                    continue;
                }
                machine.pressKey(e.type == SDL_KEYDOWN ? code
                                                       : static_cast<x68k::u8>(code | 0x80u));
            }
        }

        machine.run(x68k::Crtc::kCyclesPerFrame);

        x68k::Compositor::render(graphicVram.data(), textVram.data(), &machine.sprite(),
                                 machine.video(), 0, 0, viewW, viewH, pixels.data(), viewW);

        SDL_UpdateTexture(texture, nullptr, pixels.data(),
                          static_cast<int>(viewW) * static_cast<int>(sizeof(x68k::u16)));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        // 切り分け用のスクリーンショット。窓を開けない環境でも
        // 「絵が出ているか」を機械的に確かめられるようにする。
        if (!shotPath.empty() && frameNo == shotFrame)
        {
            std::FILE* f = std::fopen(shotPath.c_str(), "wb");
            if (f != nullptr)
            {
                std::fprintf(f, "P6\n%u %u\n255\n", viewW, viewH);
                for (std::size_t i = 0; i < pixels.size(); ++i)
                {
                    const x68k::u16 c = pixels[i];
                    const unsigned char rgb[3] = {
                        static_cast<unsigned char>(((c >> 11) & 0x1F) << 3),
                        static_cast<unsigned char>(((c >> 5) & 0x3F) << 2),
                        static_cast<unsigned char>((c & 0x1F) << 3)};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
                std::printf("[shot] %s に書き出しました\n", shotPath.c_str());
            }
            running = false;
        }
        ++frameNo;

        // 55.45Hz に合わせる。1 フレーム約 18ms。
        constexpr x68k::u32 kFrameMs = 18;
        const x68k::u32 elapsed = SDL_GetTicks() - frameStart;
        if (elapsed < kFrameMs)
        {
            SDL_Delay(kFrameMs - elapsed);
        }
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
