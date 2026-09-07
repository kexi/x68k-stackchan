// SPDX-License-Identifier: MIT
#include <array>
#include <vector>

#include "doctest.h"
#include "machine.h"
#include "video/tiled_compositor.h"

namespace
{
struct Scene
{
    x68k::Machine machine;
    std::vector<x68k::u8> text = std::vector<x68k::u8>(x68k::kTvramSize);
    std::vector<x68k::u8> graphic = std::vector<x68k::u8>(0x80000);
    x68k::MemoryMap memory{};
    x68k::TiledCompositor tiled;
    std::array<std::vector<x68k::u16>, 2> buffers{std::vector<x68k::u16>(320 * 240, 0x1234),
                                                  std::vector<x68k::u16>(320 * 240, 0x5678)};
    std::vector<x68k::u16> reference = std::vector<x68k::u16>(320 * 240);
    x68k::u32 viewX = 0;
    x68k::u32 viewY = 0;

    Scene()
    {
        memory.textVram = text.data();
        memory.graphicVram = graphic.data();
        machine.setMemory(memory);
        machine.sprite().setVisualDamage(tiled.observer());
        machine.video().setVisualDamage(tiled.observer());
        machine.bus().setVisualDamage(tiled.observer());
        machine.video().reset();
        machine.sprite().reset();
        for (x68k::u32 i = 0; i < 256; ++i)
        {
            machine.bus().write16(0xe82000 + i * 2, static_cast<x68k::u16>(i * 193));
        }
        for (x68k::u32 i = 0; i < 16; ++i)
        {
            machine.bus().write16(0xe82200 + i * 2, static_cast<x68k::u16>(i * 3809));
        }
        machine.bus().write16(0xe82600, 0x60);
        machine.bus().write16(0xeb0808, 0x200);
        for (x68k::u32 i = 0; i < 128; ++i)
        {
            machine.bus().write8(0xeb8000 + i, static_cast<x68k::u8>((i * 37) | 1));
        }
    }

    x68k::u32 check(x68k::u32 buffer)
    {
        x68k::Compositor::render(graphic.data(), text.data(), &machine.sprite(), machine.video(),
                                 viewX, viewY, 320, 240, reference.data(), 320, &machine.crtc());
        const auto count =
            tiled.render(graphic.data(), text.data(), &machine.sprite(), machine.video(),
                         buffers[buffer].data(), buffer, &machine.crtc());
        CHECK(buffers[buffer] == reference);
        return count;
    }
};
}  // namespace

TEST_CASE("MODE3 register-only pageflipは両bufferへ届き同じ有効scrollは0tile")
{
    Scene scene;
    auto& bus = scene.machine.bus();
    bus.write16(0xE80028, 0x0300);
    bus.write16(0xE82400, 3);
    bus.write16(0xE82600, 0x1F);
    for (x68k::u32 y = 0; y < 512; ++y)
        for (x68k::u32 x = 0; x < 512; ++x)
            bus.write16(0xC00000 + (y * 512u + x) * 2u,
                        static_cast<x68k::u16>(((x + y * 313u) & 0xFFFEu) | 1u));
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);
    for (const x68k::u16 scroll : std::initializer_list<x68k::u16>{256, 511, 0, 1})
    {
        bus.write16(0xE8001A, scroll);
        CHECK(scene.check(0) == 300);
        CHECK(scene.check(0) == 0);
        CHECK(scene.check(1) == 300);
        CHECK(scene.check(1) == 0);
        bus.write16(0xE8001A, static_cast<x68k::u16>(scroll | 0xFE00u));
        CHECK(scene.check(0) == 0);
        CHECK(scene.check(1) == 0);
        CHECK(scene.reference.front() == x68k::VideoController::toRgb565(static_cast<x68k::u16>(
                                             ((scroll * 313u) & 0xFFFEu) | 1u)));
    }
    // byte MMIOもG0へ届き、G1..3はMODE3の表示を変えない。
    bus.write8(0xE80018, 1);
    bus.write8(0xE80019, 0xFF);
    CHECK(scene.check(1) == 300);
    CHECK(scene.check(0) == 300);
    for (x68k::u32 reg = 14; reg < 20; ++reg)
        bus.write16(0xE80000 + reg * 2u, 37);
    CHECK(scene.check(0) == 0);
    CHECK(scene.check(1) == 0);
    scene.viewX = 507;
    scene.viewY = 509;
    scene.tiled.setViewport(scene.viewX, scene.viewY);
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);

    // nullptr互換経路との往復も鍵が違い、前回のscroll済みframeを使わない。
    for (x68k::u32 buffer = 0; buffer < 2; ++buffer)
        CHECK(scene.tiled.render(scene.graphic.data(), scene.text.data(), &scene.machine.sprite(),
                                 scene.machine.video(), scene.buffers[buffer].data(),
                                 buffer) == 300);
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);
    bus.write16(0xE82400, 0);
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);
    bus.write16(0xE82400, 3);
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);
    // 隠れた行の更新後でも次の表示位置が正しい。
    bus.write16(0xC00000 + 256u * 1024u, 0xBEEF);
    bus.write16(0xE80018, 0);
    bus.write16(0xE8001A, 256);
    scene.viewX = scene.viewY = 0;
    scene.tiled.setViewport(0, 0);
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);
    CHECK(scene.reference.front() == x68k::VideoController::toRgb565(0xBEEF));
}

