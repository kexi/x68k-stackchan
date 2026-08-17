// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// microSD から ROM とディスクイメージを読む。
//
// X68000 の IPL-ROM / CGROM / Human68k はライセンス上リポジトリに同梱できず
// (NOTICE.md 参照)、利用者が自分で用意する。SD に置く方式にしたのは、
// カードリーダーで直接コピーできて差し替えも楽なため。
//
// SD 上の配置:
//   /x68k/iplrom.dat   IPL-ROM (128KB, 必須)
//   /x68k/cgrom.dat    CGROM (768KB, 任意)
//   /x68k/hdd0.hdf     SASI HDD イメージ (必須)
//   /x68k/fd0.xdf      FDD0 のフロッピーイメージ (任意)
//   /x68k/fd1.xdf      FDD1 のフロッピーイメージ (任意)
//   /x68k/sram.dat     SRAM の保存先 (自動生成)

#ifndef X68K_PLATFORM_STORAGE_SD_H
#define X68K_PLATFORM_STORAGE_SD_H

#include <cstddef>
#include <cstdint>

#include "machine.h"

namespace x68k_platform
{

#if X68K_MEASURE_DISK
// ディスク読み出しの実時間 (storage_sd.cpp)。恒久的な機能ではない。
// 「実効クロックの落ち込みがディスク由来か」を推定でなく実測で確かめる。
extern std::int64_t g_diskReadUs;
extern std::uint32_t g_diskReadCount;
extern std::int64_t g_diskSeekUs;
#endif

// SD 上のパス。
inline constexpr const char* kSdMountPoint = "/sd";
inline constexpr const char* kIplromPath = "/sd/x68k/iplrom.dat";
inline constexpr const char* kCgromPath = "/sd/x68k/cgrom.dat";
inline constexpr const char* kHddPath = "/sd/x68k/hdd0.hdf";
inline constexpr const char* kSramPath = "/sd/x68k/sram.dat";
// フロッピー。ドライブごとに 1 本。無ければ「ディスクが入っていない」。
inline constexpr const char* kFd0Path = "/sd/x68k/fd0.xdf";
inline constexpr const char* kFd1Path = "/sd/x68k/fd1.xdf";

// SD をマウントする。成功したら true。
bool mountSd();

// ファイル全体を buffer へ読み込む。
//
// 戻り値は実際に読めたバイト数。ファイルが無い、または buffer より大きい場合は 0。
// 部分読み込みを許さないのは、ROM が途中までしか読めていない状態で起動すると
// 原因の分からない暴走になるため。
std::size_t loadFile(const char* path, std::uint8_t* buffer, std::size_t bufferSize);

// buffer の内容を path へ丸ごと書き出す。成功したら true。
//
// Why not 差分だけ書くか: SRAM は 16KB しかなく、FAT のクラスタ単位で見れば
// 数クラスタでしかない。どこが変わったかを追う仕組みを持つ方が、書き込み量の
// 削減より確実に高く付く。書き込み回数の間引きは呼び出し側 (dirty) で行う。
bool saveFile(const char* path, const std::uint8_t* buffer, std::size_t size);

// SD の sram.dat から SRAM を復元する。取り込めたら true。
//
// 大きさが kSramSize ちょうどで、マジックが正しいときだけ受け入れる。
// 拒否した場合 machine の SRAM は触らない (呼び出し前の内容が残る)。
// ファイルが無い初回起動も false になるが、これは異常ではない。
bool loadSram(x68k::Machine& machine);

// SRAM に変更があれば SD へ書き戻す。書いたら true。
//
// 変更が無ければ何もしない。SD の書き込み寿命を守るため、呼び出し側は
// さらに時間で間引くこと (毎スライス呼んでも Sram::isDirty() で止まるが、
// 1 バイト書かれるたびに 16KB を書くのは避けたい)。
bool saveSramIfDirty(x68k::Machine& machine);

// SD 上の HDD イメージをセクタ単位で読むディスク。
//
// イメージ全体をメモリに載せない (数十 MB あり PSRAM に収まらないため)。
// 読み出しのたびに SD へアクセスするが、SASI は 256 バイト/セクタなので
// 1 回のアクセスは小さい。
class SdDisk final : public x68k::DiskImage
{
public:
    ~SdDisk() override;

    // イメージを開く。成功したら true。
    bool open(const char* path);
    void close();

    bool readSector(x68k::u32 lba, x68k::u8* buffer, x68k::u32 sectorCount) override;
    bool writeSector(x68k::u32 lba, const x68k::u8* buffer, x68k::u32 sectorCount) override;
    [[nodiscard]] bool isPresent() const override;

private:
    // SASI のセクタ長。
    static constexpr x68k::u32 kSectorSize = 256;

    void* file_ = nullptr;  // FILE*。ヘッダに <cstdio> を持ち込まないため void*
};

// SD 上のフロッピーイメージを CHS でアクセスするドライブ。
//
// SdDisk と同じく、イメージ全体をメモリに載せない。2HD でも 1.2MB あり、
// PSRAM に置けなくはないが、起動のたびに 1.2MB を SD から読むのは遅い。
// 1 セクタ (1024 バイト) ずつ読む方が体感で速い。
//
// 受け付ける形式は XDF (ヘッダ無しのセクタダンプ) と DIM (256 バイトの
// ヘッダ付き)。どちらもファイル長からジオメトリを引く。拡張子は見ない。
class SdFloppy final : public x68k::FloppyImage
{
public:
    ~SdFloppy() override;

    // イメージを開く。長さが未対応なら false を返して開かない。
    //
    // Why not 未対応でも開くか: 切り上げて「とりあえず 2HD」として扱うと、
    // 末尾のトラックが読めないまま「ディスクはある」と見える。Human68k は
    // FAT を読んだ時点でおかしくなるが、そこまで症状が離れると原因に
    // 辿り着けない。
    bool open(const char* path);
    void close();

    bool readSector(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record,
                    x68k::u8* buffer) override;
    bool writeSector(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record,
                     const x68k::u8* buffer) override;
    [[nodiscard]] bool isPresent() const override;
    [[nodiscard]] bool isWriteProtected() const override;
    [[nodiscard]] const x68k::FloppyGeometry& geometry() const override;

private:
    // CHS からファイル内のオフセットを引く。R は 1 起点。
    bool locate(x68k::u32 cylinder, x68k::u32 head, x68k::u32 record, long* offset) const;

    void* file_ = nullptr;  // FILE*
    x68k::FloppyGeometry geometry_{};
    // DIM のヘッダぶん。XDF なら 0。
    x68k::u32 dataOffset_ = 0;
    // 書き込み用に開けなかったら書き込み禁止として扱う。
    bool writeProtected_ = false;
};

}  // namespace x68k_platform

#endif  // X68K_PLATFORM_STORAGE_SD_H
