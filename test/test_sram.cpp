// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: SRAM の工場出荷状態が、IPL-ROM の期待する形になっていること。
//
// IPL-ROM は起動時にマジックを検査し、メモリ容量と起動デバイスの設定を読む。
// ここが 1 バイトでも違うと起動シーケンスが先へ進まないが、症状は
// 「何も起きずに固まる」なので原因が非常に追いにくい。先に押さえておく。

// あわせて、バッテリバックアップとしての振る舞いも保証する。実機の SRAM は
// リセットしても電源を切っても内容が残る。エミュレータではリセットで消さない
// ことと、保存イメージを安全に取り込めることがそれに当たる。

#include <array>
#include <cstdint>

#include "dev/sram.h"
#include "doctest.h"
#include "machine.h"

namespace
{

// 保存イメージを模したバイト列。正しいマジックを持つ 16KB。
std::array<std::uint8_t, x68k::kSramSize> makeValidImage()
{
    std::array<std::uint8_t, x68k::kSramSize> image{};
    const x68k::Sram defaults;  // 構築時点で工場出荷状態
    for (std::uint32_t i = 0; i < x68k::kSramSize; ++i)
    {
        image[i] = defaults.read8(i);
    }
    return image;
}

}  // namespace

TEST_CASE("工場出荷状態のマジックが正しい")
{
    x68k::Sram sram;
    CHECK(sram.hasValidMagic());

    // 「Ｘ68000」+ $57 の 8 バイト。先頭は半角の 'X' ではなく
    // Shift_JIS の全角「Ｘ」($82 $77)。実機の IPL-ROM が書く値と一致する
    // ことが要件で、半角だとマジック不正として SRAM を書き戻される。
    CHECK(sram.read8(0) == 0x82);
    CHECK(sram.read8(1) == 0x77);
    CHECK(sram.read8(2) == '6');
    CHECK(sram.read8(3) == '8');
    CHECK(sram.read8(4) == '0');
    CHECK(sram.read8(5) == '0');
    CHECK(sram.read8(6) == '0');
    CHECK(sram.read8(7) == 0x57);
}

TEST_CASE("メモリ容量がバスの実装と一致する")
{
    x68k::Sram sram;
    const std::uint32_t ramSize =
        (static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize)) << 24) |
        (static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize + 1)) << 16) |
        (static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize + 2)) << 8) |
        static_cast<std::uint32_t>(sram.read8(x68k::Sram::kOffsetRamSize + 3));

    // ここが実際のバスの実装容量とずれると、Human68k が存在しない領域を
    // 使おうとして壊れる。
    CHECK(ramSize == x68k::kMainRamSize);
}

TEST_CASE("起動デバイスが SASI 優先に設定される")
{
    x68k::Sram sram;
    // 標準優先順位 ($0000) だと FD から探し始める。FDC が実際に読み書き
    // できるようになった今、標準にしても IPL-ROM の待ちループには入らない
    // (ホストで確認済み) が、実物の Human68k イメージで「今までどおり
    // 起動する」ことを確かめられていない。既定値を変えて Human68k が
    // 起動しなくなる退行の方が明確に悪いので、SASI 優先のままにしてある。
    // 理由の全文は sram.cpp の formatDefaults にある。
    CHECK(sram.read16(x68k::Sram::kOffsetBootDevice) == x68k::Sram::kBootDeviceSasi0);
}

TEST_CASE("起動時画面モードが 16 である")
{
    x68k::Sram sram;
    CHECK(sram.read8(x68k::Sram::kOffsetScreenMode) == 16);
}

TEST_CASE("書き込みが読み戻せて、dirty が立つ")
{
    x68k::Sram sram;
    CHECK_FALSE(sram.isDirty());

    sram.write8(0x100, 0xAB);
    CHECK(sram.read8(0x100) == 0xAB);
    CHECK(sram.isDirty());

    sram.clearDirty();
    CHECK_FALSE(sram.isDirty());
}

TEST_CASE("範囲外アクセスで壊れない")
{
    x68k::Sram sram;
    // 16KB を超えるオフセットは無視される。ここで落ちるとエミュレータ全体が
    // 道連れになるので、黙って捨てる方が安全。
    sram.write8(x68k::kSramSize, 0xFF);
    sram.write8(x68k::kSramSize + 1000, 0xFF);
    CHECK(sram.read8(x68k::kSramSize) == 0);
    CHECK(sram.hasValidMagic());
}

TEST_CASE("マジックが壊れたら検出できる")
{
    x68k::Sram sram;
    sram.write8(0, 0x00);
    CHECK_FALSE(sram.hasValidMagic());

    // 初期化し直せば復旧する。
    sram.formatDefaults();
    CHECK(sram.hasValidMagic());
}

