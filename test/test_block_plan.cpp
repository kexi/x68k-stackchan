// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// BlockPlanner が組む計画が、実際に M68k を走らせた結果と一致すること。
//
// このファイルが守るのは速さではない。ブロックへ入れた命令は、実行器が
// 「例外に入らない」「メモリを触らない」という契約のもとで走る。契約を
// 破る命令が 1 つでも紛れ込むと、失敗は例外フレームの中身や halt という
// **原因から最も遠いところ**で表面化する。だから許可リストの内側と外側を
// 65,536 通り全部で問う。
//
// 翻訳器は入力が u16 と平坦な配列だけで閉じているので (block_planner.h の
// 冒頭を見よ)、Machine を立てずに全数を回せる。突き合わせ相手として
// M68k::step() を回すときだけ Machine を立てる。
//
// 各テストには「これを壊すと落ちる変異」を書いてある。落ちない変異しか
// 書けないテストは価値が無い。

#include <cstring>
#include <vector>

#include "cpu/block_plan.h"
#include "cpu/block_planner.h"
#include "cpu/code_gen_map.h"
#include "cpu/m68k_length.h"
#include "machine.h"
#include "doctest.h"

namespace
{

// 翻訳器へ渡す平坦なコードメモリ。
//
// Why not Machine の RAM をそのまま読ませないか: 翻訳器が Bus を握ると
// 「窓の外」を作れなくなり、I3 (kWindowExit) を起こす手段が消える。
// 窓の内外をテストが決められる形にしておく。
struct FlatCode
{
    // 窓は [begin, end)。この外を読もうとしたら read16 が false を返す。
    x68k::u32 begin = 0;
    x68k::u32 end = 0;
    std::vector<x68k::u8> bytes;  // begin からの生バイト

    void reset(x68k::u32 windowBegin, x68k::u32 windowEnd)
    {
        begin = windowBegin;
        end = windowEnd;
        bytes.assign(windowEnd - windowBegin, 0);
    }

    void poke16(x68k::u32 addr, x68k::u16 v)
    {
        REQUIRE(addr >= begin);
        REQUIRE(addr + 1 < end);
        bytes[addr - begin] = static_cast<x68k::u8>(v >> 8);
        bytes[addr + 1 - begin] = static_cast<x68k::u8>(v & 0xFFu);
    }

    static bool read16(void* ctx, x68k::u32 addr, x68k::u16& out)
    {
        const auto* self = static_cast<const FlatCode*>(ctx);
        if (addr < self->begin || addr + 1 >= self->end)
        {
            return false;
        }
        const std::size_t i = addr - self->begin;
        out = static_cast<x68k::u16>((static_cast<x68k::u16>(self->bytes[i]) << 8) |
                                     static_cast<x68k::u16>(self->bytes[i + 1]));
        return true;
    }

    x68k::PlanSource source()
    {
        return x68k::PlanSource{&FlatCode::read16, this};
    }
};

// 偽のページ世代。飽和 (kAlwaysStale) を任意に作れることが要点。
struct FakeGen
{
    x68k::u16 value = 1;
    x68k::u32 epoch = 0x1234;

    static x68k::u16 generation(void* ctx, x68k::u32)
    {
        return static_cast<const FakeGen*>(ctx)->value;
    }
    static x68k::u32 mappingEpoch(void* ctx)
    {
        return static_cast<const FakeGen*>(ctx)->epoch;
    }

