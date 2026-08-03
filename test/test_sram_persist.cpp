// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: sram.dat の保存と復元が、どのタイミングで電源が落ちても
// 利用者の設定を失わないこと。
//
// 保存は「.tmp へ書き切る → 本体を消す → .tmp を本体へ改名する」の順に進む。
// FATFS の rename は既存の宛先があると失敗するので、本体を先に消すしかない。
// その remove と rename の隙間で電源が落ちると、本体が消えて完全な .tmp だけが
// 残る。ここを復元側が拾えないと、原子的に書いた意味が無くなって工場出荷値へ
// 落ちる (= 設定が全部消える)。実機の FATFS では電源断のタイミングを再現
// できないので、疑似ファイルシステムに失敗を注入して確かめる。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dev/sram.h"
#include "doctest.h"
#include "sram_persist.h"

namespace
{

// メモリ上の疑似ファイルシステム。任意の操作で失敗させられる。
class FakeFileOps final : public x68k_platform::SramFileOps
{
public:
    std::map<std::string, std::vector<std::uint8_t>> files;

    // .tmp への書き込みを失敗させる (SD の I/O エラー)。
    bool failWrite = false;
    // rename だけを失敗させる。remove は通るので、本体が消えて .tmp だけが
    // 残る状態 (= 例の隙間) をそのまま作れる。
    bool failRename = false;

    std::size_t read(const char* path, std::uint8_t* buffer, std::size_t bufferSize) override
    {
        const auto it = files.find(path);
        if (it == files.end())
        {
            return 0;
        }
        // 実装 (loadFile) と同じく、buffer に収まらない場合は部分読み込みを
        // 許さず 0 を返す。
        if (it->second.empty() || it->second.size() > bufferSize)
        {
            return 0;
        }
        for (std::size_t i = 0; i < it->second.size(); ++i)
        {
            buffer[i] = it->second[i];
        }
        return it->second.size();
    }

    bool write(const char* path, const std::uint8_t* buffer, std::size_t size) override
    {
        if (failWrite)
        {
            // 半端なファイルを残さないのが契約。
            files.erase(path);
            return false;
        }
        files[path] = std::vector<std::uint8_t>(buffer, buffer + size);
        return true;
    }

    bool remove(const char* path) override
    {
        files.erase(path);
        return true;
    }

    bool rename(const char* from, const char* to) override
    {
        if (failRename)
        {
            return false;
        }
        const auto it = files.find(from);
        if (it == files.end())
        {
            return false;
        }
        // FAT の rename は既存の宛先があると失敗する。方針側がその前提で
        // 本体を先に消しているかを、ここで縛って検査する。
        if (files.count(to) != 0)
        {
            return false;
        }
        files[to] = it->second;
        files.erase(it);
        return true;
    }

    [[nodiscard]] bool exists(const char* path) override
    {
        return files.count(path) != 0;
    }
};

constexpr const char* kPath = "/sd/x68k/sram.dat";
constexpr const char* kTempPath = "/sd/x68k/sram.dat.tmp";

// 正しいマジックを持つ 16KB のイメージ。marker で世代を区別する。
std::vector<std::uint8_t> makeImage(std::uint8_t marker)
{
    x68k::Sram sram;
    sram.write8(0x200, marker);
    return std::vector<std::uint8_t>(sram.data(), sram.data() + x68k::kSramSize);
}

// 復元先。scratch は実装と同じく kSramSize ちょうどを渡す。
struct Loader
{
    x68k::Sram sram;
    std::uint8_t scratch[x68k::kSramSize]{};

    x68k_platform::SramLoadResult load(FakeFileOps& ops)
    {
        return x68k_platform::loadSramImage(ops, kPath, sram, scratch, sizeof(scratch));
    }
};

}  // namespace

TEST_CASE("本体が正しければそこから復元する")
{
    FakeFileOps ops;
    ops.files[kPath] = makeImage(0x11);

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kPrimary);
    CHECK(loader.sram.read8(0x200) == 0x11);
    // 読んだ直後はファイルと一致しているので書き戻す必要は無い。
    CHECK_FALSE(loader.sram.isDirty());
}

