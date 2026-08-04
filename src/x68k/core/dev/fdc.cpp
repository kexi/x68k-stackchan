// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "fdc.h"

namespace x68k
{
namespace
{

// コマンドコード (1 バイト目の下位 5bit)。
// 上位 3bit は MT/MF/SK の修飾ビットなので落として判定する。
constexpr u8 kCmdSpecify = 0x03;
constexpr u8 kCmdSenseDriveStatus = 0x04;
constexpr u8 kCmdRecalibrate = 0x07;
constexpr u8 kCmdSenseInterruptStatus = 0x08;
constexpr u8 kCmdSeek = 0x0F;
constexpr u8 kCmdReadId = 0x0A;
constexpr u8 kCmdWriteId = 0x0D;
constexpr u8 kCmdReadTrack = 0x02;
constexpr u8 kCmdWriteData = 0x05;
constexpr u8 kCmdReadData = 0x06;
constexpr u8 kCmdWriteDeleted = 0x09;
constexpr u8 kCmdReadDeleted = 0x0C;

// コマンド 1 バイト目の修飾ビット。
// MT (Multi Track) が立っていると、EOT に達したあとヘッドを替えて
// 同じシリンダの裏面へ続ける。IPL-ROM は上位 2bit を呼び出し元から
// 引き継いで送るので ($FF8AE0 の and.b #$c0,d1 → $FF8AE4 の or.b #$6,d1)、
// MT が立つかどうかは呼び出し元次第。
constexpr u8 kCmdFlagMultiTrack = 0x80;

// 結果ステータス ST0 のビット。
// 終了コード (IC) は bit7-6 の 2bit で、uPD765/72065 の定義は
//   00 正常終了 / 01 異常終了 / 10 無効なコマンド / 11 レディ状態が変化した。
//
// Why not 無効なコマンドを $C0 (両ビット) にしないか: 一度そう書いていたが、
// それは 11 = 「レディ状態が変化した」の意味になる。IPL-ROM の FDC 割り込み
// ハンドラは $FF1162 で CMP.B #$80,D0 / BEQ $FF11A2 と書かれていて、
// ちょうど $80 のときだけハンドラを抜ける。$C0 を返すとこの分岐が外れ、
// ハンドラが結果フェーズを読み続けて抜けられなくなる (実際に SASI 起動が
// 画面出力前で止まった)。$80 が正しいことはデータシートと ROM の双方が示す。
constexpr u8 kSt0AbnormalTermination = 0x40;
constexpr u8 kSt0InvalidCommand = 0x80;
// bit4 = 装置チェック (ドライブが応答しない)。
constexpr u8 kSt0EquipmentCheck = 0x10;
// bit5 = シーク終了。
constexpr u8 kSt0SeekEnd = 0x20;
// bit3 = ノットレディ。
constexpr u8 kSt0NotReady = 0x08;

// ST1 のビット。
//
// 結果フェーズを積むコマンドが SENSE 系しか無いので、ST1 は
// 実際には誰にも読まれない。それでも result_ に書いているのは、
// 将来 FDC 割り込みを配線して $C90 経由の結果を通すときに、
// どのビットを立てるべきかの判断をここに残しておくため。
constexpr u8 kSt1EndOfCylinder = 0x80;  // EOT を超えて読もうとした
constexpr u8 kSt1NotWritable = 0x02;    // ライトプロテクト

// ST3 のビット。SENSE DRIVE STATUS が返す。
constexpr u8 kSt3Ready = 0x20;            // bit5 = レディ
constexpr u8 kSt3WriteProtected = 0x40;   // bit6 = ライトプロテクト
constexpr u8 kSt3Track0 = 0x10;           // bit4 = トラック 0 にいる
constexpr u8 kSt3TwoSide = 0x08;          // bit3 = 両面ドライブ
constexpr u8 kSt3HeadAddress = 0x04;      // bit2 = 現在のヘッド
constexpr u8 kSt3UnitSelectMask = 0x03u;  // bit1-0 = ドライブ番号

// N (セクタ長コード) からバイト数へ。128 << N。
// N=3 が X68000 標準の 2HD (1024 バイト/セクタ)。
u32 sectorSizeFromN(u8 n)
{
    return 128u << (n & 0x07u);
}

// 対応するジオメトリの一覧。長さで引く。
//
// 2HD が X68000 の標準。2DD は 5 インチ / 3.5 インチの両方で使われた。
constexpr FloppyGeometry kKnownGeometries[] = {
    {77, 2, 8, 1024},  // 2HD  1,261,568 バイト (X68000 標準)
    {77, 2, 26, 256},  // 2HD  1,025,024 バイト (1.25MB / 256B セクタ)
    {80, 2, 8, 512},   // 2DD    655,360 バイト
    {80, 2, 9, 512},   // 2DD    737,280 バイト
    {40, 2, 8, 512},   // 2D     327,680 バイト
};

}  // namespace

bool floppyGeometryFromSize(u32 imageBytes, FloppyGeometry* out)
{
    for (const FloppyGeometry& geo : kKnownGeometries)
    {
        if (geo.totalBytes() != imageBytes)
        {
            continue;
        }
        if (out != nullptr)
        {
            *out = geo;
        }
        return true;
    }
    return false;
}

FloppyFormat detectFloppyFormat(u32 fileBytes, u32* dataOffset, FloppyGeometry* geometry)
{
    // 先にヘッダ無し (XDF) として見る。
    //
    // Why not DIM を先に見るか: DIM の判定は「256 を引いたら既知の長さ」
    // でしかない。XDF を先に試さないと、たまたま「既知の長さ + 256」に
    // なった XDF (存在しないが、将来ジオメトリを足せば起こりうる) を
    // DIM と誤認して先頭 256 バイトを捨ててしまう。
    if (floppyGeometryFromSize(fileBytes, geometry))
    {
        if (dataOffset != nullptr)
        {
            *dataOffset = 0;
        }
        return FloppyFormat::Xdf;
    }

    const bool hasDimHeader = fileBytes > kDimHeaderBytes;
    if (hasDimHeader && floppyGeometryFromSize(fileBytes - kDimHeaderBytes, geometry))
    {
        if (dataOffset != nullptr)
        {
            *dataOffset = kDimHeaderBytes;
        }
        return FloppyFormat::Dim;
    }

    return FloppyFormat::Unknown;
}

void Fdc::reset()
{
    phase_ = Phase::Command;
    command_.fill(0);
    commandLength_ = 0;
    commandExpected_ = 0;
    result_.fill(0);
    resultLength_ = 0;
    resultPos_ = 0;
    interruptPending_ = false;
    selectedDrive_ = 0;
    presentCylinder_.fill(0);
    pendingSt0_ = 0;
    interruptDrive_ = 0;
    transfer_ = Transfer::None;
    currentCylinder_ = 0;
    currentHead_ = 0;
    currentRecord_ = 0;
    currentN_ = 0;
    endOfTrack_ = 0;
    multiTrack_ = false;
    sectorBytes_ = 0;
    sectorPos_ = 0;

    // イメージはリセットで外さない。SASI の転送バッファと同じ理由で、
    // 「外から与えられた設定」はリセットの対象ではない。実機の電源を
    // 入れ直してもドライブに入れたディスクは入ったままである。
}

void Fdc::setImage(u32 drive, FloppyImage* image)
{
    if (drive >= kDriveCount)
    {
        return;
    }
    images_[drive] = image;
}

FloppyImage* Fdc::image(u32 drive) const
{
    if (drive >= kDriveCount)
    {
        return nullptr;
    }
    return images_[drive];
}

FloppyImage* Fdc::currentImage() const
{
    FloppyImage* const img = image(selectedDrive_);
    if (img == nullptr || !img->isPresent())
    {
        return nullptr;
    }
    return img;
}

u32 Fdc::parameterCount(u8 commandByte)
{
    switch (commandByte & 0x1Fu)
    {
        case kCmdSpecify:
            return 2;
        case kCmdSenseDriveStatus:
        case kCmdRecalibrate:
        case kCmdReadId:
            return 1;
        case kCmdSenseInterruptStatus:
            return 0;
        case kCmdSeek:
            return 2;
        case kCmdWriteId:
            return 5;
        case kCmdReadTrack:
        case kCmdWriteData:
        case kCmdReadData:
        case kCmdWriteDeleted:
        case kCmdReadDeleted:
            return 8;
        default:
            // 未知のコマンド。パラメータ無しとして扱い、
            // 「無効なコマンド」の結果を返す。
            return 0;
    }
}

u8 Fdc::readStatus() const
{
    switch (phase_)
    {
        case Phase::Command:
            // コマンドを受け付けられる状態。
            //
            // IPL-ROM はこの順で待つ ($FF904C-$FF9062):
            //   CB (bit4) が 0 → RQM (bit7) が 1 → DIO (bit6) が 0
            // コマンドの途中 (パラメータ待ち) は CB を立てる。
            if (commandLength_ > 0)
            {
                return static_cast<u8>(kStatusRqm | kStatusCb);
            }
            return kStatusRqm;

        case Phase::Execute:
            // 実行フェーズ。CB と、方向に応じた DIO を立てる。
            //
            // RQM も立てる。**非 DMA モードでは CPU が自分でデータポートを
            // 読む**ので、RQM が無いと 1 バイトも取れない。
            //
            // Why not 「DMA で流すぶんが横から抜ける」と考えて伏せないか:
            // 以前そう書いて伏せていたが、実測で誤りだった。IPL-ROM の
            // フロッピー起動経路 ($FF89DE) は
            //   MOVE.B $E94001,D0 / AND.B #$D0,D0 / CMP.B #$D0,D0 / BNE -16
            // と RQM|DIO|CB ($D0) が揃うのを**タイムアウト無しで**待つ。
            // RQM を伏せると status が $50 のまま変わらず、ROM はここで
            // 永久に回る (実測: 6 億サイクル回しても抜けない)。
            //
            // DMA と食い合う心配は要らない。DMAC が起動されていれば
            // dmaRead() が先にバッファを空にして finishExecute() へ進むので、
            // CPU がデータポートを読む機会はそもそも来ない。両方が同じ
            // バイトを取りに来る状況は作れない。
            if (transfer_ == Transfer::Read)
            {
                return static_cast<u8>(kStatusRqm | kStatusCb | kStatusDio);
            }
            // 書き込みは CPU から届くのを待つ。DIO は落としたまま
            // (CPU → FDC の向き) で RQM を立てる。
            return static_cast<u8>(kStatusRqm | kStatusCb);

        case Phase::Result:
            // 結果を返す状態。IPL-ROM は RQM|DIO|CB ($D0) が揃うのを待つ
            // ($FF89DE)。
            return static_cast<u8>(kStatusRqm | kStatusDio | kStatusCb);
    }
    return kStatusRqm;
}

u8 Fdc::readData()
{
    // 非 DMA モード: 実行フェーズでは CPU が 1 バイトずつ取りに来る。
    //
    // IPL-ROM のフロッピー起動経路はこの形で読む。DMAC を起動せずに
    // READ DATA を投げ、$FF89DE で RQM|DIO|CB を待ってからデータポートを
    // 舐める。ここで返さないと 1 バイトも渡らず、ROM は永久に待つ。
    //
    // Why not DMA 経路と共通にしないか: DMA は dmaRead() が
    // 「DMAC が要求した長さ」で駆動するのに対し、こちらは CPU が
    // 好きなだけ読む。終了条件が違う (向こうは終端カウント、こちらは
    // セクタを配り切ったら次のセクタを用意する) ので、同じ関数に
    // 押し込むと両方の条件が絡んで読み解けなくなる。
    if (phase_ == Phase::Execute && transfer_ == Transfer::Read)
    {
        if (sectorPos_ >= sectorBytes_)
        {
            return 0u;
        }
        const u8 value = sectorBuffer_[sectorPos_++];
        if (sectorPos_ >= sectorBytes_)
        {
            // 1 セクタ配り切った。次のセクタを用意できれば続ける。
            // 終わり方は dmaRead() と同じにする (EOT 超えは End of Cylinder)。
            if (!advanceSector())
            {
                pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
                result_[1] = kSt1EndOfCylinder;
                finishExecute();
            }
            else if (!loadCurrentSector())
            {
                pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
                finishExecute();
            }
        }
        return value;
    }

    if (phase_ != Phase::Result || resultPos_ >= resultLength_)
    {
        return 0u;
    }

    const u8 value = result_[resultPos_++];
    if (resultPos_ >= resultLength_)
    {
        // 全部返したのでコマンド待ちへ戻る。
        // ここで戻さないと IPL-ROM が次のコマンドを送れない。
        phase_ = Phase::Command;
        commandLength_ = 0;
        resultLength_ = 0;
        resultPos_ = 0;
    }
    return value;
}

void Fdc::writeData(u8 value)
{
    if (phase_ == Phase::Result)
    {
        // 結果を読み切らないまま次のコマンドが来た。溜まった結果を捨てて
        // 受け付ける。
        //
        // IPL-ROM は DMA を使うコマンド (READ/WRITE DATA) の結果フェーズを
        // インラインでは読まず、FDC 割り込みハンドラ ($FF1130) に任せている。
        // 本エミュレータは FDC の割り込み線 (実機では IRQ レベル 1 の
        // オートベクタで、MFP は通さない) を配線していないので、結果は
        // 誰にも読まれずに残る。残したまま無視すると次のコマンド送出
        // ($FF9036) が CB の落ちるのを永久に待つ。CB を無条件に落とす手も
        // あるが、それだと SENSE DRIVE STATUS のように IPL-ROM が
        // インラインで結果を読むコマンド ($FF89DE) が読めなくなるので、
        // 「次のコマンドが来た時点で捨てる」形にした。
        phase_ = Phase::Command;
        commandLength_ = 0;
        resultLength_ = 0;
        resultPos_ = 0;
    }

    if (phase_ != Phase::Command)
    {
        // 実行中 (DMA 転送の途中) の書き込みは無視する。
        //
        // Why not ここで転送を中断するか: DMAC が MTC 分を流し切る前に
        // CPU がデータポートを書くのは、そもそも起こらない順序である
        // (IPL-ROM は $FF8F3C で DMAC を起動してから $FF9014 で完了を待つ)。
        // 中断できる形にすると、異常時に「途中まで書けたセクタ」が
        // ディスクに残る経路ができてしまう。
        return;
    }

    if (commandLength_ == 0)
    {
        // コマンドの 1 バイト目。
        command_[0] = value;
        commandLength_ = 1;
        commandExpected_ = 1 + parameterCount(value);
    }
    else if (commandLength_ < command_.size())
    {
        command_[commandLength_++] = value;
    }

    if (commandLength_ >= commandExpected_)
    {
        executeCommand();
    }
}

// $E94005 / $E94007 はどちらも X68000 固有のドライブ選択・モーター制御。
// bit1-0 がドライブ番号、bit7 がモーター。IPL-ROM の $FF909E は
// 「$80 | ドライブ番号」を $E94007 へ書いてからコマンドを送る。
//
// ここで覚えるのは「次のコマンドが US フィールドを持たなかったときの
// 既定のドライブ」でしかない。uPD72065 のコマンドはほぼすべて 2 バイト目に
// HD|US を持ち、そちらが最終的な選択になる (IPL-ROM も $FF8FEE の
// and.b #$3,d1 でドライブ番号をコマンドへ入れてから送る)。
//
// Why not このポートを無視するか: モーターの状態は実機では回転待ちを
// 伴い、選択もコマンドを持たない操作 (イジェクト検出など) の対象になる。
// 今は振る舞いの差にならないが、落とすと「書いたはずの選択がどこにも
// 残らない」状態になり、後から回転待ちを足すときに繋ぎ先が無くなる。
void Fdc::writeDriveControl(u8 value)
{
    selectedDrive_ = static_cast<u8>(value & 0x03u);
}

void Fdc::writeDriveSelect(u8 value)
{
    selectedDrive_ = static_cast<u8>(value & 0x03u);
}

u8 Fdc::unitSelect() const
{
    // ST0 の bit2 = ヘッド、bit1-0 = ドライブ番号。
    const u8 head = static_cast<u8>((currentHead_ & 1u) << 2);
    return static_cast<u8>(head | (selectedDrive_ & 0x03u));
}

u8 Fdc::buildSt3(u32 drive) const
{
    // ドライブ番号は常に載せる。IPL-ROM は結果からどのドライブの答えかを
    // 引く ($FF8C44 が $C90 + ドライブ*8 を見るのと同じ考え方)。
    u8 st3 = static_cast<u8>(drive & kSt3UnitSelectMask);

    // 両面ドライブであることは、ディスクの有無に関わらず表明する。
    // ドライブ自体は繋がっている。
    st3 = static_cast<u8>(st3 | kSt3TwoSide);

    const bool isTrack0 = drive < kDriveCount && presentCylinder_[drive] == 0;
    if (isTrack0)
    {
        st3 = static_cast<u8>(st3 | kSt3Track0);
    }
    if ((currentHead_ & 1u) != 0)
    {
        st3 = static_cast<u8>(st3 | kSt3HeadAddress);
    }

    FloppyImage* const img = image(drive);
    const bool hasMedia = img != nullptr && img->isPresent();
    if (!hasMedia)
    {
        // レディを立てないことが「メディアが入っていない」の表明。
        // $FF90BC の btst #29,d0 (= ST3 の bit5) がこれを見て諦める。
        return st3;
    }

    st3 = static_cast<u8>(st3 | kSt3Ready);
    if (img->isWriteProtected())
    {
        st3 = static_cast<u8>(st3 | kSt3WriteProtected);
    }
    return st3;
}

void Fdc::executeCommand()
{
    const u8 opcode = static_cast<u8>(command_[0] & 0x1Fu);
    resultPos_ = 0;

    switch (opcode)
    {
        case kCmdSpecify:
            // タイミングパラメータ (SRT/HUT/HLT/ND) の設定。
            // 結果を返さずコマンド待ちへ戻る。
            //
            // ND (bit0 of command_[2]) で非 DMA モードを選べるが、
            // X68000 の IPL-ROM も Human68k も DMA を使うので、
            // 値を覚えるだけで振る舞いは変えない。
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;

        case kCmdSenseInterruptStatus:
            // 直前の割り込みの原因を返す。ST0 と現在のシリンダ番号。
            //
            // 割り込みが無いのに呼ばれたら「無効なコマンド」を返す。
            // ここで正常終了を返し続けると IPL-ROM が
            // 「まだ処理中の割り込みがある」と判断してループする。
            if (interruptPending_)
            {
                result_[0] = pendingSt0_;
                result_[1] = interruptDrive_ < kDriveCount ? presentCylinder_[interruptDrive_] : 0u;
                resultLength_ = 2;
                interruptPending_ = false;
            }
            else
            {
                result_[0] = kSt0InvalidCommand;
                resultLength_ = 1;
            }
            break;

        case kCmdSenseDriveStatus:
            // ドライブの状態 (ST3)。メディアが入っていればレディを立てる。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            currentHead_ = static_cast<u8>((command_[1] >> 2) & 1u);
            result_[0] = buildSt3(selectedDrive_);
            resultLength_ = 1;
            break;

        case kCmdRecalibrate:
            // トラック 0 へ戻す。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            presentCylinder_[selectedDrive_] = 0;
            interruptDrive_ = selectedDrive_;
            // RECALIBRATE のパラメータは HD|US ではなく US だけで、ヘッドは
            // 常に 0 に戻る。ここで落とさないと直前のコマンドのヘッドが
            // 残り、ST0 の bit2 (HD) が立ったままになる。
            //
            // Why not 放置してよくないか: この ST0 は割り込みハンドラ経由で
            // $C90 に積まれ、ドライブの状態としてあとから読まれる。直前の
            // コマンドのヘッドが残ると「ヘッド 1 にいるドライブ」として
            // 記録され、以降の判定がそれを引きずる。RECALIBRATE の意味は
            // 「トラック 0 かつヘッド 0 へ戻す」なので、ここで落とすのが正しい。
            currentHead_ = 0;
            // メディアが無ければ「シーク終了 + 異常終了 + ノットレディ」。
            //
            // Why not 装置チェック (EC) も立てないか: IPL-ROM は起動時に
            // ドライブ 0-3 へ順に RECALIBRATE を投げて存在を調べ
            // ($FF8C26 経由。実測のパラメータは $04/$05/$06/$07)、その結果を
            // 割り込みハンドラ経由で $C90 に積む。積まれた値を最終的に読むのは
            // ブートセクタ側 ($00007FBA の AND.L / $00007FC0 の BNE) で、
            // そのマスクは ST0 の EC (bit4) を残す。ドライブが無いだけで EC を
            // 立てると、ここが「装置エラー」と判定してエラー処理へ抜ける。
            // 実測では ST0=$78 のとき D0=$10000000 が残って分岐し、フロッピーを
            // 挿していないときに SASI からの起動まで巻き添えで止まった。
            // NR (bit3) は同じマスクで落ちるので、こちらで表すのが正しい。
            //
            // 実機の uPD72065 はトラック 0 信号が返らないまま規定ステップ数を
            // 使い切ると EC で終わるが、それは「ドライブはあるが壊れている」
            // 場合の話で、そもそもドライブが繋がっていない (= ノットレディ) の
            // とは別物である。
            //
            // この退行は割り込みを配線するまで現れなかった。配線前は $C90 が
            // 誰にも埋められず、ブートセクタは 0 を読んでいたため。
            pendingSt0_ = currentImage() != nullptr
                              ? static_cast<u8>(kSt0SeekEnd | unitSelect())
                              : static_cast<u8>(kSt0SeekEnd | kSt0AbnormalTermination |
                                                kSt0NotReady | unitSelect());
            interruptPending_ = true;
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;

        case kCmdSeek:
        {
            // 指定シリンダへ移動する。command_[1] が HD|US、command_[2] が NCN。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            currentHead_ = static_cast<u8>((command_[1] >> 2) & 1u);
            interruptDrive_ = selectedDrive_;

            FloppyImage* const img = currentImage();
            const bool hasMedia = img != nullptr;
            // シリンダ数を超える指定は異常終了。ヘッドは物理的にそこまで
            // 動けるが、その位置に ID が無いので次の READ で必ず失敗する。
            // ここで弾いておかないと、失敗の原因が READ 側にずれて見える。
            const bool isInRange = hasMedia && command_[2] < img->geometry().cylinders;
            if (isInRange)
            {
                presentCylinder_[selectedDrive_] = command_[2];
                pendingSt0_ = static_cast<u8>(kSt0SeekEnd | unitSelect());
            }
            else
            {
                // 実機はシリンダ番号を進めてしまうが、ここでは動かさない。
                // 動かすと以後の SENSE INTERRUPT STATUS が「存在しない位置」を
                // 返し続け、SENSE DRIVE STATUS の Track0 も嘘になる。
                pendingSt0_ = static_cast<u8>(kSt0SeekEnd | kSt0AbnormalTermination |
                                              (hasMedia ? 0u : kSt0NotReady) | unitSelect());
            }
            interruptPending_ = true;
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;
        }

        case kCmdReadId:
        {
            // 現在のトラックで最初に見つかる ID を「返す」コマンド。
            //
            // ただし結果フェーズは積まない。IPL-ROM は READ ID の結果を
            // その場では読まず、コマンド送出のあと $FF8A50 → $FF9014 →
            // $FF9006 で「メインステータスの下位 5bit が 0 になる」のを待ち、
            // 実際の 7 バイトは FDC 割り込みハンドラ ($FF1130) が積んだ
            // ドライブごとの状態表 $C90 から $FF8C44 で引く。
            //
            // 本エミュレータは FDC の割り込み線 (実機は IRQ レベル 1 の
            // オートベクタ) を配線していないので、ここで結果を積むと
            // 誰も読まないまま CB が立ち続け、$FF9006 が永久に回る。
            // 実測でそうなった: SPECIFY → SENSE DRIVE STATUS → RECALIBRATE
            // → SEEK と進んだあと READ ID ($4A) で status=$D0 のまま停止。
            //
            // SENSE DRIVE STATUS / SENSE INTERRUPT STATUS だけが例外で、
            // あちらは $FF89DE / $FF9036 の直後にインラインで読まれる。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            currentHead_ = static_cast<u8>((command_[1] >> 2) & 1u);
            interruptDrive_ = selectedDrive_;

            FloppyImage* const img = currentImage();
            pendingSt0_ =
                img != nullptr
                    ? unitSelect()
                    : static_cast<u8>(kSt0AbnormalTermination | kSt0NotReady | unitSelect());
            interruptPending_ = true;
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;
        }

        case kCmdReadData:
        case kCmdReadDeleted:
            // READ DELETED DATA はデータマークの区別だけが違う。本エミュレータの
            // イメージ形式はデータマークを持たない (XDF/DIM はセクタの中身しか
            // 記録しない) ので、通常の READ と同じに扱う。
            beginReadWrite(false);
            return;

        case kCmdWriteData:
        case kCmdWriteDeleted:
            beginReadWrite(true);
            return;

        case kCmdReadTrack:
        case kCmdWriteId:
            // READ TRACK (診断用の全セクタ読み) と WRITE ID (フォーマット) は
            // 実装しない。
            //
            // Why not 実装するか: どちらもセクタ ID フィールドを個別に扱う
            // 必要があり、XDF のような「セクタの中身だけを並べた」イメージでは
            // 表現できない。中途半端に成功を返すと、フォーマットしたつもりの
            // ディスクが実際には元のままという壊れ方をする。異常終了を返して
            // 呼び出し側に諦めさせる方が安全。
            selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
            currentHead_ = static_cast<u8>((command_[1] >> 2) & 1u);
            interruptPending_ = true;
            interruptDrive_ = selectedDrive_;
            pendingSt0_ =
                static_cast<u8>(kSt0AbnormalTermination | kSt0EquipmentCheck | unitSelect());
            phase_ = Phase::Command;
            commandLength_ = 0;
            resultLength_ = 0;
            return;

        default:
            // 未知のコマンド。
            result_[0] = kSt0InvalidCommand;
            resultLength_ = 1;
            break;
    }

    phase_ = Phase::Result;
    commandLength_ = 0;
}

void Fdc::beginReadWrite(bool isWrite)
{
    // READ/WRITE DATA のパラメータ ($FF8B06 が送る 9 バイト):
    //   [0] コマンド (MT|MF|SK|opcode)
    //   [1] HD|US   [2] C   [3] H   [4] R   [5] N
    //   [6] EOT     [7] GPL [8] DTL
    selectedDrive_ = static_cast<u8>(command_[1] & 0x03u);
    currentHead_ = static_cast<u8>((command_[1] >> 2) & 1u);
    multiTrack_ = (command_[0] & kCmdFlagMultiTrack) != 0;
    currentCylinder_ = command_[2];
    // H は command_[3] にも入る。ID フィールドとの照合用なので、
    // ヘッド選択は command_[1] の HD を正とする (実機も同じ)。
    currentRecord_ = command_[4];
    currentN_ = command_[5];
    endOfTrack_ = command_[6];

    interruptDrive_ = selectedDrive_;
    transfer_ = Transfer::None;
    sectorBytes_ = 0;
    sectorPos_ = 0;

    FloppyImage* const img = currentImage();
    if (img == nullptr)
    {
        // メディアが無い。実行フェーズに入らず即終了する。
        // 結果は積まない (積むと CB が落ちず $FF9036 で止まる)。
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | kSt0NotReady | unitSelect());
        finishExecute();
        return;
    }

