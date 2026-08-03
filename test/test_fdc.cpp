// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: FD ドライブが繋がっていないことを IPL-ROM が諦められる形で
// 返し、かつメインステータスが「コマンドを受け付けられる状態」へ必ず戻ること。
//
// 本エミュレータの起動デバイスは SASI だが、IPL-ROM は起動デバイスに関わらず
// FDC を初期化しに来る。その待ちループはどれもタイムアウトを持たないので、
// メインステータスが一度でも塞がったままになると起動がそこで永久に止まる。
//
// 特に危ないのが結果フェーズの置き去りである。IPL-ROM は DMA を使う
// READ/WRITE DATA の結果を、コマンド送出のその場では読まず FDC 割り込み
// ハンドラ ($FF1130) に任せている。本エミュレータは FDC の割り込み線を
// 配線していないので、結果バイトを積むと誰も読まない。すると次のコマンド
// 送出ルーチン ($FF9036) が CB (bit4) の落ちるのを待ち続けて止まる。
// この退行は「FDC のテストが通っている」だけでは捕まらず、起動を最後まで
// 走らせて初めて分かるので、ここで直接ステータスを見る。

#include "dev/fdc.h"
#include "doctest.h"

namespace
{

// コマンド 1 バイトとパラメータを順に送る。
void sendCommand(x68k::Fdc& fdc, std::initializer_list<x68k::u8> bytes)
{
    for (const x68k::u8 byte : bytes)
    {
        fdc.writeData(byte);
    }
}

// メインステータスが「コマンド待ち」を示しているか。
// IPL-ROM の $FF9006 は下位 5bit が 0 になるのを完了条件にしている。
bool isIdle(const x68k::Fdc& fdc)
{
    return (fdc.readStatus() & 0x1Fu) == 0;
}

}  // namespace

TEST_CASE("FDC はリセット直後にコマンドを受け付けられる")
{
    x68k::Fdc fdc;
    fdc.reset();

    // RQM が立ち、CB と DIO は落ちている ($FF904C-$FF9060 が待つ条件)。
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusRqm) != 0);
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusCb) == 0);
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusDio) == 0);
    CHECK(isIdle(fdc));
}

TEST_CASE("SENSE DRIVE STATUS はレディを立てずに返す")
{
    x68k::Fdc fdc;
    fdc.reset();

    // $FF89CC が発行するもの: コマンド $04 とドライブ番号。
    sendCommand(fdc, {0x04, 0x00});

    // 結果フェーズ。IPL-ROM の $FF89DE は RQM|DIO|CB ($D0) が揃うのを待つ。
    CHECK((fdc.readStatus() & 0xD0u) == 0xD0u);

    // ST3。bit5 (レディ) が落ちていることがドライブ未接続の表明。
    // $FF90BC の btst #29,d0 がこのビットを見て、立っていなければ諦める。
    const x68k::u8 st3 = fdc.readData();
    CHECK((st3 & 0x20u) == 0);

    // 結果を読み切ったらコマンド待ちへ戻る。
    CHECK(isIdle(fdc));
}

TEST_CASE("READ DATA は結果フェーズを残さずコマンド待ちへ戻る")
{
    x68k::Fdc fdc;
    fdc.reset();

    // $FF8B06 が送る 9 バイト。先頭は MT/MF 付きの READ DATA ($46)。
    sendCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x06, 0x03, 0x06, 0x35, 0xFF});

    // ここで結果バイトを積むと、割り込みで拾う相手がいないため
    // CB が立ちっぱなしになり $FF904C で止まる。
    CHECK(isIdle(fdc));
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusCb) == 0);

    // 続けて次のコマンドを送れる。
    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readStatus() & 0xD0u) == 0xD0u);
}

TEST_CASE("WRITE DATA も結果フェーズを残さない")
{
    x68k::Fdc fdc;
    fdc.reset();

    sendCommand(fdc, {0x45, 0x00, 0x00, 0x00, 0x06, 0x03, 0x06, 0x35, 0xFF});
    CHECK(isIdle(fdc));
}

TEST_CASE("SEEK と RECALIBRATE は結果を持たず割り込みだけ上げる")
{
    x68k::Fdc fdc;
    fdc.reset();

    // RECALIBRATE ($07)。$FF8C26 が発行する。
    sendCommand(fdc, {0x07, 0x00});
    CHECK(isIdle(fdc));
    CHECK(fdc.hasInterrupt());

    // SENSE INTERRUPT STATUS ($08) で異常終了を受け取る。
    // ST0 の bit7-6 = 01 が異常終了、bit4 が装置チェック。
    sendCommand(fdc, {0x08});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0x40u);
    CHECK((st0 & 0x10u) != 0);
    (void)fdc.readData();  // シリンダ番号
    CHECK(isIdle(fdc));
    CHECK_FALSE(fdc.hasInterrupt());
}

TEST_CASE("SPECIFY は結果を返さない")
{
    x68k::Fdc fdc;
    fdc.reset();

    // $03 とタイミングパラメータ 2 バイト。結果フェーズを持たないコマンド。
    sendCommand(fdc, {0x03, 0xD0, 0x10});
    CHECK(isIdle(fdc));
}

TEST_CASE("割り込みが無いのに SENSE INTERRUPT STATUS を投げたら無効コマンド")
{
    x68k::Fdc fdc;
    fdc.reset();

    sendCommand(fdc, {0x08});

    // ST0 の bit7-6 = 11 が無効コマンド。ここで正常終了を返すと
    // IPL-ROM が「まだ処理すべき割り込みがある」と判断して回り続ける。
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0xC0u);
    CHECK(isIdle(fdc));
}

TEST_CASE("結果を読み切らないまま次のコマンドが来たら受け付ける")
{
    x68k::Fdc fdc;
    fdc.reset();

    // SENSE DRIVE STATUS の結果を残したまま放置する。
    sendCommand(fdc, {0x04, 0x00});
    CHECK((fdc.readStatus() & x68k::Fdc::kStatusCb) != 0);

    // 次のコマンドで古い結果は捨てられる。ここで無視すると
    // CB が落ちず、$FF9036 のコマンド送出が永久に止まる。
    sendCommand(fdc, {0x04, 0x01});
    CHECK((fdc.readStatus() & 0xD0u) == 0xD0u);
    const x68k::u8 st3 = fdc.readData();
    CHECK((st3 & 0x20u) == 0);
    CHECK((st3 & 0x03u) == 0x01u);  // 新しいコマンドのドライブ番号
    CHECK(isIdle(fdc));
}

TEST_CASE("未知のコマンドは無効コマンドとして返す")
{
    x68k::Fdc fdc;
    fdc.reset();

    sendCommand(fdc, {0x1F});
    const x68k::u8 st0 = fdc.readData();
    CHECK((st0 & 0xC0u) == 0xC0u);
    CHECK(isIdle(fdc));
}
