// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#include "jit/block_runner.h"

#include "cpu/block_planner.h"
#include "cpu/code_gen_map.h"
#include "jit/block_emitter.h"

namespace x68k::jit
{
namespace
{

// PlanSource / PlanGenSource が M68k を触るための箱。
struct PlanCtx
{
    M68k* cpu;
};

bool readCodeWord(void* ctx, u32 addr, u16& out)
{
    // **窓の中からだけ読む。** バスを叩くと翻訳しただけで MFP の
    // 割り込み要因レジスタが動く。
    return static_cast<PlanCtx*>(ctx)->cpu->peekCodeWord(addr, out);
}

std::uint16_t pageGeneration(void* ctx, u32 addr)
{
    return static_cast<PlanCtx*>(ctx)->cpu->codeGenMap().generation(addr);
}

u32 mappingEpochOf(void* ctx)
{
    return static_cast<PlanCtx*>(ctx)->cpu->codeGenMap().mappingEpoch();
}

}  // namespace

void BlockRunner::rememberFailure(std::uint32_t entryPc, std::uint16_t gen)
{
    // **飽和したページは覚えない。** kAlwaysStale は「常に古い」なので、
    // 覚えても次回必ず外れる。覚えた分だけ表を無駄にする。
    if (gen == CodeGenMap::kAlwaysStale)
    {
        return;
    }
    neg_.insert(entryPc, gen);
}

void BlockRunner::reset()
{
    for (std::uint32_t i = 0; i < slotCount_; ++i)
    {
        slots_[i] = BlockSlot{};
    }
    if (code_ != nullptr)
    {
        code_->reset();
    }
    neg_.clear();
    codeFull_ = false;
}

BlockSlot* BlockRunner::translate(M68k& cpu, std::uint32_t entryPc)
{
    // **満杯からの回復。** ここに回復経路が無かったせいで、実機 45 秒の
    // うち翻訳器はほぼ全期間停止していた (実測 14,815,340 回 = 諦めた
    // 回数の 99.997%)。
    //
    // Why not 満杯のたびに捨てるか: 捨てる費用は常駐ブロックの再翻訳
    // まるごとなので、要求のたびに捨てるとキャッシュが永久に冷たいまま
    // になる (スラッシング)。要求が閾値まで積もってから 1 回だけ捨てる。
    //
    // Why not eviction にしないか: 実行可能メモリはバンプアロケータ
    // (used_ を進めるだけ) なので、個別のブロックだけ解放できない。
    // 段 3 の課題として温存し、ここでは「全部捨てる」で回復させる。
    if (codeFull_)
    {
        ++stats_.fullDeferred;
        if (++fullSeen_ >= kCapacityResetThreshold)
        {
            // reset() が codeFull_ を false へ戻し、スロットと負の記憶も
            // 一緒に捨てる。**世代は捨てない** (飽和の回復とは別の話で、
            // 世代を 0 へ戻すのは控えを全部捨てたときだけという条件は
            // reset() だけでは満たせるが、ここで一緒にやると
            // 「満杯」と「飽和」の統計が混ざる)。
            reset();
            fullSeen_ = 0;
            ++stats_.capacityReset;
        }
        return nullptr;
    }

    // **一度失敗した番地なら、もう試さない。**
    //
    // 世代が当時のままなら同じ命令列が同じ結果になる。動いていれば
    // 覚え直す (ゲストが書き換えた可能性がある)。写像の変化は run() が
    // 一括で捨てている。
    CodeGenMap& map = cpu.codeGenMap();
    BlockSlot& here = slots_[slotIndex(entryPc)];
    const std::uint16_t nowGen = map.generation(entryPc);
    if (neg_.contains(entryPc, nowGen))
    {
        ++stats_.negativeHit;
        return nullptr;
    }

    PlanCtx ctx{&cpu};
    const PlanSource src{&readCodeWord, &ctx};
    const PlanGenSource gen{&pageGeneration, &mappingEpochOf, &ctx};

    const M68k::CodeWindow window = cpu.codeWindowForJit();
    EmitEnv env{};
    if constexpr (sizeof(void*) == 4)
    {
        env.ramBaseAddr =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(window.ramBase));
        env.ramLimit = window.ramLimit;
        env.ramReadable = window.ramReadable;
        // 世代配列も焼く (Tier C)。**plan が控えた mappingEpoch と同じ
        // 呼び出しの中で読む** (G8 と同じ論法)。CodeGenMap::setStorage が
        // bumpMappingEpoch を呼ぶので、差し替われば走る前に鍵が外れる。
        env.genBaseAddr =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(map.storage()));
        env.genPageCount = map.pageCount();
        // 動的分岐 (Tier D) の飛び先を受け取る 1 語 (G21)。
        //
        // **epoch の保護が要らない唯一の焼き込み。** 窓や世代配列は
        // 差し替わりうるので mappingEpoch を鍵に持つが、これは runner 自身の
        // メンバで、runner が動かない限り不変 (block_runner.h の根拠)。
        env.mailboxAddr =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&branchMailbox_));
    }

    // **翻訳器にエミッタの都合を教える。**
    //
    // 窓が読めない (ROM 写像中など) なら、読み形を積んだ時点でエミッタが
    // ブロックを丸ごと拒否する。手前で終端させれば短くても翻訳できる。
    struct CapsCtx
    {
        const EmitEnv* env;
    };
    CapsCtx capsCtx{&env};
    const PlanCapabilities caps{[](void* c) -> bool
                                {
                                    const EmitEnv& e = *static_cast<CapsCtx*>(c)->env;
                                    return e.ramReadable && e.ramBaseAddr != 0 && e.ramLimit != 0;
                                },
                                // 書き形の条件 (G19)。**ramReadable は要らない**
                                // (書き経路は見ない、m68k.cpp:331-334)。代わりに
                                // 世代配列が要る (touch を再現するため) のと、
                                // 「範囲ガード成立 ⇒ ページ番号が配列内」が
                                // 導けることを条件にする。
                                [](void* c) -> bool
                                {
                                    const EmitEnv& e = *static_cast<CapsCtx*>(c)->env;
                                    return canEmitWritesIn(e);
                                },
                                // 動的分岐の条件 (Tier D)。**飛び先の置き場**
                                // だけを見る。RTS が要る読みの窓と JSR が要る
                                // 書きの窓は、エミッタが kind ごとに別に見る。
                                [](void* c) -> bool
                                {
                                    const EmitEnv& e = *static_cast<CapsCtx*>(c)->env;
                                    return canEmitDynamicBranchIn(e);
                                },
                                &capsCtx};

    BlockPlan plan{};
    if (!BlockPlanner::plan(src, gen, entryPc, plan, caps))
    {
        ++stats_.translateFail;
        // **飽和したページが増えると翻訳できなくなる。**
        if (map.generation(entryPc) == CodeGenMap::kAlwaysStale)
        {
            //
            // 世代は 16bit で、1KB ページに 65,536 回書くと飽和して
            // kAlwaysStale で止まる。スタックやワーク領域なら数秒で届く。
            // 飽和したページ上のコードは I9 で永久に翻訳を拒否される。
            //
            // 実測: 翻訳失敗 190 万件の **99.6% がこれ**だった。
            // どれも $000000 台 (メインメモリ) で、範囲外ではなく飽和。
            //
            // 世代を 0 へ戻せば再び翻訳できる。**キャッシュを全部捨てる
            // のと同時にしかやってはいけない** (控えを持ったまま戻すと
            // 「変わっていない」と誤判定する)。ここは reset() を呼ぶので
            // その条件を満たす。
            ++saturatedSeen_;
            if (saturatedSeen_ >= kSaturationResetThreshold)
            {
                reset();
                map.resetGenerations();
                saturatedSeen_ = 0;
                ++stats_.generationReset;
            }
        }

        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    // 出口の ir / irc は翻訳時に読める。I4 が fallThroughPc + 2 まで
    // 同一 1KB ページを保証しているので、必ず窓の中にある。
    u16 fallIr = 0;
    u16 fallIrc = 0;
    if (!cpu.peekCodeWord(plan.fallThroughPc, fallIr) ||
        !cpu.peekCodeWord(plan.fallThroughPc + 2, fallIrc))
    {
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    // 窓の実体を焼く (G8)。**plan が控えた mappingEpoch と同じ呼び出しの
    // 中で読む。** 実行前の鍵照合が epoch を見るので、窓が動けば走る前に
    // このブロックは捨てられる。
    //
    // ホストアドレスを u32 で持つのは、ESP32-S3 のポインタが 32bit だから。
    // 32bit でない環境では読み形を焼かない (env が 0 のまま = G12 で断る)。
    const std::size_t need = requiredSize(plan, fallIr, fallIrc, env);
    if (need == 0)
    {
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    if (need > kStagingBytes)
    {
        // 発行用の控えに収まらない。段 1 の kMaxOps = 4 では起きないが、
        // 上限を上げたときに黙って壊れないよう明示的に諦める。
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    std::uint8_t* buf = code_->allocate(need);
    if (buf == nullptr)
    {
        // 使い切った。**以後は翻訳を試みない。**
        //
        // Why not 毎回試すか: 溢れてからも allocate を呼び続けると、
        // ブロックに当たらない命令のたびに翻訳のコストを払う。
        // 段 3 で eviction を入れるまでは、埋まったら諦める。
        codeFull_ = true;
        ++stats_.translateFail;
        return nullptr;
    }

    // **通常の RAM へ発行してから写す。** IRAM へバイト書き込みをすると
    // LoadStoreError で落ちる (実機で踏んだ)。
    EmittedBlock emitted{};
    if (!emitBlock(plan, fallIr, fallIrc, env, staging_, need, emitted))
    {
        ++stats_.translateFail;
        rememberFailure(entryPc, nowGen);
        return nullptr;
    }

    // 32bit 単位で写す。allocate が 4 バイト整列を返すので、
    // 端数は最後の 1 語だけ読み書きすれば足りる。
    const std::size_t words = (need + 3) / 4;
    auto* dst = reinterpret_cast<volatile std::uint32_t*>(buf);
    const auto* words32 = reinterpret_cast<const std::uint32_t*>(staging_);
    for (std::size_t i = 0; i < words; ++i)
    {
        dst[i] = words32[i];
    }

    // **書いたら確定させる。** isync を通さないと命令フェッチが
    // 書く前の中身を拾いうる (1 回目だけ壊れる形で出る)。
    code_->commit();

    BlockSlot& slot = here;
    // **鍵だけを写す。** ops[] は翻訳の途中でしか要らない。
    slot.entryPc = plan.entryPc;
    slot.mappingEpoch = plan.mappingEpoch;
    slot.page = plan.page;
    slot.pageGen = plan.pageGen;
    slot.count = plan.count;
    slot.code = buf + emitted.entryOffset;
    slot.endsWithBranch = emitted.endsWithBranch;
    slot.endsWithDynamicBranch = emitted.endsWithDynamicBranch;
    slot.branchTarget = emitted.branchTarget;
    return &slot;
}

NativeResult BlockRunner::run(M68k& cpu)
{
    // 設計 §5.4: 入口で必ず見る。
    if (cpu.mustDeferToStep())
    {
        ++stats_.deferInterrupt;
        return NativeResult{0, NativeExit::kDeferToStep};
    }

    // **写像が変わったら負の記憶を全部捨てる。**
    //
    // 世代はページ単位だが写像は全体に効く。個別に持つより一括で捨てる方が
    // 安く、かつ漏れようがない。写像が動くのは setFastRam / setFastRom /
    // reset / loadStateForTest だけなので稀。
    const u32 nowEpoch = cpu.codeGenMap().mappingEpoch();
    if (nowEpoch != seenEpoch_)
    {
        neg_.clear();
        seenEpoch_ = nowEpoch;
    }

    // 現在の命令語アドレス。プリフェッチの契約により pc は「命令語 + 4」。
    const M68kState& st = cpu.state();
    const u32 entryPc = st.pc - 4;

    BlockSlot* slot = &slots_[slotIndex(entryPc)];

    // 設計 §5.5 の順で照合する。**順序が意味を持つ。**
    CodeGenMap& map = cpu.codeGenMap();
    const std::uint16_t nowGen = map.generation(slot->page << CodeGenMap::kPageShift);
    const bool hit = slot->code != nullptr &&                     // 翻訳済み
                     slot->entryPc != 0 &&                        // 空きの番兵
                     slot->entryPc == entryPc &&                  // タグ
                     slot->count <= kMaxOps &&                    // ゴミ検査
                     slot->mappingEpoch == map.mappingEpoch() &&  // 写像
                     nowGen != CodeGenMap::kAlwaysStale &&        // **先に見る**
                     nowGen == slot->pageGen;                     // 世代の一致

    if (!hit)
    {
        if (slot->code == nullptr)
        {
            // 空きスロット (コールドミス)。**ここを数えていなかったので、
            // 取りこぼしの最大成分が統計から消えていた。**
            ++stats_.keyMissCold;
        }
        else
        {
            // 鍵が外れた理由を分けて数える。段 3 の判断材料になる。
            //
            // **判定 (hit) の順序と帰属の順序は別物。** hit は
            // kAlwaysStale を世代一致より先に見る (§5.5 の正しさの根拠) が、
            // ここでタグより先に見ると、居座りブロックのページが飽和して
            // いるだけでスロット衝突が「飽和」に化ける。実機ではページ 0
            // が真っ先に飽和するので、衝突 151 万件がまるごと誤帰属された。
            //
            // タグを先に見れば、飽和を数える時点で slot->page は entryPc の
            // ページと同一が保証され、keyMissStale は「自分のページの飽和」
            // だけを意味する。
            if (slot->entryPc != entryPc)
            {
                ++stats_.keyMissTag;
            }
            else if (slot->mappingEpoch != map.mappingEpoch())
            {
                ++stats_.keyMissEpoch;
            }
            else if (nowGen == CodeGenMap::kAlwaysStale)
            {
                ++stats_.keyMissStale;
            }
            else
            {
                ++stats_.keyMissGen;
            }
        }
        slot = translate(cpu, entryPc);
        if (slot == nullptr)
        {
            ++stats_.deferUnsupported;
            return NativeResult{0, NativeExit::kDeferToStep};
        }
    }

    // 生成コードを呼ぶ。**引数の順は (state, code)。**
    const std::uint32_t ret = runBlock(&cpu.state(), slot->code);

    ++stats_.blocksRun;
    // **実行ごとの終端理由。** translate() で数えると、キャッシュが温まった
    // 後は翻訳が起きないので実態が見えない (Tier C で踏んだ)。
    // 分岐で終わったブロックの割合。direct chaining が効くかを決める。
    if (slot->endsWithBranch)
    {
        ++stats_.endedWithBranch;
    }
    if (slot->endsWithDynamicBranch)
    {
        ++stats_.endedWithDynamicBranch;
    }

    const BlockReturn decoded = decodeBlockReturn(ret);
    const u32 cycles = decoded.cycles;

    if (decoded.guardExit)
    {
        // ガードが不成立で降りた。**実際に走った命令数だけ数える。**
        ++stats_.guardExit;
        stats_.insnsRun += decoded.ranOps;

        if (decoded.selfPageExit)
        {
            // G13/G18: 自ページ書き換えで降りた。
            //
            // **このあと step() が書いて、そのページの世代を必ず上げる。**
            // 次に同じ entryPc へ来ると鍵が世代で外れ、毎周まるごと
            // 再翻訳になる。ふつうの負のキャッシュは (pc, gen) 一致でしか
            // 効かないので、gen が毎回動くと素通りする。
            // **世代を鍵から外した印**を焼くことでしか止められない。
            //
            // 保守的すぎる面がある (一度きりの自己パッチでも epoch が
            // 動くまでその番地の JIT を失う) ので、selfPageExit を数えて
            // blocksRun に対する比率を見る。0.1% を超えるなら、
            // 「N 回までは許す」等の再設計を検討する材料になる。
            ++stats_.selfPageExit;
            neg_.insert(entryPc, NegativeCache::kAnyGen);
        }

        // G10: 1 命令も進んでいないなら kDeferToStep を返す。
        //
        // **これが無いと Machine::run が used == 0 を halted と誤読する**か、
        // 同じブロックを 0 サイクルで回し続ける。ガードは状態を 1 bit も
        // 変えずに降りているので、NativeExec の「何も起きなかった」という
        // 事後条件をそのまま満たす。
        if (!guardExitMadeProgress(decoded))
        {
            ++stats_.deferGuard;
            return NativeResult{0, NativeExit::kDeferToStep};
        }

        // 途中まで進んだ。残りは step() が本物の read16 / read32 で実行し、
        // そこで例外 (アドレスエラー / バスエラー) や I/O の副作用が起きる。
        return NativeResult{cycles, NativeExit::kRan};
    }

    stats_.insnsRun += slot->count;

    // **飛び先の決定は純関数に出してある** (block_emitter.h の nextBranch)。
    // ここは決まった答えに従うだけ。runner は runBlock (ESP32 のアセンブリ)
    // に依存していてホストで走らせられないので、if をここに書くと
    // 「静的とメールボックスのどちらから飛び先を採ったか」をホストの
    // テストが一切問えなくなる (guardExitMadeProgress と同じ手口)。
    const BranchDecision decision = nextBranch(decoded, slot->branchTarget, branchMailbox_);

    if (decision.source == BranchSource::kMailbox)
    {
        ++stats_.dynamicBranch;
    }

    if (decision.shouldBranch())
    {
        // 設計 §5.2: プリフェッチを詰め直すのは成立側だけ。
        //
        // 静的分岐の奇数は I7 が翻訳時に、動的分岐の奇数は生成コードの
        // ガード (G22) が弾いているので通常は起きない。起きたら branchTo が
        // 既にアドレスエラーへ入っており、そのまま返してよい。
        //
        // **ここに残る 2 行はホストで検査できない。** 純関数に出せるのは
        // 「飛ぶか」と「どこへ」の判断までで、branchTo を実際に呼ぶ行は
        // runBlock 経由でしか到達しないため、テストで殺せる変異にならない
        // (decision.target を slot->branchTarget に戻す変異、shouldBranch を
        // 無視して常に飛ぶ変異は、いずれもホストのテストを素通りする)。
        // 残す代わりに **判断を一切含まない形**に保つ: 条件は shouldBranch
        // そのまま、引数は decision.target そのまま。ここに if を足したり
        // 別の値を渡したりしたくなったら、それは nextBranch 側へ動かす。
        (void)cpu.branchTo(decision.target);
    }

    return NativeResult{cycles, NativeExit::kRan};
}

NativeResult BlockRunner::runThunk(void* context, M68k& cpu)
{
    return static_cast<BlockRunner*>(context)->run(cpu);
}

const NativeStats* BlockRunner::statsThunk(void* context)
{
    return &static_cast<BlockRunner*>(context)->stats_;
}

}  // namespace x68k::jit
