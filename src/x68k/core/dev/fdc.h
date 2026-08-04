// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// FDC (NEC uPD72065) ($E94000)。
//
// IPL-ROM は起動デバイスの設定に関わらず FDC を初期化しに来る。その
// ポーリングループ (CB 待ち / RQM 待ち / 結果待ち) はいずれもタイムアウトを
// 持たないため、正しく応答しないとそこで永久に止まる。
//
// 実装範囲: コマンド・実行・結果の 3 フェーズを持つ状態機械と、
// イメージを繋いだときの実データ転送。
//   READ DATA / WRITE DATA (マルチセクタ、DMAC のターミナルカウントまで継続)
//   READ ID / SEEK / RECALIBRATE / SENSE INTERRUPT STATUS /
//   SENSE DRIVE STATUS / SPECIFY
//
// データ転送は DMAC (HD63450) のチャネル 0 経由で行う。IPL-ROM の
// $FF8F3C が $E84005 (ch0 OCR) に $B2、$E8400C (ch0 MAR)、$E8400A (ch0 MTC)、
// $E84007 (ch0 CCR) に $80 を書いて起動し、$FF9014 が $E84000 (ch0 CSR) の
// bit4 (ERR) を見る。SASI が使うチャネル 1 とは別チャネルである点に注意。
//
// 割り込み線は配線していない。実機の FDC は IRQ レベル 1 のオートベクタで
// (MFP は通さない)、IPL-ROM の割り込みハンドラ ($FF1130) が結果フェーズを
// 読み出してドライブごとの状態表 $C90 に積む。本エミュレータにはその経路が
// 無いので、結果を積むコマンドは IPL-ROM がその場で読むもの
// (SENSE DRIVE STATUS / SENSE INTERRUPT STATUS) に限る。DMA を使う
// READ/WRITE DATA は結果を持たずコマンド待ちへ戻す。積んだまま残すと
// メインステータスの CB が落ちず、次のコマンド送出 ($FF9036) がそこで
// 永久に止まる。この制約はイメージを繋いでも変わらない。

#ifndef X68K_CORE_DEV_FDC_H
#define X68K_CORE_DEV_FDC_H

#include <array>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

// フロッピーのジオメトリ。X68000 標準の 2HD は
// 77 シリンダ x 2 ヘッド x 8 セクタ x 1024 バイト = 1,261,568 バイト。
//
// Why not SASI と同じ DiskImage を使い回すか: SASI は 256 バイト固定の LBA
// 空間だが、FD は CHS でアクセスされ、セクタ長も N (0=128 / 1=256 / 2=512 /
// 3=1024) で毎コマンド変わる。同じ口に押し込むと「どちらのセクタ長で数えた
// LBA か」が呼び出し側にしか分からなくなり、読み出し位置を静かに間違える。
struct FloppyGeometry
{
    u32 cylinders = 77;
    u32 heads = 2;
    u32 sectorsPerTrack = 8;
    u32 sectorSize = 1024;

    [[nodiscard]] u32 totalBytes() const
    {
        return cylinders * heads * sectorsPerTrack * sectorSize;
    }
};

// 受け付けるイメージ形式。
//
//   XDF  ヘッダの無いセクタダンプ。ファイル長がそのままイメージ長。
//        2HD (1,261,568) / 2DD 8 セクタ (655,360) / 2DD 9 セクタ (737,280)。
//   DIM  先頭 256 バイトのヘッダが付く。ヘッダを飛ばした残りが XDF と同じ
//        セクタダンプ。ヘッダ先頭 1 バイトがメディア種別で、$00 が 2HD。
//
// Why not ヘッダの中身を信じるか: DIM のヘッダはツールによって埋め方が
// まちまちで、トラック使用フラグを立てていないイメージが実在する。
// 「ヘッダを飛ばした残りの長さ」からジオメトリを引く方が、実際に
// 手元にあるイメージで確実に当たる。
enum class FloppyFormat
{
    Unknown,
    Xdf,  // 生セクタダンプ (ヘッダ無し)
    Dim,  // 256 バイトヘッダ付き
};

// DIM のヘッダ長。
inline constexpr u32 kDimHeaderBytes = 256;

