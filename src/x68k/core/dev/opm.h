// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// FM 音源 YM2151 (OPM) ($E90000)。
//
// X68000 の FM 音源。8ch × 4 オペレータ、位相変調 (いわゆる FM) 合成。
// レジスタは 2 本の口だけで触る。$E90001 にレジスタ番号を書き、$E90003 に
// 値を書く。読み出しは $E90003 のステータス (bit7 = BUSY, bit1/0 = タイマ)。
//
// IPL-ROM の実際の使い方 ($FF9C80 のサブルーチン、rom/iplrom.dat の
// ファイル先頭 = $FE0000):
//     FF9C8A: 13C1 00E90001   MOVE.B D1,$E90001   ; レジスタ番号
//     FF9C90: 610A            BSR   $FF9C9C       ; BUSY 待ち
//     FF9C94: 13C0 00E90003   MOVE.B D0,$E90003   ; 値
//   BUSY 待ちの実体 ($FF9C9C):
//     FF9C9C: 4A39 00E90003   TST.B $E90003
//     FF9CA2: 6BF8            BMI.S $FF9C9C       ; bit7 が立っている間まわる
// このループにタイムアウトは無い。ステータスの bit7 を落とし続けないと
// 起動がここで永久に止まる。ステータスを読むのが $E90001 ではなく $E90003 で
// あることもここで確定する。
//
// 【実装範囲と近似】
// 実装したもの:
//   - 4 オペレータ × 8ch、エンベロープ (DR1/D1R/D2R/RR = ADSR)
//   - 8 種のコネクション (アルゴリズム) 全部
//   - キーオン/キーオフ (レジスタ $08 のスロットマスク)
//   - KC/KF による音程、DT1/DT2/MUL によるデチューンと逓倍
//   - KS (キースケーリング) によるレート補正、TL (トータルレベル)
//   - フィードバック (ch ごと、オペレータ M1 の自己変調)
// 近似したもの (Why not は各実装箇所に書いた):
//   - LFO (AMS/PMS/波形) は未実装。無変調として扱う。
//   - ノイズ (レジスタ $0F, ch8 の C2) は未実装。
//   - タイマ A/B は数えるが割り込み線は張っていない。
//   - オペレータの実行順・パイプライン段数は再現しない。1 サンプルぶんを
//     一括で計算する。
//
// 【出力レート】
// 実チップは φM 3.579545MHz を 64 分周した 55.9kHz でスロットを回すが、
// ここでは合成レートをホスト側が選べる形にし、既定を 15625Hz にする。
// 理由は render の実装コメントに書いた。

#ifndef X68K_CORE_DEV_OPM_H
#define X68K_CORE_DEV_OPM_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../cpu/m68k_types.h"

namespace x68k
{

class Opm
{
public:
    static constexpr u32 kChannelCount = 8;
    static constexpr u32 kOperatorsPerChannel = 4;
    static constexpr u32 kRegCount = 256;
    static constexpr u32 kOutputGainUnityQ8 = 256;
    static constexpr u32 kOutputGainMaxQ8 = 2048;
    static constexpr u32 kOutputPeakLimitMax = 32767;

    // 既定の合成レート。
    //
    // 実チップの内部スロットレートは φM(3.579545MHz)/64 = 55.9kHz。
    // ESP32-S3 で 8ch × 4op を 55.9kHz で回すのは重すぎるので落とすが、
    // 15625Hz では足りない。
    //
    // Why not 15625Hz (ADPCM と同じ値):
    // ナイキストが 7812Hz しかなく、ゲームの主旋律 (約1568Hz, KC=96) では
    // **第4倍音までしか収まらない**。第5倍音以降は折り返して基音と無関係な
    // 位置に現れる。FM の変調を深くするほど高次倍音が増えるので、深くする
    // ほど非調和な成分が増えて濁る。実機で「FM音源に聞こえない、PSGに
    // 聞こえる」と言われた原因がこれだった。
    //
    // 同じ patch で測ると (KC=96、基音1568Hz):
    //   15625Hz : 2nd=123%  3rd= 71%  4th=11%  5th= 3.7%
    //   31250Hz : 2nd=158%  3rd=141%  4th=30%  5th= 7.7%
    // 金管の「輝き」は高次倍音なので、レートが足りないと原理的に出せない。
    //
    // 31250Hz でも足りなかった。1568Hz を 31250Hz で鳴らすと 1 周期あたり
    // **約20点**しか無く、波形が階段状のまま残る。実際に同じ patch を
    // 15625/31250/55930Hz で鳴らして聴き比べ、一番高いレートが明らかに
    // 良いと確認した。倍音が収まるかどうかだけでなく、1 周期を何点で
    // 描けるかが効く。
    //
    //   15625Hz : 10.0 点/周期
    //   31250Hz : 19.9 点/周期
    //   62500Hz : 39.9 点/周期  <- これを採る
    //
    // 62500Hz は 160 cycles/sample で、ゲスト 10MHz との積 (10^7) を割り切る。
    // 実チップの内部レート 55930Hz より高いが、割り切れる値の方が
    // ゲスト時間とのずれが出ない。
    //
    // 合成コストはホスト実測で 15625Hz の約1.36倍 (3ch・1秒ぶんで
    // 4.37ms -> 5.95ms)。レートは4倍だが1サンプルあたりの仕事が減るので
    // 比例はしない。50000Hz より速いのは 10^7 を割り切れるため。
    //
    // ADPCM は実機どおり 15.625kHz のまま。4:1 の整数比なので変換は単純。
    //
    // Why not 実チップと同じ 55.9kHz にするか: エンベロープの進み方は
    // 「1 サンプルあたり何ステップ」で決まる。レートを変えてもエンベロープ
    // 時間が変わらないよう、レート比を EG の歩進に掛けて補正する
    // (envelopeStepScale_)。これで実時間としての ADSR は保たれる。
    static constexpr u32 kDefaultSampleRate = 62500;