TEST_CASE("本体が無く .tmp が正しければ復旧して昇格させる")
{
    // remove と rename の隙間で電源が落ちた状態。本体は消え、完全な .tmp だけが
    // 残っている。ここを拾えないと、原子的に書いた意味が無くなる。
    FakeFileOps ops;
    ops.files[kTempPath] = makeImage(0x22);

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kRecovered);
    CHECK(loader.sram.read8(0x200) == 0x22);

    // 昇格していること。次回は本体から普通に読める。
    CHECK(ops.exists(kPath));
    CHECK_FALSE(ops.exists(kTempPath));
    CHECK(ops.files[kPath] == makeImage(0x22));
}

TEST_CASE("本体が壊れていても .tmp が正しければ復旧する")
{
    FakeFileOps ops;
    // マジックを潰した本体。Sram::loadImage に拒否される。
    auto broken = makeImage(0x33);
    broken[x68k::Sram::kOffsetMagic + 1] = 0x00;
    ops.files[kPath] = broken;
    ops.files[kTempPath] = makeImage(0x44);

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kRecovered);
    CHECK(loader.sram.read8(0x200) == 0x44);
    CHECK(ops.files[kPath] == makeImage(0x44));
    CHECK_FALSE(ops.exists(kTempPath));
}

TEST_CASE("本体と .tmp が両方あれば本体を採る")
{
    // 保存は remove(本体) → rename(.tmp) の順なので、「本体があるのに .tmp の方が
    // 新しい」状態は作れない。両方あるなら .tmp は書き損じか古い世代。
    // ここが .tmp 優先だと、書き込みに一度失敗しただけで設定が巻き戻る。
    FakeFileOps ops;
    ops.files[kPath] = makeImage(0xAA);
    ops.files[kTempPath] = makeImage(0xBB);

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kPrimary);
    CHECK(loader.sram.read8(0x200) == 0xAA);

    // 用済みの .tmp は消しておく。残すと、次に保存が失敗して本体が消えた
    // 瞬間に古い世代が昇格してしまう。
    CHECK_FALSE(ops.exists(kTempPath));
    CHECK(ops.files[kPath] == makeImage(0xAA));
}

TEST_CASE("どちらも無ければ復元しない")
{
    // 初回起動。異常ではなく、工場出荷値のまま進んでよい。
    FakeFileOps ops;

    Loader loader;
    const std::uint8_t before = loader.sram.read8(0x200);
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kNone);
    CHECK(loader.sram.read8(0x200) == before);
    CHECK(loader.sram.hasValidMagic());
}

TEST_CASE("長さの足りない .tmp は昇格させない")
{
    // .tmp を書いている途中で電源が落ちた場合。中身は本物のイメージの前半だが、
    // これを受け入れると「起動デバイスだけ読めて画面設定はゼロ」になる。
    FakeFileOps ops;
    auto partial = makeImage(0x55);
    partial.resize(x68k::kSramSize / 2);
    ops.files[kTempPath] = partial;

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kNone);
    CHECK(loader.sram.hasValidMagic());
    // 昇格していないこと。半端な内容が本体の名前を得るのが最悪。
    CHECK_FALSE(ops.exists(kPath));
}

TEST_CASE("マジックの無い .tmp は昇格させない")
{
    FakeFileOps ops;
    auto noMagic = makeImage(0x66);
    noMagic[x68k::Sram::kOffsetMagic + 7] = 0x00;  // $57 だけ違う場合も拒否する
    ops.files[kTempPath] = noMagic;

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kNone);
    CHECK_FALSE(ops.exists(kPath));
    // 使えない .tmp は残さない。残しても次回また候補に挙がるだけ。
    CHECK_FALSE(ops.exists(kTempPath));
}