    const bool isProtected = isWrite && img->isWriteProtected();
    if (isProtected)
    {
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
        finishExecute();
        return;
    }

    // N が示すセクタ長がイメージのそれと違えば、読んでも意味のある
    // バイトにならない。黙って進めると転送量とセクタ境界がずれ、
    // ファイルシステムが静かに壊れる。
    const u32 wanted = sectorSizeFromN(currentN_);
    const bool sizeMatches = wanted == img->geometry().sectorSize;
    const bool fitsBuffer = wanted <= kMaxSectorSize;
    if (!sizeMatches || !fitsBuffer)
    {
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
        finishExecute();
        return;
    }
    sectorBytes_ = wanted;

    // ヘッドが今いるシリンダと、コマンドが指す C が食い違っていたら
    // 指定のセクタは見つからない。実機の uPD72065 も ID フィールドの C を
    // 照合してから読む。
    const bool isOnTrack = currentCylinder_ == presentCylinder_[selectedDrive_];
    if (!isOnTrack)
    {
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
        finishExecute();
        return;
    }

    transfer_ = isWrite ? Transfer::Write : Transfer::Read;
    phase_ = Phase::Execute;
    commandLength_ = 0;
    resultLength_ = 0;
    sectorPos_ = 0;

    if (isWrite)
    {
        // 書き込みはメモリから届くのを待つ。バッファはまだ空でよい。
        return;
    }

