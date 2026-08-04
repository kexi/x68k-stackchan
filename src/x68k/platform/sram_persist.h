// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// sram.dat の保存と復元の「方針」だけを取り出したもの。
//
// storage_sd.cpp は FATFS を直に叩くので、電源断のタイミングを再現できず
// ホストのテストに載らない。載らないまま置くには、この部分は失敗の仕方が
// 効きすぎる (壊れ方が「利用者の設定が全部消える」なので)。
// そこでファイル操作を SramFileOps 越しにして、方針をここへ分離する。
//
// Why not core/ に置くか: core/ は X68000 というハードウェアの模倣であって、
// SD に何を置くかは実装側の都合でしかない。同じ core を別の保存先 (NVS や
// ホストの普通のファイル) で使うときに、ここの規約が付いて回るのはおかしい。
// ただしホストのテストからは見えている必要があるので、ESP-IDF に触らない
// ヘッダオンリーにして platform/ に置く。core-guard の対象は core/ だけ。

#ifndef X68K_PLATFORM_SRAM_PERSIST_H
#define X68K_PLATFORM_SRAM_PERSIST_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "dev/sram.h"

namespace x68k_platform
{

// 一時ファイルの拡張子。保存も復元もこの規約を共有する。
inline constexpr const char* kTempSuffix = ".tmp";

inline std::string tempPathOf(const char* path)
{
    std::string temp(path);
    temp += kTempSuffix;
    return temp;
}

// 方針が必要とするファイル操作。実機は FATFS、テストは疑似ファイルシステム。
//
// Why not std::filesystem を使うか: C++17 の <filesystem> は ESP-IDF の
// FATFS 上では実装が無いに等しく、そもそも「rename が既存の宛先を潰せない」
// という FAT 固有の制約を隠してくれない。隠せない以上、抽象化しても
// 方針側が FAT を意識することになる。ならば必要な操作だけを並べた方が短い。
class SramFileOps
{
public:
    virtual ~SramFileOps() = default;

    // ファイル全体を buffer へ読む。読めたバイト数を返す。
    // ファイルが無い、または bufferSize を超える場合は 0。
    virtual std::size_t read(const char* path, std::uint8_t* buffer, std::size_t bufferSize) = 0;

    // buffer を path へ書き切る。書けたら true。
    // 途中で失敗した場合、path は消しておく (半端なファイルを残さない)。
    virtual bool write(const char* path, const std::uint8_t* buffer, std::size_t size) = 0;

    // path を消す。元から無い場合も true でよい (呼び出し側は区別しない)。
    virtual bool remove(const char* path) = 0;

    // from を to へ改名する。FAT に合わせ、to が存在する場合は失敗してよい。
    virtual bool rename(const char* from, const char* to) = 0;