TEST_CASE("リセットしても SRAM の設定が残る")
{
    x68k::Machine machine;

    // Human68k が設定を書き換えた状況を作る。起動デバイスを FD 優先にし、
    // 画面モードも変える。
    machine.sram().write16(x68k::Sram::kOffsetBootDevice, 0x9070);
    machine.sram().write8(x68k::Sram::kOffsetScreenMode, 12);
    machine.sram().write8(0x200, 0x5A);

    machine.reset();

    // 実機の SRAM はバッテリバックアップなのでリセットで消えない。
    // ここが工場出荷値へ戻ると、設定を変えた直後にリセットしただけで
    // 起動デバイスや画面設定が失われる。
    CHECK(machine.sram().read16(x68k::Sram::kOffsetBootDevice) == 0x9070);
    CHECK(machine.sram().read8(x68k::Sram::kOffsetScreenMode) == 12);
    CHECK(machine.sram().read8(0x200) == 0x5A);
}

TEST_CASE("マジックが壊れていればリセットで初期化し直す")
{
    x68k::Machine machine;

    // 電池切れや壊れた sram.dat を模す。マジックを潰し、設定も汚しておく。
    machine.sram().write8(x68k::Sram::kOffsetMagic, 0x00);
    machine.sram().write16(x68k::Sram::kOffsetBootDevice, 0xFFFF);
    CHECK_FALSE(machine.sram().hasValidMagic());

    machine.reset();

    // 保持を優先してゴミのまま起動すると、IPL-ROM が不正な設定を読んで
    // 起動しない。マジック不正のときだけは工場出荷値へ戻す。
    CHECK(machine.sram().hasValidMagic());
    CHECK(machine.sram().read16(x68k::Sram::kOffsetBootDevice) == x68k::Sram::kBootDeviceSasi0);
}

TEST_CASE("保存イメージを読み込むと内容が置き換わる")
{
    auto image = makeValidImage();
    image[0x200] = 0x77;
    image[x68k::Sram::kOffsetScreenMode] = 12;

    x68k::Sram sram;
    CHECK(sram.loadImage(image.data(), image.size()));
    CHECK(sram.read8(0x200) == 0x77);
    CHECK(sram.read8(x68k::Sram::kOffsetScreenMode) == 12);
    CHECK(sram.hasValidMagic());

    // 読んだ直後はファイルと一致しているので、書き戻す必要は無い。
    CHECK_FALSE(sram.isDirty());
}

TEST_CASE("大きさの違う保存イメージは拒否される")
{
    const auto image = makeValidImage();
    x68k::Sram sram;
    sram.write8(0x200, 0x11);
    sram.clearDirty();

    // 途中で切れた sram.dat。0 埋めで受け入れると、起動デバイスだけ読めて
    // 画面設定はゼロという中途半端な状態になる。
    CHECK_FALSE(sram.loadImage(image.data(), image.size() - 1));
    CHECK_FALSE(sram.loadImage(image.data(), image.size() + 1));
    CHECK_FALSE(sram.loadImage(image.data(), 0));
    CHECK_FALSE(sram.loadImage(nullptr, image.size()));

    // 拒否したときは現在の内容を変えない。
    CHECK(sram.read8(0x200) == 0x11);
    CHECK_FALSE(sram.isDirty());
}

TEST_CASE("マジックの無い保存イメージは拒否される")
{
    auto image = makeValidImage();
    image[x68k::Sram::kOffsetMagic + 1] = 0x00;

    x68k::Sram sram;
    sram.write8(0x200, 0x11);
    CHECK_FALSE(sram.loadImage(image.data(), image.size()));
    CHECK(sram.read8(0x200) == 0x11);

    // 8 バイト目 (メモリチェック完了を示す $57) だけが違う場合も拒否する。
    auto tailBroken = makeValidImage();
    tailBroken[x68k::Sram::kOffsetMagic + 7] = 0x00;
    CHECK_FALSE(sram.loadImage(tailBroken.data(), tailBroken.size()));
}

TEST_CASE("保存の間引きに使う dirty が変更でだけ立つ")
{
    x68k::Sram sram;

    // 構築直後は工場出荷状態そのもの。保存する理由が無い。
    CHECK_FALSE(sram.isDirty());

    // 保存した (= clearDirty した) 後に変更が無ければ、次の周期でも
    // 書かない。ここが常に true だと、10 秒ごとに 16KB を書き続けて
    // SD の寿命を削る。
    sram.write16(x68k::Sram::kOffsetBootDevice, 0x9070);
    CHECK(sram.isDirty());
    sram.clearDirty();
    CHECK_FALSE(sram.isDirty());

    // 読むだけでは立たない。
    (void)sram.read8(x68k::Sram::kOffsetBootDevice);
    (void)sram.read16(x68k::Sram::kOffsetBootDevice);
    (void)sram.hasValidMagic();
    CHECK_FALSE(sram.isDirty());

    // 範囲外の書き込みは何も変えないので立たない。
    sram.write8(x68k::kSramSize, 0xFF);
    CHECK_FALSE(sram.isDirty());

    // 次の変更でまた立つ。
    sram.write8(x68k::Sram::kOffsetScreenMode, 12);
    CHECK(sram.isDirty());
}