// イメージ全体のバイト数からジオメトリを引く。
//
// 戻り値が false なら対応していない大きさ。呼び出し側は読み込みを
// 拒否すること。切り上げて受け入れると、末尾のトラックが読めないまま
// 「ディスクはある」と見えてしまう。
bool floppyGeometryFromSize(u32 imageBytes, FloppyGeometry* out);

// ファイル長から形式を判定する。
//
// 純粋に長さで見る。DIM は「対応するどの生イメージ長にも一致しないが、
// 256 を引くと一致する」ときに限って認める。
FloppyFormat detectFloppyFormat(u32 fileBytes, u32* dataOffset, FloppyGeometry* geometry);

// フロッピーイメージを外から与えるための口。
// ホストでは通常のファイル、実機では microSD が実装する。
//
// セクタ番号 R は 1 起点 (uPD72065 / IBM フォーマットの慣習)。0 起点で
// 渡すと 1 トラック分ずれる。実装側は CHS → イメージ内オフセットの換算に
// geometry() を使う。
class FloppyImage
{
public:
    virtual ~FloppyImage() = default;

    // CHS で 1 セクタ読む。sectorSize バイトを buffer へ。成功したら true。
    // record は 1 起点のセクタ番号。
    virtual bool readSector(u32 cylinder, u32 head, u32 record, u8* buffer) = 0;
    virtual bool writeSector(u32 cylinder, u32 head, u32 record, const u8* buffer) = 0;

    [[nodiscard]] virtual bool isPresent() const = 0;
    // 書き込み禁止 (ライトプロテクト) か。
    [[nodiscard]] virtual bool isWriteProtected() const = 0;
    [[nodiscard]] virtual const FloppyGeometry& geometry() const = 0;
};

class Fdc
{
public:
    // 対応するドライブ数。X68000 は内蔵 2 台 (FDD0/FDD1)。
    static constexpr u32 kDriveCount = 2;

    // 1 セクタの最大長。N=3 (1024 バイト) までを扱う。
    //
    // Why not N=7 (16KB) まで持つか: uPD72065 は N を 8bit で受けるが、
    // X68000 の 2HD/2DD で実際に使われるのは N<=3 まで。バッファは
    // Fdc に埋め込む (実機の内部 SRAM に載る大きさに抑える必要がある)
    // ので、使われない大きさのために 16KB を常駐させたくない。
    static constexpr u32 kMaxSectorSize = 1024;

    // メインステータスレジスタ ($E94001) のビット。
    static constexpr u8 kStatusRqm = 0x80;     // データ転送要求
    static constexpr u8 kStatusDio = 0x40;     // 転送方向 (1 = FDC → CPU)
    static constexpr u8 kStatusNdm = 0x20;     // 非 DMA モード
    static constexpr u8 kStatusCb = 0x10;      // コマンド実行中
    static constexpr u8 kStatusDrive0 = 0x01;  // ドライブ 0 がシーク中

    void reset();

    // ドライブ n にイメージを繋ぐ。nullptr で「ディスクが入っていない」。
    void setImage(u32 drive, FloppyImage* image);
    [[nodiscard]] FloppyImage* image(u32 drive) const;

    // $E94001 (メインステータス) を読む。
    [[nodiscard]] u8 readStatus() const;

    // $E94003 (データレジスタ) を読む。結果フェーズで結果バイトを返す。
    u8 readData();

    // $E94003 へ書く。コマンドとそのパラメータ。
    void writeData(u8 value);

    // $E94005 (ドライブ制御) へ書く。モーター制御など。
    void writeDriveControl(u8 value);

    // $E94007 (ドライブ選択) へ書く。IPL-ROM の $FF909E が
    // 「$80 | ドライブ番号」を書く。
    void writeDriveSelect(u8 value);

    // 割り込みが上がっているか。IPL-ROM は SENSE INTERRUPT STATUS で
    // これを確認する。
    [[nodiscard]] bool hasInterrupt() const
    {
        return interruptPending_;
    }

    // --- DMA ---
    //
    // DMAC のチャネル 0 がここからバイトを取り/置きする。SASI と同じく
    // 「1 バイトずつ差し出す」形にしてあるので、DMAC 側は転送元がどの
    // デバイスかを知らなくてよい。
    //
    // 実行フェーズの外で呼ばれたら false を返す。DMAC はそこで転送を
    // 打ち切り、CSR に ERR を立てる ($FF9014 がそれを見る)。