    x68k::PlanGenSource source()
    {
        return x68k::PlanGenSource{&FakeGen::generation, &FakeGen::mappingEpoch, this};
    }
};

// --- 仕様側から書いた「安全な命令」の定義 -----------------------------------
//
// **実装 (planOne) を読んで書いてはいけない。** 実装を写すと、実装の誤りを
// そのまま期待値にしたテストになる。68000 の符号化から、「例外に入らない
// かつメモリを触らない」命令の集合を独立に組み立てて、それと突き合わせる。
//
// ここに入れてよいのは、次を全部満たす形だけ:
//   - 不当命令・特権違反・トラップ・ゼロ除算に入らない
//   - A レジスタもスタックも触らない (BSR / MOVEA / ADDA)
//   - メモリに触るなら **読むだけ** で、実効アドレスが Tier B の
//     許可 EA に入っている (下の specReadEa)
//
// **Tier B でメモリ読みが入った。** Tier A までは「実効アドレスが
// メモリを指さない」ことが条件だったが、読み形は実行時ガードが
// 「窓の中か」を確かめ、外れたら 1 bit も状態を変えずに step() へ
// 降りる。だから「バスエラー / アドレスエラーが起きない」という
// 性質は、静的な EA の制限ではなくガードが担うようになっている。
// 書き方向は依然として入らない (ページ世代の更新を背負う)。

// Tier B が読み形として受ける実効アドレス。
//
// 68000 の符号化から直接:
//   mode 2 (An) / 3 (An)+ / 4 -(An)  拡張ワード無し
//   mode 5 (d16,An)                  1 ワード
//   mode 7 reg 0 (xxx).W             1 ワード
//   mode 7 reg 1 (xxx).L             2 ワード
//
// mode 6 (d8,An,Xn) と 7.2/7.3 (PC 相対) は入れない。前者は拡張ワードの
// インデックス解釈が要り、後者は翻訳時の PC に依存する。
// mode 7.4 (即値) はメモリを読まないので、ここではなく Tier A の
// 即値形が持つ。
bool specReadEa(x68k::u32 mode, x68k::u32 reg)
{
    if (mode >= 2 && mode <= 5)
    {
        return true;
    }
    return mode == 7 && reg <= 1;
}

// MOVE.b/w/l Dn,Dm。
bool specSafeMove(x68k::u16 op)
{
    const x68k::u32 group = static_cast<x68k::u32>(op >> 12);
    if (group != 1 && group != 2 && group != 3)
    {
        return false;
    }
    const x68k::u32 srcMode = static_cast<x68k::u32>((op >> 3) & 7u);
    const x68k::u32 srcReg = static_cast<x68k::u32>(op & 7u);
    const x68k::u32 dstMode = static_cast<x68k::u32>((op >> 6) & 7u);
    const x68k::u32 size = group == 1 ? 1u : (group == 2 ? 4u : 2u);

    // 転送先は Dn か An。メモリへ書く形はアドレスエラーとページ世代の
    // 更新を背負うので安全ではない。
    if (dstMode > 1)
    {
        return false;
    }
    // 転送元は Dn / An / 即値 / 読み形メモリ (Tier B)。
    //
    // 読み形は Dn 宛てだけ。An 宛て (MOVEA) は .w の符号拡張で本体が
    // 別物になるので、Tier B の初回スコープには入っていない。
    const bool srcIsImmediate = srcMode == 7 && srcReg == 4;
    const bool srcIsReadMemory = !srcIsImmediate && srcMode > 1 && specReadEa(srcMode, srcReg);
    if (srcIsReadMemory)
    {
        return dstMode == 0;
    }
    if (srcMode > 1 && !srcIsImmediate)
    {
        return false;
    }
    // byte で An に触る形は 68000 に無い (不当命令として例外に入る)。
    const bool touchesAddressRegisterAsByte = size == 1 && (srcMode == 1 || dstMode == 1);
    return !touchesAddressRegisterAsByte;
}

// $4 のうち、メモリに触れず例外も起きない形。
//
// LEA は実効アドレスを**求めるだけで読まない**ので、メモリに触らない。
// TST / CLR は mode 0 (Dn) に限れば同じ。
bool specSafeMisc(x68k::u16 op)
{
    if ((op >> 12) != 0x4u)
    {
        return false;
    }
    const x68k::u32 mode = static_cast<x68k::u32>((op >> 3) & 7u);
    const x68k::u32 reg = static_cast<x68k::u32>(op & 7u);

    const bool isLea = (op & 0xF1C0u) == 0x41C0u;
    if (isLea)
    {
        // mode 3/4 は An を進める副作用を持つ。mode 6 と 7.2/7.3 は
        // 安全だが拡張ワードの解釈が要るので範囲外。
        return mode == 2 || mode == 5 || (mode == 7 && reg <= 1);
    }

    // 単項演算は (op >> 8) & 0xF で判別する (実装の unary_ops と同じ)。
    const x68k::u32 opcodeBits = static_cast<x68k::u32>((op >> 8) & 0xFu);
    const x68k::u32 sizeField = static_cast<x68k::u32>((op >> 6) & 3u);
    if (sizeField == 3)
    {
        return false;
    }
    if (mode != 0)
    {
        // TST <mem> は読むだけ (Tier B)。CLR <mem> は読んで書く RMW なので
        // 入らない。
        return opcodeBits == 0xAu && specReadEa(mode, reg);
    }
    return opcodeBits == 0xAu || opcodeBits == 0x2u;  // TST / CLR
}

// MOVEQ #imm8,Dn。bit8 が立つ符号は 68000 に無い。
bool specSafeMoveq(x68k::u16 op)
{
    return (op >> 12) == 0x7u && (op & 0x0100u) == 0;
}

// ADD/SUB/AND/OR/CMP の <Dn>,Dm 方向。
//
// **EOR がここに無いのは意図的。** EOR Dn,Dn は $B のメモリ方向 (opmode 4-6)
// にしか存在せず、そこは CMPM や ADDX/SBCD と符号が重なる領域なので、
// 命令長デコーダが長さを返さない。長さが無い命令はブロックへ入れられない
// (I1)。EOR をここに足すと「許可リストは受けるのに I1 で必ず落ちる」形に
// なり、テストは緑のまま実効的に死んだ枝ができる。
bool specSafeAlu(x68k::u16 op)
{
    const x68k::u32 group = static_cast<x68k::u32>(op >> 12);
    const bool isAluGroup =
        group == 0x8u || group == 0x9u || group == 0xBu || group == 0xCu || group == 0xDu;
    if (!isAluGroup)
    {
        return false;
    }
    const x68k::u32 opmode = static_cast<x68k::u32>((op >> 6) & 7u);
    const x68k::u32 mode = static_cast<x68k::u32>((op >> 3) & 7u);
    const x68k::u32 reg = static_cast<x68k::u32>(op & 7u);
    if (mode != 0)
    {
        // <mem>,Dn の読み方向だけ (Tier B)。opmode 3/7 は ADDA/SUBA/CMPA と
        // MULU/MULS/DIVU/DIVS、opmode 4-6 はメモリへ書く方向。
        return opmode <= 2 && specReadEa(mode, reg);
    }
    // opmode 3/7 は ADDA/SUBA/CMPA (A レジスタ) と MULU/MULS/DIVU/DIVS
    // (ゼロ除算)。opmode 4-6 は ADDX/SUBX/ABCD/SBCD/EOR/CMPM の特殊形。
    return opmode <= 2;
}

// Bcc / BRA。BSR (cond 1) は a[7] へ write32 するので入れない。
// 変位 $FF は 68020 以降の 32bit 変位。
bool specSafeBranch(x68k::u16 op)
{
    if ((op >> 12) != 0x6u)
    {
        return false;
    }
    const x68k::u32 cond = static_cast<x68k::u32>((op >> 8) & 0xFu);
    const x68k::u32 disp8 = static_cast<x68k::u32>(op & 0xFFu);
    return cond != 1 && disp8 != 0xFFu;
}

bool specSafe(x68k::u16 op)
{
    return specSafeMove(op) || specSafeMoveq(op) || specSafeAlu(op) || specSafeBranch(op) ||
           specSafeMisc(op);
}

// --- 実行して突き合わせるための Machine -------------------------------------

constexpr x68k::u32 kEntry = 0x1000;

std::vector<x68k::u8>& execRam()
{
    static std::vector<x68k::u8> storage(x68k::kMainRamSize, 0);
    return storage;
}

void ramPoke16(x68k::u32 a, x68k::u16 v)
{
    execRam()[a] = static_cast<x68k::u8>(v >> 8);
    execRam()[a + 1] = static_cast<x68k::u8>(v & 0xFFu);
}

// 1 命令を pc に置いて実行し、消費サイクルと次の PC を返す。
struct StepResult
{
    x68k::u32 cycles = 0;
    x68k::u32 nextPc = 0;
    bool halted = false;
};

// ccr: 実行前に CCR へ流し込む値。分岐の成立/不成立を切り替えるのに使う。
StepResult runOne(const std::vector<x68k::u16>& words, x68k::u16 ccr)
{
    std::fill(execRam().begin(), execRam().end(), 0);
    ramPoke16(0, 0x0000);
    ramPoke16(2, 0x8000);  // SSP
    ramPoke16(4, 0x0000);
    ramPoke16(6, kEntry);  // PC
    // 命令の後ろは NOP で埋める。分岐先が命令列の外へ出ても安全に止まる。
    for (x68k::u32 a = kEntry - 0x200; a < kEntry + 0x400; a += 2)
    {
        ramPoke16(a, 0x4E71);
    }
    x68k::u32 at = kEntry;
    for (const x68k::u16 w : words)
    {
        ramPoke16(at, w);
        at += 2;
    }

    x68k::Machine m;
    x68k::MemoryMap map{};
    map.mainRam = execRam().data();
    m.setMemory(map);
    m.reset();

    x68k::M68kState st = m.cpu().state();
    st.pc = kEntry;
    // S ビットのみ。CCR は引数で決める。
    st.sr = static_cast<x68k::u16>(0x2000u | (ccr & 0x1Fu));
    for (int i = 0; i < 8; ++i)
    {
        st.d[i] = 0x20;
        st.a[i] = 0x40;
    }
    st.a[7] = 0x8000;
    m.cpu().loadStateForTest(st);
    m.cpu().refillPrefetchForTest(kEntry);

    StepResult r{};
    r.cycles = m.step();
    r.halted = m.isHalted();
    // 実行後の PC は「次の命令語 + 4」。
    r.nextPc = m.cpu().state().pc - 4;
    return r;
}

}  // namespace