    // 読み出しは最初のセクタを先に用意する。DMAC は起動された時点で
    // dmaRead を呼び始めるので、その前に中身が要る。
    if (!loadCurrentSector())
    {
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
        finishExecute();
    }
}

bool Fdc::loadCurrentSector()
{
    FloppyImage* const img = currentImage();
    if (img == nullptr)
    {
        return false;
    }
    if (!img->readSector(currentCylinder_, currentHead_, currentRecord_, sectorBuffer_.data()))
    {
        return false;
    }
    sectorPos_ = 0;
    return true;
}

bool Fdc::storeCurrentSector()
{
    FloppyImage* const img = currentImage();
    if (img == nullptr)
    {
        return false;
    }
    return img->writeSector(currentCylinder_, currentHead_, currentRecord_, sectorBuffer_.data());
}

bool Fdc::advanceSector()
{
    // R は 1 起点。EOT が「そのトラックで最後に扱うセクタ番号」。
    const bool isLastOfTrack = currentRecord_ >= endOfTrack_;
    if (!isLastOfTrack)
    {
        ++currentRecord_;
        return true;
    }

    // EOT に達した。MT が立っていて表面 (ヘッド 0) にいるなら裏面へ続ける。
    //
    // Why not MT でシリンダも進めるか: uPD72065 の MT はシリンダをまたがない。
    // 両面を読み終えたらそこで End of Cylinder として終わり、次のシリンダは
    // ホスト側が SEEK し直して新しいコマンドを出す。ここでシリンダを進めると
    // ホストの期待する終了位置とずれ、次のコマンドが 1 トラック先を読む。
    const bool canFlipHead = multiTrack_ && (currentHead_ & 1u) == 0;
    if (canFlipHead)
    {
        currentHead_ = 1;
        currentRecord_ = 1;
        return true;
    }

    return false;
}