    // 実チップの内部レート。エンベロープと位相の基準。
    static constexpr u32 kChipRate = 55930;

    Opm();

    void reset();

    // 合成レートを変える。エンベロープの実時間を保つよう内部で補正する。
    void setSampleRate(u32 rate);

    [[nodiscard]] u32 sampleRate() const
    {
        return sampleRate_;
    }

    // レジスタ音色を変えずにホスト側だけで調整するため、reset 後も保持する。
    // 所有コアだけで設定/読取する。無効 ch は変更せず false、gain は上限で止める。
    bool setChannelOutputGainQ8(u32 channel, u32 gain);
    // 無効 ch の参照を末尾へ折り返さない。既定の unity を返す。
    [[nodiscard]] u32 channelOutputGainQ8(u32 channel) const;

    // 増幅後のピークだけを抑えるホスト設定。0 は無効、reset 後も保持する。
    // ゲストの音色を一律変更しないため、既定は全 ch 無効。
    bool setChannelOutputPeakLimit(u32 channel, u32 limit);
    [[nodiscard]] u32 channelOutputPeakLimit(u32 channel) const;

    // --- CPU から見える口 ($E90001 / $E90003) ---

    // $E90001 への書き込み。次に書く値の宛先レジスタ番号を latch する。
    void writeAddress(u8 reg);

    // $E90003 への書き込み。latch 済みのレジスタへ値を入れる。
    void writeData(u8 value);

    // $E90003 の読み出し。bit7 = BUSY, bit1 = タイマ B, bit0 = タイマ A。
    //
    // BUSY は常に 0 を返す。理由は実装のコメントに書いた。
    [[nodiscard]] u8 readStatus() const;

    // レジスタの読み返し。実チップにこの口は無い (書き込み専用) が、
    // テストが「書いた値が効いているか」を確かめるのに要る。
    [[nodiscard]] u8 peekRegister(u8 reg) const
    {
        return regs_[reg];
    }

    // 現在 latch されているレジスタ番号。
    [[nodiscard]] u8 latchedAddress() const
    {
        return address_;
    }

    // --- サンプル生成 ---

    // frames サンプルぶんモノラルで合成して out に書く。
    //
    // Why not ステレオにするか: OPM は ch ごとに L/R の on/off しか持たない
    // (レジスタ $20 の bit7/bit6)。パンを活かすなら 2ch 出力が要るが、
    // まずモノラルで鳴らすことを優先する。パン情報は保持しているので、
    // ステレオ化するときは L/R を別々に足し込むだけで済む。
    void renderSamples(std::int16_t* out, std::size_t frames);

    // 1 サンプルだけ合成する。テストとデバッグ用。
    [[nodiscard]] std::int16_t renderOneSample();

    // 鳴っている音が 1 つも無い (全スロットがエンベロープ最小) か。
    // 実機には無い問い合わせだが、音を出す必要があるかを platform 層が
    // 判断するのに要る (無音なら合成ごと省ける)。
    [[nodiscard]] bool isSilent() const;

    // --- 内部状態の観測 (テスト用) ---

    // エンベロープの現在値。0 が最大音量、1023 が無音 (実チップと同じ向き)。
    [[nodiscard]] u32 envelopeLevel(u32 channel, u32 op) const;

    // エンベロープのフェーズ。
    enum class EgPhase : u8
    {
        kAttack,
        kDecay,    // D1R。D1L に達するまで
        kSustain,  // D2R。以降ずっと減衰
        kRelease,  // RR
        kOff,      // 完全に減衰しきってキーオフ済み
    };
    [[nodiscard]] EgPhase envelopePhase(u32 channel, u32 op) const;