TEST_CASE("局所合成は移動/非表示/属性/byte RMWと二枚の遅れを全面参照と一致させる")
{
    Scene scene;
    auto& bus = scene.machine.bus();
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);
    CHECK(scene.check(0) == 0);
    for (x68k::u32 frame = 0; frame < 120; ++frame)
    {
        CAPTURE(frame);
        bus.write16(0xeb0000, static_cast<x68k::u16>((frame * 7) % 370));
        bus.write16(0xeb0002, static_cast<x68k::u16>((frame * 3) % 280));
        bus.write16(0xeb0004, static_cast<x68k::u16>((frame % 4) << 8));
        bus.write8(0xeb0007, static_cast<x68k::u8>(frame % 4));
        const auto buffer = frame % 3 == 0 ? 0u : 1u;
        CHECK(scene.check(buffer) < 300);
        // 公開しなかったバッファを続けて使っても再合成不要。
        CHECK(scene.check(buffer) == 0);
    }
    CHECK(scene.check(0) < 300);
    CHECK(scene.check(1) < 300);
}

TEST_CASE("局所合成はT/G VRAM全plane・モード・palette・BG・resetとviewportを追跡する")
{
    Scene scene;
    auto& bus = scene.machine.bus();
    for (x68k::u32 frame = 0; frame < 96; ++frame)
    {
        CAPTURE(frame);
        const x68k::u32 offset = ((frame * 3) % 240) * 128 + frame % 40;
        bus.write8(0xe00000 + (frame % 4) * x68k::kTvramPlaneSize + offset,
                   static_cast<x68k::u8>(frame * 91));
        if (const bool word = frame % 3 == 0; word)
        {
            bus.write16(0xe00000 + (offset & ~1u), 0xabcd);
        }
        if (const bool mode = frame % 8 == 0; mode)
        {
            bus.write16(0xe82400, static_cast<x68k::u16>((frame / 8) % 8));
            bus.write16(0xe82600, static_cast<x68k::u16>(0x60 | (frame / 8)));
            bus.write16(0xe82500, static_cast<x68k::u16>(frame * 91));
            bus.write8(0xe82203, static_cast<x68k::u8>(frame * 13));
        }
        bus.write16(0xc00000 + (frame % 4) * 0x80000 + frame * 514,
                    static_cast<x68k::u16>(frame * 631));
        bus.write8(0xc00001 + (frame % 4) * 0x80000 + frame * 514, 0x91);
        if (const bool bg = frame % 5 == 0; bg)
        {
            bus.write16(0xeb0808, 0x219);
            bus.write16(0xeb0800, static_cast<x68k::u16>(frame * 31));
            bus.write16(0xeb0806, static_cast<x68k::u16>(frame * 17));
            bus.write16(0xebc000 + frame * 2, static_cast<x68k::u16>(frame * 319));
            bus.write8(0xeb8000 + frame, static_cast<x68k::u8>(frame * 17));
        }
        if (const bool viewport = frame % 11 == 0; viewport)
        {
            scene.viewX = (frame * 29) % 800;
            scene.viewY = (frame * 19) % 850;
            scene.tiled.setViewport(scene.viewX, scene.viewY);
        }
        scene.check(frame % 2);
    }
    scene.machine.sprite().reset();
    scene.check(0);
    scene.machine.video().reset();
    scene.check(1);
    scene.text[0] = 0xff;
    scene.machine.setMemory(scene.memory);
    CHECK(scene.check(0) == 300);
    CHECK(scene.check(1) == 300);
}

TEST_CASE("同値のsprite/BG/PCG/palette書込みは合成を増やさず無効な描画はdirtyを消さない")
{
    Scene scene;
    scene.check(0);
    scene.check(1);
    auto& machine = scene.machine;
    machine.sprite().write(0, machine.sprite().read(0));
    machine.sprite().write(0x800, machine.sprite().read(0x800));
    machine.sprite().vramWrite8(0, machine.sprite().vramRead8(0));
    machine.video().write(0x200, machine.video().read(0x200));
    CHECK(scene.check(0) == 0);
    scene.tiled.invalidateAll();
    CHECK(scene.tiled.render(nullptr, nullptr, nullptr, machine.video(), nullptr, 0) == 0);
    CHECK(scene.tiled.render(nullptr, nullptr, nullptr, machine.video(), scene.reference.data(),
                             2) == 0);
    CHECK(scene.check(0) == 300);
}

TEST_CASE("T-VRAMの各planeとDMA書込みはviewport内の必要tileだけを更新する")
{
    Scene scene;
    scene.viewX = 8;
    scene.viewY = 17;
    scene.tiled.setViewport(scene.viewX, scene.viewY);
    scene.check(0);
    scene.check(1);
    for (x68k::u32 plane = 0; plane < 4; ++plane)
    {
        const auto address = 0xe00000 + plane * x68k::kTvramPlaneSize + 17 * 128 + 1;
        scene.machine.dmaMemWrite(address, 0x81);
        CHECK(scene.check(0) == 1);
        CHECK(scene.check(1) == 1);
        CHECK(scene.check(0) == 0);
    }
    scene.machine.bus().write16(0xe00000 + 18 * 128 + 2, 0xabcd);
    CHECK(scene.check(0) == 2);
    CHECK(scene.check(1) == 2);
    scene.machine.bus().write8(0xe00000, 0xff);
    CHECK(scene.check(0) == 0);
    CHECK(scene.check(1) == 0);
    const x68k::u8 bytes[] = {0x91, 0x23};
    CHECK_FALSE(scene.machine.tryDmaMemWriteBlock(0xe00000, bytes, 2));
}