void Fdc::finishExecute()
{
    // 実行フェーズを終える。割り込みは上げるが、結果バイトは積まない。
    //
    // IPL-ROM は READ/WRITE DATA の結果を FDC 割り込みハンドラ ($FF1130) で
    // 読み、ドライブごとの状態表 $C90 へ積む。そのハンドラは結果フェーズが
    // 残っていなければ自分で SENSE INTERRUPT STATUS を投げて ST0 を取りに
    // 来る ($FF1152 で CB を見てから $FF1158 の MOVEQ #8,D1) ので、
    // 結果を残しておく必要は無い。
    //
    // Why not ここで結果を積まないか: 積むとメインステータスの CB が
    // 落ちないまま次のコマンド送出 ($FF9036) が永久に止まる。ROM 側の
    // 完了条件は $FF9014 → $FF9006 の「メインステータスの下位 5bit が 0 に
    // なる」なので、即アイドルへ戻すのが正しい。転送の成否は DMAC の CSR
    // (COC/ERR) と、割り込みハンドラが読む ST0 が伝える。
    transfer_ = Transfer::None;
    sectorBytes_ = 0;
    sectorPos_ = 0;
    phase_ = Phase::Command;
    commandLength_ = 0;
    resultLength_ = 0;
    resultPos_ = 0;
    interruptPending_ = true;
}