    // FDC → メモリ (READ DATA)。
    bool dmaRead(u8* value);
    // メモリ → FDC (WRITE DATA)。
    bool dmaWrite(u8 value);

    // DMAC が転送を終えた (ターミナルカウント到達、または打ち切り)。
    //
    // FDC は自分では転送長を知らない。READ/WRITE DATA は「EOT に達するか
    // DMAC が止めるまで」続くので、DMAC が呼ぶのをやめただけでは実行
    // フェーズを畳めない。ここで畳まないとメインステータスの CB が
    // 立ったままになり、次のコマンド送出 ($FF9036) が永久に止まる。
    void dmaComplete(bool isComplete);

    // 実行フェーズ (DMA 転送の途中) か。
    [[nodiscard]] bool isExecuting() const
    {
        return phase_ == Phase::Execute;
    }

private:
    enum class Phase
    {
        Command,  // コマンドとパラメータを受け取る
        Execute,  // DMA でデータを流している最中
        Result,   // 結果バイトを返す
    };

    // 実行フェーズの転送方向。
    enum class Transfer
    {
        None,
        Read,   // FDC → メモリ
        Write,  // メモリ → FDC
    };

    // コマンドに続くパラメータのバイト数を返す。
    [[nodiscard]] static u32 parameterCount(u8 commandByte);

    // コマンドが揃ったときに呼ばれる。結果バイトを組み立てる。
    void executeCommand();

    // READ/WRITE DATA の入口。実行フェーズへ入る (または即エラー終了)。
    void beginReadWrite(bool isWrite);

    // 現在の C/H/R から 1 セクタぶんをバッファへ読む。成功したら true。
    bool loadCurrentSector();
    // バッファの内容を現在の C/H/R へ書き戻す。成功したら true。
    bool storeCurrentSector();

    // 1 セクタ流し終えたので次のセクタへ進む。継続できたら true。
    // EOT を超えたら MT に応じてヘッドを替え、それも尽きたら false。
    bool advanceSector();

    // 実行フェーズを終える。結果は積まない (積むと CB が落ちない)。
    void finishExecute();

    // 現在選択されているドライブのイメージ。無ければ nullptr。
    [[nodiscard]] FloppyImage* currentImage() const;

    // ST3 (SENSE DRIVE STATUS が返す) を組み立てる。
    [[nodiscard]] u8 buildSt3(u32 drive) const;

    // ST0 の下位 (ヘッド + ドライブ番号) を組み立てる。
    [[nodiscard]] u8 unitSelect() const;

    Phase phase_ = Phase::Command;

    // 受け取ったコマンドとパラメータ。最長は WRITE ID の 9 バイト。
    std::array<u8, 9> command_{};
    u32 commandLength_ = 0;
    u32 commandExpected_ = 0;

    // 返す結果バイト。最長は READ/WRITE 系の 7 バイト。
    std::array<u8, 7> result_{};
    u32 resultLength_ = 0;
    u32 resultPos_ = 0;

    bool interruptPending_ = false;
    // 直近に選択されたドライブ番号。結果ステータスに載せる。
    u8 selectedDrive_ = 0;

    // ドライブごとの現在シリンダ。SEEK / RECALIBRATE が動かし、
    // SENSE INTERRUPT STATUS が返す。
    std::array<u8, kDriveCount> presentCylinder_{};

    // SENSE INTERRUPT STATUS が返す ST0。シークの完了時に組み立てておく。
    u8 pendingSt0_ = 0;
    // その割り込みを起こしたドライブ。シリンダ番号を引くのに使う。
    u8 interruptDrive_ = 0;

    std::array<FloppyImage*, kDriveCount> images_{};

    // --- 実行フェーズの状態 ---
    Transfer transfer_ = Transfer::None;
    // 転送中の C/H/R/N。READ/WRITE DATA のパラメータから始まり、
    // セクタを流し切るたびに進む。
    u8 currentCylinder_ = 0;
    u8 currentHead_ = 0;
    u8 currentRecord_ = 0;
    u8 currentN_ = 0;
    u8 endOfTrack_ = 0;
    bool multiTrack_ = false;
    // セクタバッファと、その中の位置。
    std::array<u8, kMaxSectorSize> sectorBuffer_{};
    u32 sectorBytes_ = 0;
    u32 sectorPos_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_FDC_H