TEST_CASE("本体が読めたときに古い .tmp を掃除する")
{
    // 掃除しないと、本体が正しく書けた後も古い .tmp が居座る。本体優先なので
    // 普段は害が無いが、保存が失敗して本体が消えた瞬間に何世代も前の内容が
    // 昇格し、「復元されたのに設定が巻き戻る」という追いにくい壊れ方になる。
    FakeFileOps ops;
    ops.files[kPath] = makeImage(0x77);
    ops.files[kTempPath] = makeImage(0x88);

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kPrimary);
    CHECK_FALSE(ops.exists(kTempPath));

    // 掃除した後に本体が消えても、古い世代は昇格しない。
    ops.files.erase(kPath);
    Loader second;
    CHECK(second.load(ops) == x68k_platform::SramLoadResult::kNone);
}

TEST_CASE("保存は .tmp へ書いてから本体へ改名する")
{
    FakeFileOps ops;
    const auto image = makeImage(0x99);

    CHECK(x68k_platform::saveSramImage(ops, kPath, image.data(), image.size()));
    CHECK(ops.files[kPath] == image);
    // 一時ファイルを残さない。
    CHECK_FALSE(ops.exists(kTempPath));
}

TEST_CASE("保存は既存の本体を上書きできる")
{
    // FAT の rename は宛先があると失敗する。先に本体を消していないと、
    // 二回目以降の保存が永久に通らない。
    FakeFileOps ops;
    ops.files[kPath] = makeImage(0x01);

    const auto next = makeImage(0x02);
    CHECK(x68k_platform::saveSramImage(ops, kPath, next.data(), next.size()));
    CHECK(ops.files[kPath] == next);
}

TEST_CASE("書き込みに失敗しても本体は壊れない")
{
    // SD の一時的な I/O エラー。.tmp へ書いている段階で失敗するので、
    // 本体にはまだ手を付けていない。前回の設定がそのまま残ること。
    FakeFileOps ops;
    ops.files[kPath] = makeImage(0x03);
    ops.failWrite = true;

    const auto next = makeImage(0x04);
    CHECK_FALSE(x68k_platform::saveSramImage(ops, kPath, next.data(), next.size()));
    CHECK(ops.files[kPath] == makeImage(0x03));
    CHECK_FALSE(ops.exists(kTempPath));

    // 失敗した後でも、本体から普通に復元できる。
    ops.failWrite = false;
    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kPrimary);
    CHECK(loader.sram.read8(0x200) == 0x03);
}

TEST_CASE("改名の直前で落ちても次回起動で設定が残る")
{
    // 本丸。remove は通って rename が通らなかった = 電源断の隙間そのもの。
    // 保存としては失敗だが、内容は .tmp に完全な形で残っていなければならない。
    FakeFileOps ops;
    ops.files[kPath] = makeImage(0x05);
    ops.failRename = true;

    const auto next = makeImage(0x06);
    CHECK_FALSE(x68k_platform::saveSramImage(ops, kPath, next.data(), next.size()));

    // 本体は消えている。ここで .tmp まで消していたら設定は本当に失われる。
    CHECK_FALSE(ops.exists(kPath));
    CHECK(ops.files[kTempPath] == next);

    // 次回起動。工場出荷値ではなく、保存しようとしていた新しい内容が戻ること。
    ops.failRename = false;
    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kRecovered);
    CHECK(loader.sram.read8(0x200) == 0x06);
    CHECK(ops.files[kPath] == next);
    CHECK_FALSE(ops.exists(kTempPath));
}

TEST_CASE("昇格できなくても復元した内容は保たれる")
{
    // 復旧はできたが、.tmp を本体へ改名する段で失敗した場合。SRAM には正しい
    // 内容が載っているので、この回の起動は設定を保てている。ここで失敗扱いに
    // して工場出荷値へ落とす方が損失が大きい。
    FakeFileOps ops;
    ops.files[kTempPath] = makeImage(0x07);
    ops.failRename = true;

    Loader loader;
    CHECK(loader.load(ops) == x68k_platform::SramLoadResult::kRecovered);
    CHECK(loader.sram.read8(0x200) == 0x07);

    // .tmp は消さない。消すと本当に内容が無くなる。次回も同じ経路で拾える。
    CHECK(ops.files[kTempPath] == makeImage(0x07));

    ops.failRename = false;
    Loader second;
    CHECK(second.load(ops) == x68k_platform::SramLoadResult::kRecovered);
    CHECK(second.sram.read8(0x200) == 0x07);
}