bool Fdc::dmaRead(u8* value)
{
    if (phase_ != Phase::Execute || transfer_ != Transfer::Read || value == nullptr)
    {
        return false;
    }

    if (sectorPos_ >= sectorBytes_)
    {
        // 今のセクタを流し切った。次のセクタへ進めるなら続ける。
        // これがマルチセクタ転送の本体で、DMAC のターミナルカウントが
        // 尽きるまで (= DMAC が dmaRead を呼ぶのをやめるまで) 続く。
        if (!advanceSector())
        {
            // EOT を超えた。実機はここで End of Cylinder を立てて終わる。
            pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
            result_[1] = kSt1EndOfCylinder;
            finishExecute();
            return false;
        }
        if (!loadCurrentSector())
        {
            pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
            finishExecute();
            return false;
        }
    }

    *value = sectorBuffer_[sectorPos_++];
    return true;
}

void Fdc::dmaComplete(bool isComplete)
{
    if (phase_ != Phase::Execute)
    {
        // 既に自分から終わっている (EOT を超えた、読み書きに失敗した)。
        // pendingSt0_ はそのときに立ててあるので上書きしない。
        return;
    }

    // 溜まりかけのセクタはここで捨てられる (finishExecute が sectorPos_ を
    // 0 に戻すだけで、書き戻しは行わない)。書き戻しは dmaWrite が
    // 「1 セクタ分きっちり溜まった」ときにしか呼ばないので、前半だけが
    // 新しく後半が古いセクタは構造的に作れない。
    //
    // Why not 途中まででも書き戻すか: そうすると、ファイルシステムから見て
    // 「読めるが中身が混ざっている」セクタができる。エラーとして表面化
    // しないぶん、書けなかったと分かる方がまだよい。
    //
    // 状態としては異常終了で残す。ホストが要求した長さを運べていない以上、
    // 正常終了を返すと「書けたつもり」で先へ進まれる。
    const bool hasPartialSector = transfer_ == Transfer::Write && sectorPos_ > 0;
    const bool isClean = isComplete && !hasPartialSector;
    pendingSt0_ = isClean ? unitSelect() : static_cast<u8>(kSt0AbnormalTermination | unitSelect());

    finishExecute();
}