    // ch がキーオンされているか (どれかのスロットが on)。
    [[nodiscard]] bool isKeyOn(u32 channel) const;

    struct OutputStats
    {
        struct ChannelOutput
        {
            u32 peak = 0;
            u32 nonzero = 0;
            std::uint64_t squares = 0;
        };
        u32 samples = 0;
        std::array<ChannelOutput, kChannelCount> channels{};
    };
    // 所有コアだけで設定/読取する。再合成して測ると音源時間を二重に進めてしまう。
    void setOutputStats(OutputStats* stats)
    {
        outputStats_ = stats;
    }

private:
    OutputStats* outputStats_ = nullptr;
    struct Operator
    {
        // --- レジスタから写した値 ---
        u32 detune1 = 0;         // DT1 (0-7)
        u32 multiple = 0;        // MUL (0-15)。0 は 0.5 倍を意味する
        u32 totalLevel = 0;      // TL (0-127)。大きいほど小さい音
        u32 keyScale = 0;        // KS (0-3)。エンベロープのレート補正の強さ
        u32 attackRate = 0;      // AR (0-31)
        u32 decay1Rate = 0;      // D1R (0-31)
        u32 decay2Rate = 0;      // D2R (0-31)
        u32 releaseRate = 0;     // RR (0-15)。実効は RR*2+1
        u32 decay1Level = 0;     // D1L (0-15)。減衰の折り返し点
        u32 detune2 = 0;         // DT2 (0-3)
        bool amsEnable = false;  // AMS-EN。LFO 未実装なので保持のみ

        // --- 実行時の状態 ---
        u32 phase = 0;  // 位相アキュムレータ (10.10 固定小数の上位)
        // 1 サンプルあたりの位相増分。KC/KF/DT1/DT2/MUL とサンプルレートから
        // 決まるのでキャッシュする。毎サンプル計算すると std::pow が
        // 1 サンプルに 32 回走り、実測で 70 倍遅くなる
        // (updateOperatorPhaseStep の "Why not" を見よ)。
        u32 phaseStep = 0;
        u32 envelope = kEnvelopeMax;  // 0 = 最大音量, 1023 = 無音

        EgPhase egPhase = EgPhase::kOff;
        bool keyOn = false;
        // 直前 2 サンプルの出力。フィードバックの平均に使う。
        std::int32_t prevOut[2] = {0, 0};
        // エンベロープの歩進を貯める端数 (レート変換ぶんの補正を含む)。
        u32 egCounter = 0;
    };

    struct Channel
    {
        std::array<Operator, kOperatorsPerChannel> ops{};
        u32 keyCode = 0;      // KC (レジスタ $28)。オクターブ + 音名
        u32 keyFraction = 0;  // KF (レジスタ $30)。KC の 1/64 刻みの微調整
        u32 connection = 0;   // CONN (0-7)
        u32 feedback = 0;     // FL (0-7)
        bool leftOn = true;
        bool rightOn = true;
    };

    void applyRegister(u8 reg, u8 value);
    void writeKeyOn(u8 value);
    void updateOperatorPhaseStep(Channel& ch, Operator& op);
    void advanceEnvelope(Channel& ch, Operator& op);
    [[nodiscard]] std::int32_t renderChannel(Channel& ch);

    // ch に鳴っている (あるいはこれから鳴る) スロットがあるか。
    // isSilent と renderOneSample の ch 飛ばしで同じ判定を使う。
    [[nodiscard]] static bool isChannelSounding(const Channel& ch);

    // エンベロープの実効レートを求める。KS によるキースケーリングを含む。
    [[nodiscard]] static u32 effectiveRate(u32 rate, u32 keyScale, u32 keyCode);

    static constexpr u32 kEnvelopeMax = 1023;  // 無音
    // エンベロープの内部分解能。実チップの EG は 10bit だが、
    // 減衰を滑らかに扱うため 1/16 ステップの内部カウンタを持つ。
    static constexpr u32 kEnvelopeShift = 4;

    std::array<Channel, kChannelCount> channels_{};
    std::array<u32, kChannelCount> channelOutputGainsQ8_{};
    std::array<u32, kChannelCount> channelOutputPeakLimits_{};
    std::array<u8, kRegCount> regs_{};
    u8 address_ = 0;

    u32 sampleRate_ = kDefaultSampleRate;
    // 実チップレートに対する合成レートの比。EG と位相の歩進に掛ける。
    // 15625Hz なら約 3.58 倍 (1 サンプルで実チップ 3.58 ステップぶん進む)。
    u32 stepScaleQ16_ = 0;

    // タイマ A/B。数えるだけで割り込み線は張っていない。
    u32 timerACount_ = 0;
    u32 timerBCount_ = 0;
};

}  // namespace x68k

#endif  // X68K_CORE_DEV_OPM_H