TEST_SUITE("BlockPlanner")
{
    // 許可リストの内側と外側が、68000 の符号化から独立に組んだ定義と
    // 完全に一致すること。
    //
    // **これが段 1 の正しさの中心。** 許可リストは「例外に入らない」
    // 「メモリを触らない」という実行器の契約を静的に保証する唯一の仕組みで、
    // 穴が空いてもワークロードでは滅多に踏まない。全数でしか確実に捕まらない。
    //
    // 落ちる変異:
    //   - planMove の mode 検査を落とす (MOVE.b An,Dn が入り、不当命令例外)
    //   - planBranch の cond == 1 検査を落とす (BSR が入り、スタックへ書く)
    //   - planAlu の opmode 3/7 検査を落とす (DIVU が入り、ゼロ除算例外)
    //   - planAlu の mode 検査を落とす (ADD (An),Dn が入り、バスを読む)
    TEST_CASE("planOne の許可リストが符号化から組んだ安全な集合と一致する")
    {
        std::size_t accepted = 0;
        std::size_t falseAccept = 0;
        std::size_t falseReject = 0;
        x68k::u16 firstFalseAccept = 0;
        x68k::u16 firstFalseReject = 0;

        for (x68k::u32 opv = 0; opv < 0x10000; ++opv)
        {
            const auto op = static_cast<x68k::u16>(opv);
            x68k::PlannedOp planned{};
            const bool got = x68k::BlockPlanner::planOne(op, kEntry, planned);
            const bool want = specSafe(op);

            if (got)
            {
                ++accepted;
            }
            if (got && !want)
            {
                if (falseAccept == 0)
                {
                    firstFalseAccept = op;
                }
                ++falseAccept;
            }
            if (!got && want)
            {
                if (falseReject == 0)
                {
                    firstFalseReject = op;
                }
                ++falseReject;
            }
        }

        CAPTURE(firstFalseAccept);
        CAPTURE(firstFalseReject);
        // 許可リストの外が漏れて入っていないこと。
        CHECK(falseAccept == 0);
        // 安全な命令を取りこぼしていないこと (取りこぼしは速度にしか
        // 現れないので、明示的に数える)。
        CHECK(falseReject == 0);
        // 素通りしていないこと。
        CAPTURE(accepted);
        CHECK(accepted > 3000);
    }

    // 許可リストに入った命令が、実際に実行しても例外にも halt にも
    // 入らないこと。
    //
    // **上のテストだけでは不十分。** 上は「テストが書いた仕様」と
    // 「実装」を突き合わせているだけなので、両方が同じ誤りをしていると
    // 素通りする。実際に走らせて、CPU が生きたまま次の命令へ進むことを
    // 68000 の実装そのものに問う。
    //
    // 落ちる変異: 許可リストへ例外を起こす形 (TRAP / CHK / DIVU) を足す
    TEST_CASE("許可リストの命令は実行しても例外にも halt にも入らない")
    {
        std::size_t executed = 0;
        std::size_t halted = 0;
        x68k::u16 firstHalt = 0;

        for (x68k::u32 opv = 0; opv < 0x10000; ++opv)
        {
            const auto op = static_cast<x68k::u16>(opv);
            x68k::PlannedOp planned{};
            if (!x68k::BlockPlanner::planOne(op, kEntry, planned))
            {
                continue;
            }
            // 拡張ワードは 0 (Bcc.w なら「次の命令へ」の変位)。
            const StepResult r = runOne({op, 0x0000}, 0);
            ++executed;
            if (r.halted)
            {
                if (halted == 0)
                {
                    firstHalt = op;
                }
                ++halted;
            }
        }

        CAPTURE(firstHalt);
        CHECK(halted == 0);
        CAPTURE(executed);
        CHECK(executed > 3000);
    }

    // 計画したサイクル数が、実際に step() を回したときの戻り値と一致すること。
    //
    // 分岐は成立側と不成立側の**両方**を回す。Bcc.b は不成立 8 / 成立 10、
    // Bcc.w は不成立 12 / 成立 10 で **符号が逆になる**ので、片方だけ
    // 見ていると「成立なら +2」という誤った実装が通ってしまう。
    //
    // 落ちる変異:
    //   - Bcc.w の不成立側を 8 にする (12 が要る)
    //   - 成立側を「不成立 + 2」で導く (Bcc.w で 4 ずれる)
    //   - MOVE / MOVEQ / ALU の cycles を 4 以外にする
    TEST_CASE("計画のサイクル数が実行と一致する")
    {
        std::size_t checked = 0;
        std::size_t mismatched = 0;
        x68k::u16 firstBad = 0;
        x68k::u32 firstWant = 0;
        x68k::u32 firstGot = 0;

        FlatCode code;
        FakeGen gen;

        for (x68k::u32 opv = 0; opv < 0x10000; ++opv)
        {
            const auto op = static_cast<x68k::u16>(opv);
            x68k::PlannedOp probe{};
            if (!x68k::BlockPlanner::planOne(op, kEntry, probe))
            {
                continue;
            }
            if (x68k::instructionLength(op) == x68k::kUnknownLength)
            {
                continue;  // I1 が弾くのでブロックには入らない
            }

            // 1 命令だけのブロックを組む。後続は必ず終端する命令 (NOP) にして、
            // 計画のサイクル合計が「この 1 命令ぶん」になるようにする。
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, op);
            code.poke16(kEntry + 2, 0x0000);  // Bcc.w の変位 = 次の命令へ
            code.poke16(kEntry + 4, 0x4E71);  // NOP。許可リストの外なので必ず切れる
            code.poke16(kEntry + 6, 0x4E71);

            x68k::BlockPlan plan{};
            const auto src = code.source();
            const auto gsrc = gen.source();
            if (!x68k::BlockPlanner::plan(src, gsrc, kEntry, plan))
            {
                continue;
            }
            REQUIRE(plan.count >= 1);
            if (plan.count != 1)
            {
                continue;  // 後続まで積んだ形は、この検査の対象外
            }

            const bool isBranch = plan.end == x68k::BlockEnd::kBranch;

            // 不成立側 (分岐でなければこちらだけ)。
            // CCR = 0 なら BEQ/BCS/BMI/BVS などが不成立になる。
            const StepResult notTaken = runOne({op, 0x0000}, 0x00);
            // 成立側。CCR = 0x1F (全ビット立ち) で逆側の条件が成立する。
            const StepResult taken = runOne({op, 0x0000}, 0x1F);

            if (!isBranch)
            {
                ++checked;
                if (notTaken.cycles != plan.cyclesNotTaken)
                {
                    if (mismatched == 0)
                    {
                        firstBad = op;
                        firstWant = notTaken.cycles;
                        firstGot = plan.cyclesNotTaken;
                    }
                    ++mismatched;
                }
                continue;
            }

            // 分岐は「実際に飛んだ方」を成立側として判定する。条件コード
            // ごとにどちらの CCR で成立するかが違うので、PC の行き先で見る。
            const x68k::u32 fallThrough = plan.fallThroughPc;
            const StepResult* takenRun = nullptr;
            const StepResult* notTakenRun = nullptr;
            if (notTaken.nextPc == fallThrough)
            {
                notTakenRun = &notTaken;
            }
            else
            {
                takenRun = &notTaken;
            }
            if (taken.nextPc == fallThrough)
            {
                notTakenRun = &taken;
            }
            else
            {
                takenRun = &taken;
            }

            if (notTakenRun != nullptr)
            {
                ++checked;
                if (notTakenRun->cycles != plan.cyclesNotTaken)
                {
                    if (mismatched == 0)
                    {
                        firstBad = op;
                        firstWant = notTakenRun->cycles;
                        firstGot = plan.cyclesNotTaken;
                    }
                    ++mismatched;
                }
            }
            if (takenRun != nullptr)
            {
                ++checked;
                if (takenRun->cycles != plan.cyclesTaken)
                {
                    if (mismatched == 0)
                    {
                        firstBad = op;
                        firstWant = takenRun->cycles;
                        firstGot = plan.cyclesTaken;
                    }
                    ++mismatched;
                }
            }
        }

        CAPTURE(firstBad);
        CAPTURE(firstWant);
        CAPTURE(firstGot);
        CHECK(mismatched == 0);
        CAPTURE(checked);
        CHECK(checked > 3000);
    }

    // 計画した命令長と分岐先が、実際に実行した PC の進み方と一致すること。
    //
    // 長さが 1 でも違うと 2 命令目以降の位置が全部ずれ、ブロックが
    // 「別の命令列」を実行する。分岐先は静的に決まる (変位は即値) ので、
    // 実行結果と突き合わせられる。
    //
    // 落ちる変異:
    //   - planBranch の base を pc + 2 から pc + 4 にする
    //   - Bcc.w の変位を符号拡張しない
    //   - PlannedOp::length に instructionLength の値を入れ忘れる
    TEST_CASE("計画の命令長と分岐先が実行と一致する")
    {
        std::size_t checkedLength = 0;
        std::size_t checkedTarget = 0;
        std::size_t badLength = 0;
        std::size_t badTarget = 0;
        x68k::u16 firstBadLength = 0;
        x68k::u16 firstBadTarget = 0;

        FlatCode code;
        FakeGen gen;

        for (x68k::u32 opv = 0; opv < 0x10000; ++opv)
        {
            const auto op = static_cast<x68k::u16>(opv);
            x68k::PlannedOp probe{};
            if (!x68k::BlockPlanner::planOne(op, kEntry, probe))
            {
                continue;
            }

            // 拡張ワードは **偶数の実効アドレス**を作る値にする。
            //
            // Why not 0x0000 と NOP 埋めのままにしないか: 読み形 (Tier B) が
            // 入ったので、(xxx).L の 2 語が {0x0000, 0x4E71} だと実効アドレスが
            // $00004E71 (奇数) になり、word / long の読みが**本物の
            // アドレスエラー**に入る。すると PC は例外ベクタへ飛び、
            // 「fallThroughPc へ来ない」= 命令長が違う、と読めてしまう。
            // 落ちている 98 件は全部これで、命令長デコーダは正しかった。
            //
            // ここで見たいのは「計画した長さのぶん PC が進むか」なので、
            // 例外に入らない値を置いて長さだけを問う。ガードが奇数を弾く
            // ことは test_block_emitter.cpp が別に確かめる。
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, op);
            code.poke16(kEntry + 2, 0x0000);
            code.poke16(kEntry + 4, 0x0100);
            code.poke16(kEntry + 6, 0x4E71);

            x68k::BlockPlan plan{};
            const auto src = code.source();
            const auto gsrc = gen.source();
            if (!x68k::BlockPlanner::plan(src, gsrc, kEntry, plan))
            {
                continue;
            }
            if (plan.count != 1)
            {
                continue;
            }

            // **code と同じ語を置く。** 片方だけ変えると、計画した命令と
            // 実行した命令が違うものになる。
            const StepResult notTaken = runOne({op, 0x0000, 0x0100}, 0x00);
            const StepResult taken = runOne({op, 0x0000, 0x0100}, 0x1F);
            const bool isBranch = plan.end == x68k::BlockEnd::kBranch;

            // 不成立 (または非分岐) なら PC は fallThroughPc へ進む。
            const StepResult* fell = nullptr;
            const StepResult* jumped = nullptr;
            for (const StepResult* r : {&notTaken, &taken})
            {
                if (r->nextPc == plan.fallThroughPc)
                {
                    fell = r;
                }
                else
                {
                    jumped = r;
                }
            }

            if (fell != nullptr)
            {
                ++checkedLength;
                // fallThroughPc は entryPc + length。
                if (plan.fallThroughPc != kEntry + plan.ops[0].length)
                {
                    if (badLength == 0)
                    {
                        firstBadLength = op;
                    }
                    ++badLength;
                }
            }
            else if (!isBranch)
            {
                // 非分岐なのに PC が fallThroughPc へ来ないのは、計画が
                // 想定していない制御移動が起きたということ。
                ++badLength;
                if (firstBadLength == 0)
                {
                    firstBadLength = op;
                }
            }

            if (isBranch && jumped != nullptr)
            {
                ++checkedTarget;
                if (jumped->nextPc != plan.branchTarget)
                {
                    if (badTarget == 0)
                    {
                        firstBadTarget = op;
                    }
                    ++badTarget;
                }
            }
        }

        CAPTURE(firstBadLength);
        CHECK(badLength == 0);
        CAPTURE(firstBadTarget);
        CHECK(badTarget == 0);
        CAPTURE(checkedLength);
        CHECK(checkedLength > 3000);
        // 分岐が実際に飛ぶケースを見ていること。BRA と、成立する Bcc。
        CAPTURE(checkedTarget);
        CHECK(checkedTarget > 100);
    }

    // I9: ページ世代が kAlwaysStale なら、翻訳そのものを諦めること。
    //
    // **未配線と飽和は「常に有効」に化けやすい。** 控えた世代と現在の世代を
    // 素朴に比べると 0xFFFF == 0xFFFF で一致してしまい、自己書き換えを
    // 一切検出しないブロックキャッシュが成立する。作った瞬間に必ず外れると
    // 分かっている計画は、そもそも作らない。
    //
    // 落ちる変異: plan の入口から kAlwaysStale の検査を消す
    TEST_CASE("世代が kAlwaysStale なら計画を作らない")
    {
        FlatCode code;
        code.reset(0x0800, 0x1800);
        // 確実に翻訳できる命令を 2 つ並べる (MOVEQ #0,D0 と BRA)。
        code.poke16(kEntry, 0x7000);
        code.poke16(kEntry + 2, 0x6002);
        code.poke16(kEntry + 4, 0x4E71);
        code.poke16(kEntry + 6, 0x4E71);

        FakeGen gen;
        const auto src = code.source();

        SUBCASE("通常の世代なら計画できる")
        {
            gen.value = 3;
            const auto gsrc = gen.source();
            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count >= 1);
            CHECK(plan.pageGen == 3);
            CHECK(plan.mappingEpoch == gen.epoch);
            CHECK(plan.entryPc == kEntry);
            CHECK(plan.page == kEntry / x68k::CodeGenMap::kPageSize);
        }

        SUBCASE("飽和した世代なら計画しない")
        {
            gen.value = x68k::CodeGenMap::kAlwaysStale;
            const auto gsrc = gen.source();
            x68k::BlockPlan plan{};
            CHECK_FALSE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
        }
    }

    // I4: entryPc から fallThroughPc + 2 までが同一 1KB ページに収まること。
    //
    // **+2 が要るのが要点。** 出口の CPU 状態には irc == mem16(次の命令 + 2)
    // が含まれるので、次の命令のアドレスまでしか見ないと、その先の 2 バイトが
    // ページの外へ出る。fallThroughPc だけを見る実装でも通常のケースは全部
    // 通るので、ページ末尾ぴったりのケースを作らないと踏めない。
    //
    // 落ちる変異: pageOf(nextPc + 2) を pageOf(nextPc) に狭める
    //             → 下の SUBCASE で count が 1 増える
    TEST_CASE("ページ境界の手前で終端する")
    {
        FlatCode code;
        FakeGen gen;
        const auto gsrc = gen.source();

        // 1KB ページ [0x400, 0x800) の末尾に MOVEQ を並べる。
        //   0x7F8: MOVEQ  → 次は 0x7FA、irc は 0x7FC。ページ内。積める
        //   0x7FA: MOVEQ  → 次は 0x7FC、irc は 0x7FE。ページ内。積める
        //   0x7FC: MOVEQ  → 次は 0x7FE、irc は 0x800。**ページの外**。積めない
        SUBCASE("fallThroughPc + 2 がページを跨ぐ命令は積まない")
        {
            code.reset(0x0400, 0x0C00);
            for (x68k::u32 a = 0x7F8; a < 0xA00; a += 2)
            {
                code.poke16(a, 0x7000);  // MOVEQ #0,D0
            }
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, 0x7F8, plan));
            // 0x7F8 と 0x7FA の 2 命令だけ。0x7FC は irc が 0x800 になるので
            // 積めない。
            CHECK(plan.count == 2);
            CHECK(plan.end == x68k::BlockEnd::kPageBoundary);
            CHECK(plan.fallThroughPc == 0x7FC);
        }

        SUBCASE("ページの内側なら容量まで積む")
        {
            // 同じ命令列をページの先頭寄りに置けば、境界に当たらない。
            code.reset(0x0400, 0x0C00);
            for (x68k::u32 a = 0x400; a < 0x500; a += 2)
            {
                code.poke16(a, 0x7000);
            }
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, 0x400, plan));
            CHECK(plan.count == x68k::kMaxOps);
            CHECK(plan.end == x68k::BlockEnd::kCapacity);
            CHECK(plan.fallThroughPc == 0x400 + 2 * x68k::kMaxOps);
        }
    }

    // I7: 分岐先が奇数ならアドレスエラーに入るので、その分岐は積まない。
    //
    // 実行器は「例外に入らない」契約なので、静的に分かる例外源は静的に弾く。
    // 変位は即値なので飛び先は翻訳時に確定している。
    //
    // 落ちる変異: (target & 1) の検査を消す → 奇数分岐が積まれ、
    //             count が 1 増えて end が kBranch になる
    TEST_CASE("分岐先が奇数なら積まない")
    {
        FlatCode code;
        FakeGen gen;
        const auto gsrc = gen.source();

        SUBCASE("BRA.s の奇数変位")
        {
            code.reset(0x0800, 0x1800);
            // BRA.s +3 ($6003)。飛び先は (0x1000 + 2) + 3 = 0x1005 で奇数。
            code.poke16(kEntry, 0x6003);
            code.poke16(kEntry + 2, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            // 先頭の命令が積めないので計画そのものが立たない。
            CHECK_FALSE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
        }

        SUBCASE("先行する命令があれば、その手前で終端する")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x7000);      // MOVEQ #0,D0
            code.poke16(kEntry + 2, 0x6003);  // BRA.s → 0x1007 (奇数)
            code.poke16(kEntry + 4, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count == 1);
            CHECK(plan.end != x68k::BlockEnd::kBranch);
            CHECK(plan.fallThroughPc == kEntry + 2);
        }

        SUBCASE("偶数の分岐先なら積む")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x6002);  // BRA.s → (0x1000 + 2) + 2 = 0x1004
            code.poke16(kEntry + 2, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count == 1);
            CHECK(plan.end == x68k::BlockEnd::kBranch);
            CHECK(plan.branchTarget == 0x1004);
            CHECK(plan.fallThroughPc == 0x1002);
        }

        // BRA.w の奇数変位も同じに扱われること。8bit 変位だけ検査して
        // 拡張ワードを見ない実装を弾く。
        SUBCASE("BRA.w の奇数変位")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x6000);
            code.poke16(kEntry + 2, 0x0005);  // 飛び先 0x1007
            code.poke16(kEntry + 4, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            CHECK_FALSE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
        }
    }

    // I8: BSR はブロックへ入れない。
    //
    // BSR は復帰アドレスを a[7] へ write32 する。メモリを触るので、
    // 「メモリを触らない」契約が破れるだけでなく、a[7] が奇数のときに
    // アドレスエラーへ入る。**instructionLength は BSR にも長さを返す**ので、
    // I1 は守ってくれない。planOne が手で弾くしかない。
    //
    // 落ちる変異: planBranch の cond == 1 の検査を消す
    TEST_CASE("BSR はブロックに入らない")
    {
        // 単体で見ても弾かれること。
        for (x68k::u32 disp = 0; disp < 0x100; ++disp)
        {
            const auto op = static_cast<x68k::u16>(0x6100u | disp);
            CAPTURE(op);
            x68k::PlannedOp planned{};
            CHECK_FALSE(x68k::BlockPlanner::planOne(op, kEntry, planned));
        }
        // 命令長デコーダは BSR に長さを返す。つまり I1 では守れない。
        CHECK(x68k::instructionLength(0x6102) == 2);
        CHECK(x68k::instructionLength(0x6100) == 4);

        FlatCode code;
        FakeGen gen;
        const auto gsrc = gen.source();

        code.reset(0x0800, 0x1800);
        code.poke16(kEntry, 0x7000);      // MOVEQ #0,D0
        code.poke16(kEntry + 2, 0x6102);  // BSR.s
        code.poke16(kEntry + 4, 0x4E71);
        const auto src = code.source();

        x68k::BlockPlan plan{};
        REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
        CHECK(plan.count == 1);
        CHECK(plan.end == x68k::BlockEnd::kUnsupported);
        CHECK(plan.fallThroughPc == kEntry + 2);
    }

    // MOVE.b で An を触る形をブロックへ入れないこと。
    //
    // **instructionLength は 0x1008 (MOVE.b A0,D0) にも長さ 2 を返す**ので、
    // I1 は守ってくれない。68000 では MOVE.b が A レジスタに触ると不当命令
    // 例外に入るため、planOne が手で弾くしかない。
    //
    // 落ちる変異: planMove の srcMode == 0 / dstMode == 0 の検査を消す
    TEST_CASE("MOVE.b の An 参照はブロックに入らない")
    {
        // 命令長デコーダは長さを返してしまう。ここが守られていない証拠。
        CHECK(x68k::instructionLength(0x1008) != x68k::kUnknownLength);

        for (x68k::u32 reg = 0; reg < 8; ++reg)
        {
            // MOVE.b An,Dn (srcMode 1)。
            const auto fromAn = static_cast<x68k::u16>(0x1008u | reg);
            CAPTURE(fromAn);
            x68k::PlannedOp planned{};
            CHECK_FALSE(x68k::BlockPlanner::planOne(fromAn, kEntry, planned));

            // MOVE.b Dn,An (dstMode 1) = MOVEA.b。68000 に無い形。
            const auto toAn = static_cast<x68k::u16>(0x1040u | (reg << 9));
            CAPTURE(toAn);
            CHECK_FALSE(x68k::BlockPlanner::planOne(toAn, kEntry, planned));
        }

        // MOVEA.w / MOVEA.l は Tier A で**受け入れる**ようになった。
        // A レジスタを書くがメモリには触らず、例外も起きない。
        // **フラグを 1 つも変えない**ので、MOVE と別の種別にする。
        struct MoveaCase
        {
            x68k::u16 op;
            x68k::PlanKind kind;
        };
        const MoveaCase moveaOps[] = {
            {0x3040, x68k::PlanKind::kMoveaDregToAreg},  // MOVEA.w D0,A0
            {0x2040, x68k::PlanKind::kMoveaDregToAreg},  // MOVEA.l D0,A0
            {0x3048, x68k::PlanKind::kMoveaAregToAreg},  // MOVEA.w A0,A0
            {0x307C, x68k::PlanKind::kMoveaImmToAreg},   // MOVEA.w #imm,A0
        };
        for (const MoveaCase& c : moveaOps)
        {
            CAPTURE(c.op);
            x68k::PlannedOp planned{};
            REQUIRE(x68k::BlockPlanner::planOne(c.op, kEntry, planned));
            CHECK(planned.kind == c.kind);
        }
    }

    // BlockEnd の各値が実際に起こせること。
    //
    // 終端理由は段 1 以降の対応範囲を決める判断材料になる。到達不能な値が
    // 混ざっていると「その理由では切れていない」という誤った読みをする。
    //
    // 落ちる変異: kWindowExit を kUnsupported にまとめる
    TEST_CASE("終端理由が区別して立つ")
    {
        FlatCode code;
        FakeGen gen;
        const auto gsrc = gen.source();

        SUBCASE("kUnsupported: 許可リストの外の命令")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x7000);      // MOVEQ
            code.poke16(kEntry + 2, 0x4E75);  // RTS。許可リストの外
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count == 1);
            CHECK(plan.end == x68k::BlockEnd::kUnsupported);
        }

        SUBCASE("kUnknownLength: 命令長デコーダが長さを返さない命令")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x7000);
            // MOVEQ の bit8 が立つ形は不当命令で、長さも返らない。
            REQUIRE(x68k::instructionLength(0x7100) == x68k::kUnknownLength);
            code.poke16(kEntry + 2, 0x7100);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count == 1);
            CHECK(plan.end == x68k::BlockEnd::kUnknownLength);
        }

        SUBCASE("kWindowExit: 命令語が窓の外")
        {
            // 窓を [0x1000, 0x1003) に絞る。0x1000 の命令語 (2 バイト) は
            // 読めるが、0x1002 の命令語は 2 バイト目が窓の外へ出る。
            code.reset(kEntry, kEntry + 3);
            code.poke16(kEntry, 0x7000);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count == 1);
            CHECK(plan.end == x68k::BlockEnd::kWindowExit);
        }

        SUBCASE("kWindowExit: 拡張ワードが窓の外")
        {
            // BRA.w の変位ワードだけが窓を外れる形。命令語だけを見る実装を弾く。
            code.reset(kEntry, kEntry + 2);
            code.poke16(kEntry, 0x6000);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            CHECK_FALSE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
        }

        SUBCASE("kCapacity: 対応命令を kMaxOps より多く並べる")
        {
            code.reset(0x0800, 0x1800);
            for (x68k::u32 i = 0; i < x68k::kMaxOps + 2; ++i)
            {
                code.poke16(kEntry + i * 2, 0x7000);
            }
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count == x68k::kMaxOps);
            CHECK(plan.end == x68k::BlockEnd::kCapacity);
        }

        SUBCASE("kBranch: 分岐で終端し、後続は積まない")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x7000);      // MOVEQ
            code.poke16(kEntry + 2, 0x6002);  // BRA.s
            code.poke16(kEntry + 4, 0x7000);  // 積めるが、分岐の後なので入らない
            code.poke16(kEntry + 6, 0x7000);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            // I6: 分岐はブロックの最後にしか置けない。
            CHECK(plan.count == 2);
            CHECK(plan.end == x68k::BlockEnd::kBranch);
            CHECK(plan.ops[1].kind == x68k::PlanKind::kBranch);
        }
    }

    // 1 命令も積めなければ false を返すこと。
    //
    // 呼び出し側はこれを見て step() へ落とす。true のまま count == 0 の
    // 計画を返すと、実行器が「0 命令実行した」ことになり、PC が進まず
    // 無限ループになる。
    //
    // 落ちる変異: return out.count > 0 を return true にする
    TEST_CASE("1 命令も積めなければ false")
    {
        FlatCode code;
        FakeGen gen;
        const auto gsrc = gen.source();

        code.reset(0x0800, 0x1800);
        code.poke16(kEntry, 0x4E75);  // RTS。許可リストの外
        const auto src = code.source();

        x68k::BlockPlan plan{};
        CHECK_FALSE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
    }

    // 分岐のサイクル数を名指しで固定する。
    //
    // 全数の突き合わせは「実行と一致する」ことしか言わないので、
    // 68000 のマニュアルどおりの値であることは別に押さえる。
    // **Bcc.b は不成立 8 / 成立 10、Bcc.w は不成立 12 / 成立 10 で
    // 符号が逆になる。** BRA は常に成立するので 10 だけが使われる。
    TEST_CASE("分岐のサイクル数が成立側と不成立側で逆転する")
    {
        FlatCode code;
        FakeGen gen;
        const auto gsrc = gen.source();

        SUBCASE("Bcc.b は不成立 8 / 成立 10")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x6702);  // BEQ.s +2
            code.poke16(kEntry + 2, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.cyclesNotTaken == 8);
            CHECK(plan.cyclesTaken == 10);
        }

        SUBCASE("Bcc.w は不成立 12 / 成立 10")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x6700);      // BEQ.w
            code.poke16(kEntry + 2, 0x0004);  // 飛び先 0x1006
            code.poke16(kEntry + 4, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.cyclesNotTaken == 12);
            CHECK(plan.cyclesTaken == 10);
            CHECK(plan.branchTarget == 0x1006);
            CHECK(plan.fallThroughPc == 0x1004);
        }

        SUBCASE("BRA は常に成立するので 10 だけが使われる")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x6002);
            code.poke16(kEntry + 2, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.cyclesTaken == 10);
            // 実行と突き合わせる。BRA は CCR に関わらず飛ぶ。
            const StepResult r = runOne({0x6002, 0x4E71}, 0x00);
            CHECK(r.cycles == 10);
            CHECK(r.nextPc == plan.branchTarget);
        }

        SUBCASE("非分岐が混ざるとサイクルが積み上がる")
        {
            code.reset(0x0800, 0x1800);
            code.poke16(kEntry, 0x7000);      // MOVEQ  4
            code.poke16(kEntry + 2, 0x7201);  // MOVEQ  4
            code.poke16(kEntry + 4, 0x6702);  // BEQ.s  不成立 8 / 成立 10
            code.poke16(kEntry + 6, 0x4E71);
            const auto src = code.source();

            x68k::BlockPlan plan{};
            REQUIRE(x68k::BlockPlanner::plan(src, gsrc, kEntry, plan));
            CHECK(plan.count == 3);
            CHECK(plan.cyclesNotTaken == 4 + 4 + 8);
            CHECK(plan.cyclesTaken == 4 + 4 + 10);
        }
    }

    // 計画したデコード結果 (レジスタ番号・サイズ・演算種別) が符号化と一致する。
    //
    // 段 2 のエミッタはここだけを見て機械語を吐くので、ここが 1 つ違うと
    // 別のレジスタを壊す。**サイズは 1/2/4 バイト**で、MOVE の符号化にある
    // 0/1/2 ではない。
    //
    // 落ちる変異:
    //   - moveSizeFromGroup で $2 と $3 を入れ替える (MOVE.l と MOVE.w が逆)
    //   - srcReg と dstReg を取り違える
    TEST_CASE("デコード結果が符号化と一致する")
    {
        struct Case
        {
            x68k::u16 op;
            x68k::PlanKind kind;
            x68k::u8 srcReg;
            x68k::u8 dstReg;
            x68k::u8 size;
            const char* name;
        };
        // **$2 が long で $3 が word。** グループ番号の順とサイズの順が
        // 一致しないのは 68000 の符号化の都合。ここを素直に並べると
        // MOVE.l と MOVE.w が入れ替わる。
        const Case cases[] = {
            {0x1401, x68k::PlanKind::kMoveRegToReg, 1, 2, 1, "MOVE.b D1,D2"},
            {0x3605, x68k::PlanKind::kMoveRegToReg, 5, 3, 2, "MOVE.w D5,D3"},
            {0x2E07, x68k::PlanKind::kMoveRegToReg, 7, 7, 4, "MOVE.l D7,D7"},
            {0xD203, x68k::PlanKind::kAluRegToReg, 3, 1, 1, "ADD.b D3,D1"},
            {0xD644, x68k::PlanKind::kAluRegToReg, 4, 3, 2, "ADD.w D4,D3"},
            {0xDA85, x68k::PlanKind::kAluRegToReg, 5, 5, 4, "ADD.l D5,D5"},
            {0x9002, x68k::PlanKind::kAluRegToReg, 2, 0, 1, "SUB.b D2,D0"},
            {0xC441, x68k::PlanKind::kAluRegToReg, 1, 2, 2, "AND.w D1,D2"},
            {0x8083, x68k::PlanKind::kAluRegToReg, 3, 0, 4, "OR.l D3,D0"},
            {0xB246, x68k::PlanKind::kAluRegToReg, 6, 1, 2, "CMP.w D6,D1"},
        };

        for (const auto& c : cases)
        {
            CAPTURE(c.name);
            CAPTURE(c.op);
            x68k::PlannedOp p{};
            REQUIRE(x68k::BlockPlanner::planOne(c.op, kEntry, p));
            CHECK(p.kind == c.kind);
            CHECK(p.srcReg == c.srcReg);
            CHECK(p.dstReg == c.dstReg);
            CHECK(p.size == c.size);
            CHECK(p.op == c.op);
            CHECK(p.pc == kEntry);
        }

        // 演算種別。グループ番号と演算の対応は 68000 の符号化どおり。
        struct AluCase
        {
            x68k::u16 op;
            x68k::PlanAluOp aluOp;
            const char* name;
        };
        const AluCase aluCases[] = {
            {0x8001, x68k::PlanAluOp::kOr, "OR.b D1,D0"},
            {0x9001, x68k::PlanAluOp::kSub, "SUB.b D1,D0"},
            {0xB001, x68k::PlanAluOp::kCmp, "CMP.b D1,D0"},
            {0xC001, x68k::PlanAluOp::kAnd, "AND.b D1,D0"},
            {0xD001, x68k::PlanAluOp::kAdd, "ADD.b D1,D0"},
        };
        for (const auto& c : aluCases)
        {
            CAPTURE(c.name);
            x68k::PlannedOp p{};
            REQUIRE(x68k::BlockPlanner::planOne(c.op, kEntry, p));
            CHECK(p.aluOp == c.aluOp);
        }

        // MOVEQ の即値は符号拡張して 32bit で持つ。境界を名指しで固定する。
        struct MoveqCase
        {
            x68k::u16 op;
            x68k::u32 imm;
            x68k::u8 dstReg;
            const char* name;
        };
        const MoveqCase moveqCases[] = {
            {0x7000, 0x00000000u, 0, "MOVEQ #0,D0"},
            {0x707F, 0x0000007Fu, 0, "MOVEQ #$7F,D0 (正の上限)"},
            {0x7280, 0xFFFFFF80u, 1, "MOVEQ #$80,D1 (負の下限)"},
            {0x7EFF, 0xFFFFFFFFu, 7, "MOVEQ #$FF,D7 (-1)"},
        };
        for (const auto& c : moveqCases)
        {
            CAPTURE(c.name);
            x68k::PlannedOp p{};
            REQUIRE(x68k::BlockPlanner::planOne(c.op, kEntry, p));
            CHECK(p.kind == x68k::PlanKind::kMoveq);
            CHECK(p.imm == c.imm);
            CHECK(p.dstReg == c.dstReg);
            // MOVEQ は常に 32bit をレジスタ全体へ書く。
            CHECK(p.size == 4);
        }

        // 分岐の条件コード。BRA は 0。
        x68k::PlannedOp bra{};
        REQUIRE(x68k::BlockPlanner::planOne(0x6002, kEntry, bra));
        CHECK(bra.kind == x68k::PlanKind::kBranch);
        CHECK(bra.cond == 0);
        x68k::PlannedOp beq{};
        REQUIRE(x68k::BlockPlanner::planOne(0x6702, kEntry, beq));
        CHECK(beq.cond == 7);
    }

    // 計画の器のサイズが想定どおりであること。
    //
    // 256 ブロックぶんを内部 SRAM から確保する前提で kMaxOps = 4 を選んで
    // いるので、暗黙のパディングで膨らむと確保が破綻する。
    TEST_CASE("BlockPlan の大きさが見積もりどおり")
    {
        CHECK(x68k::kMaxOps == 4);
        CHECK(sizeof(x68k::PlannedOp) == 20);
        CHECK(sizeof(x68k::BlockPlan) == 112);
    }

    // 番地 0 からは翻訳しない (空きスロットの番兵と衝突するため)。
    //
    // BlockPlan::entryPc の 0 は「このスロットは空」を意味する。番地 0 の
    // ブロックを作ると空きスロットと区別できない計画ができ、検索側が空きと
    // 見て上書きするか空きを有効と読むかで静かに壊れる。
    TEST_CASE("番地 0 からは翻訳しない")
    {
        // 番地 0 に、単体なら確実に翻訳できる命令 (MOVEQ) を置く。
        static x68k::u16 words[] = {0x7001, 0x7002, 0x7003, 0x7004};
        const auto read16 = [](void* ctx, x68k::u32 addr, x68k::u16& outWord) -> bool
        {
            (void)ctx;
            if (addr >= sizeof(words))
            {
                return false;
            }
            outWord = words[addr / 2];
            return true;
        };
        const auto generation = [](void*, x68k::u32) -> x68k::u16 { return 0; };
        const auto mappingEpoch = [](void*) -> x68k::u32 { return 1; };

        x68k::PlanSource src{read16, nullptr};
        x68k::PlanGenSource gen{generation, mappingEpoch, nullptr};

        x68k::BlockPlan plan{};
        // 同じ命令が番地 0 以外なら組めることを先に示す (番兵が理由だと分かる)。
        CHECK(x68k::BlockPlanner::plan(src, gen, 2, plan));
        CHECK(plan.count > 0);

        x68k::BlockPlan atZero{};
        CHECK_FALSE(x68k::BlockPlanner::plan(src, gen, 0, atZero));
    }
}