bool Fdc::dmaWrite(u8 value)
{
    if (phase_ != Phase::Execute || transfer_ != Transfer::Write)
    {
        return false;
    }

    // 溜め先からはみ出すなら受け取らない。
    //
    // ここに来るのは sectorPos_ の巻き戻しが漏れたときだけで、正しく
    // 動いている限り成立しない。それでも検査を置くのは、外れたときの
    // 壊れ方が Fdc のメンバを順に踏み潰す領域外書き込みだからである。
    // macOS のホストテストは ASan を張れない (test/CMakeLists.txt に
    // 理由がある) ので、この種の踏み外しは黙って通り抜けてしまう。
    if (sectorPos_ >= sectorBytes_ || sectorPos_ >= sectorBuffer_.size())
    {
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
        finishExecute();
        return false;
    }

    sectorBuffer_[sectorPos_++] = value;
    if (sectorPos_ < sectorBytes_)
    {
        return true;
    }

    // 1 セクタ揃ったので書き戻す。
    //
    // Why not 全セクタを溜めてから一度に書くか: 溜める先が要る (最大で
    // トラック 1 本ぶん) うえ、途中で失敗したときにどこまで書けたかが
    // 分からなくなる。セクタ単位で書けば、失敗した時点で止まり、
    // それ以前のセクタは確実にディスクへ載っている。
    if (!storeCurrentSector())
    {
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
        result_[1] = kSt1NotWritable;
        finishExecute();
        return false;
    }

    if (!advanceSector())
    {
        // EOT を超えた。次のバイトは受け取れない。
        pendingSt0_ = static_cast<u8>(kSt0AbnormalTermination | unitSelect());
        result_[1] = kSt1EndOfCylinder;
        finishExecute();
        return true;  // 今の 1 バイトは受け取れている
    }
    sectorPos_ = 0;
    return true;
}

}  // namespace x68k