    // path が存在するか。
    [[nodiscard]] virtual bool exists(const char* path) = 0;
};

// 復元がどの経路で成立したか。ログとテストのための区別。
enum class SramLoadResult
{
    kNone,       // 復元できなかった (初回起動、または両方が壊れている)
    kPrimary,    // 本体 (sram.dat) から復元した
    kRecovered,  // .tmp から復元し、本体へ昇格させた
};

// sram.dat を書き換える。成功したら true。
//
// いったん .tmp へ書き切ってから rename で置き換える。fopen("wb") で本体を
// 直接開くと、その時点で既存の sram.dat が長さ 0 に切り詰められ、書いている
// 途中の電源断で「保存しようとしたせいで、それまでの設定まで失う」ことになる。
//
// FATFS の rename は既存の宛先があると失敗するので、先に本体を消す。
// この remove と rename の間で電源が落ちると本体は消えて .tmp だけが残るが、
// その .tmp は完全な内容なので loadSramImage() が拾って昇格させる。
inline bool saveSramImage(SramFileOps& ops, const char* path, const std::uint8_t* image,
                          std::size_t size)
{
    const std::string temp = tempPathOf(path);

    if (!ops.write(temp.c_str(), image, size))
    {
        // write() が半端な .tmp を残さない契約なので、ここでの後始末は要らない。
        return false;
    }

    ops.remove(path);
    if (!ops.rename(temp.c_str(), path))
    {
        // 本体は既に消えている。ここで .tmp まで消すと設定が本当に失われるので
        // 残す。次回起動の復元で拾える。
        //
        // Why not 消して「保存前の状態」に戻すか: 保存前の状態はもう無い。
        // remove を通った時点で本体は消えており、戻せる先が存在しない。
        return false;
    }
    return true;
}

// sram.dat から SRAM を復元する。
//
// 【並び順】本体を先に見て、駄目なときだけ .tmp を見る。
//
// Why not 新しい方 (mtime) を採るか: 両方が揃っている状況では、.tmp は必ず
// 本体より新しくない。saveSramImage() は remove(本体) → rename(.tmp) の順に
// 進むので、
//   - 書き込み中/失敗で残った .tmp   → 本体は無傷で残っている (両方ある)
//   - remove と rename の間で落ちた  → 本体は消えている (.tmp しかない)
// の二つしか作れない。つまり「本体があるのに .tmp の方が新しい」窓は
// 構造上存在しない。両方あるなら .tmp は古いか書き損じのどちらかで、
// 本体を優先すれば正しい。
//
// Why not それでも mtime で決めないか: FAT のタイムスタンプは当てにならない。
// CoreS3 には電池で持つ時計が無く、SNTP が通らなければ FATFS は 1980-01-01 を
// 書く。両方が同じ時刻になってしまえば比較の意味が無く、時計が途中で合うと
// 古い方が新しく見える逆転すら起きる。順序が構造から決まるのに、当てにならない
// 情報を持ち込む理由が無い。
//
// 採用した .tmp は本体へ昇格させ、残った .tmp は必ず消す。
//
// Why not .tmp を残しておかないか: 残すと次に本体を正しく書けた後も古い .tmp が
// 居座る。上の並び順なら本体が勝つので実害は出ないが、保存が一度失敗して本体が
// 消えた瞬間に、何世代も前の .tmp が「完全な内容」として昇格してしまう。
// 復元されたのに設定が巻き戻る、という最も分かりにくい壊れ方になる。
inline SramLoadResult loadSramImage(SramFileOps& ops, const char* path, x68k::Sram& sram,
                                    std::uint8_t* scratch, std::size_t scratchSize)
{
    const std::string temp = tempPathOf(path);

    // 検査は core 側 (Sram::loadImage) に任せる。ちょうど kSramSize バイトで
    // マジックが正しいときだけ受け入れられ、拒否されても SRAM は変わらない。
    // 本体と .tmp で同じ関数を通すので、半端な .tmp を昇格させることはない。
    const std::size_t primarySize = ops.read(path, scratch, scratchSize);
    const bool isPrimaryAccepted = primarySize != 0 && sram.loadImage(scratch, primarySize);
    if (isPrimaryAccepted)
    {
        // 本体が生きているなら .tmp は用済み。次の失敗で昇格しないよう消す。
        ops.remove(temp.c_str());
        return SramLoadResult::kPrimary;
    }

    const std::size_t tempSize = ops.read(temp.c_str(), scratch, scratchSize);
    const bool isTempAccepted = tempSize != 0 && sram.loadImage(scratch, tempSize);
    if (!isTempAccepted)
    {
        // どちらも使えない。壊れた .tmp は残さない (次回また候補に挙がるだけ)。
        ops.remove(temp.c_str());
        return SramLoadResult::kNone;
    }

    // 内容は既に SRAM へ載っている。あとは名前を本体へ寄せるだけ。
    //
    // Why not 昇格に失敗したら復元も失敗扱いにするか: SRAM には正しい内容が
    // 載っており、この回の起動は設定を保てている。昇格できなくても .tmp は
    // そのまま残るので次回も同じ経路で拾える。ここで false を返して工場出荷値へ
    // 落とす方が損失が大きい。
    ops.remove(path);
    if (!ops.rename(temp.c_str(), path))
    {
        // 昇格できなくても .tmp は消さない。消すと本当に内容が無くなる。
    }
    return SramLoadResult::kRecovered;
}

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_SRAM_PERSIST_H
