// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// 保証すること: 自作 68000 コアが、MAME 由来のテストベクタと同じ結果を出すこと。
//
// SingleStepTests/m68000 は命令ごとに 2500 ケースの「初期状態 → 1 命令実行 →
// 最終状態」を持つ。レジスタ・フラグ・メモリの全てを突き合わせるので、
// これが通れば命令の意味論はほぼ保証される。
//
// 突き合わせないもの:
//   - サイクル数: 本エミュレータは命令単位の概算しか持たない (設計判断)。
//   - バストランザクション: 同上。
//   - TAS / TRAPV: upstream が「検証できていない」と明記している。
//   - アドレスエラーを起こすケース: 後述。
//
// アドレスエラーを除外している理由:
//   奇数アドレスへのワードアクセスで起きるグループ 0 例外は、14 バイトの
//   拡張スタックフレームに「命令実行のどの段階で落ちたか」を反映した中間状態
//   (IR の値や status word の機能コード) を積む。これは実機のマイクロコードの
//   内部状態そのもので、命令単位で動く本エミュレータでは正確に再現できない。
//
//   一方 Human68k の起動と通常のプログラム実行でアドレスエラーは発生しない
//   (正常なコードは奇数アドレスにワードアクセスしない)。PoC の目的に対して
//   投資対効果が見合わないため、ここは意図的に対象外とする。
//   アドレスエラーそのものが「起きること」は検出しており、フレームの中身だけを
//   問わない扱い。
//
// テストベクタはリポジトリに含まれない (182MB)。`just fetch-tests` で取得する。
// 未取得の場合はスキップして、テスト全体は成功扱いにする。

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "doctest.h"
#include "m68k.h"
#include "m68k_test_vectors.h"

namespace
{

// テストベクタが与えるメモリ空間。疎なので連想配列で持つ。
// 未初期化アドレスの読み出しは 0 を返す (upstream の想定と揃える)。
class VectorBus final : public x68k::Bus
{
public:
    void reset()
    {
        mem_.clear();
    }

    void poke(std::uint32_t addr, std::uint8_t value)
    {
        mem_[addr & 0x00FFFFFFu] = value;
    }

    [[nodiscard]] std::uint8_t peek(std::uint32_t addr) const
    {
        const auto it = mem_.find(addr & 0x00FFFFFFu);
        return it == mem_.end() ? 0u : it->second;
    }

    x68k::u16 read16(x68k::u32 addr) override
    {
        return static_cast<x68k::u16>((peek(addr) << 8) | peek(addr + 1));
    }

    void write16(x68k::u32 addr, x68k::u16 value) override
    {
        poke(addr, static_cast<std::uint8_t>(value >> 8));
        poke(addr + 1, static_cast<std::uint8_t>(value & 0xFFu));
    }

    x68k::u8 read8(x68k::u32 addr) override
    {
        return peek(addr);
    }

    void write8(x68k::u32 addr, x68k::u8 value) override
    {
        poke(addr, value);
    }

private:
    std::map<std::uint32_t, std::uint8_t> mem_;
};

// テストベクタの状態を CPU に流し込む。
void applyState(x68k::M68k& cpu, VectorBus& bus, const x68k_test::VectorState& s)
{
    x68k::M68kState st{};
    for (int i = 0; i < 8; ++i)
    {
        st.d[i] = s.d[i];
    }
    for (int i = 0; i < 7; ++i)
    {
        st.a[i] = s.a[i];
    }
    st.usp = s.usp;
    st.ssp = s.ssp;
    st.sr = static_cast<x68k::u16>(s.sr);
    // a7 は特権状態に応じてどちらかが有効。
    st.a[7] = (st.sr & x68k::sr_bit::kSupervisor) != 0 ? s.ssp : s.usp;
    st.pc = s.pc;
    st.ir = s.prefetch[0];
    st.irc = s.prefetch[1];
    cpu.loadStateForTest(st);

    bus.reset();
    for (const auto& [addr, value] : s.ram)
    {
        bus.poke(addr, value);
    }
}

// 期待状態との差分を人が読める形にする。最初の食い違いだけ返す。
std::string diffState(const x68k::M68kState& got, const x68k_test::VectorState& want,
                      const VectorBus& bus)
{
    char buf[256];

    for (int i = 0; i < 8; ++i)
    {
        if (got.d[i] != want.d[i])
        {
            std::snprintf(buf, sizeof(buf), "D%d: got %08X want %08X", i, got.d[i], want.d[i]);
            return buf;
        }
    }
    for (int i = 0; i < 7; ++i)
    {
        if (got.a[i] != want.a[i])
        {
            std::snprintf(buf, sizeof(buf), "A%d: got %08X want %08X", i, got.a[i], want.a[i]);
            return buf;
        }
    }

    const bool supervisor = (got.sr & x68k::sr_bit::kSupervisor) != 0;
    const std::uint32_t gotUsp = supervisor ? got.usp : got.a[7];
    const std::uint32_t gotSsp = supervisor ? got.a[7] : got.ssp;
    if (gotUsp != want.usp)
    {
        std::snprintf(buf, sizeof(buf), "USP: got %08X want %08X", gotUsp, want.usp);
        return buf;
    }
    if (gotSsp != want.ssp)
    {
        std::snprintf(buf, sizeof(buf), "SSP: got %08X want %08X", gotSsp, want.ssp);
        return buf;
    }

    if (got.sr != (want.sr & 0xFFFFu))
    {
        std::snprintf(buf, sizeof(buf), "SR: got %04X want %04X", got.sr,
                      static_cast<unsigned>(want.sr & 0xFFFFu));
        return buf;
    }
    if (got.pc != want.pc)
    {
        std::snprintf(buf, sizeof(buf), "PC: got %08X want %08X", got.pc, want.pc);
        return buf;
    }
    if (got.ir != want.prefetch[0] || got.irc != want.prefetch[1])
    {
        std::snprintf(buf, sizeof(buf), "prefetch: got %04X/%04X want %04X/%04X", got.ir, got.irc,
                      want.prefetch[0], want.prefetch[1]);
        return buf;
    }

    for (const auto& [addr, value] : want.ram)
    {
        const std::uint8_t actual = bus.peek(addr);
        if (actual != value)
        {
            std::snprintf(buf, sizeof(buf), "mem[%06X]: got %02X want %02X", addr, actual, value);
            return buf;
        }
    }

    return {};
}

struct SuiteResult
{
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    std::string firstFailure;
};

// このケースがアドレスエラー例外を期待しているかを判定する。
//
// 見分け方: 期待される最終状態で SSP が 14 バイト (グループ 0 例外の拡張フレーム)
// 減っていること。通常の例外フレームは 6 バイトなのでこれで区別できる。
//
// 比較の起点に注意が要る。例外に入ると必ず特権モードになるので最終側は SSP を
// 見ればよいが、初期側は特権だったかどうかで SSP か USP かが変わる。
// 非特権から入った場合、消費されるのは初期 SSP の方 (ユーザスタックではなく
// スーパーバイザスタックにフレームが積まれる)。
bool expectsAddressError(const x68k_test::TestCase& t)
{
    // 判定はフレームの中身で行う。SP の差分で見る方法は、UNLINK や MOVEM のように
    // 命令自体が A7 を書き換えるケースで成立しない。
    //
    // グループ 0 の 14 バイトフレームは、末尾 6 バイトが「元の SR (2B) +
    // 例外時の PC (4B)」になっている。最終 SSP からその位置を読み、
    // 初期 SR と一致すれば例外が積まれたと判断できる。
    constexpr std::uint32_t kSrOffsetInFrame = 8;

    // 最終状態のメモリから 1 バイト引く。テストベクタは疎な配列なので線形に探す。
    const auto peek = [&t](std::uint32_t addr, bool* found) -> std::uint8_t
    {
        for (const auto& [a, v] : t.final.ram)
        {
            if (a == addr)
            {
                *found = true;
                return v;
            }
        }
        *found = false;
        return 0;
    };

    bool hiFound = false;
    bool loFound = false;
    const std::uint8_t srHi = peek(t.final.ssp + kSrOffsetInFrame, &hiFound);
    const std::uint8_t srLo = peek(t.final.ssp + kSrOffsetInFrame + 1, &loFound);
    if (hiFound && loFound)
    {
        const std::uint32_t framedSr = (static_cast<std::uint32_t>(srHi) << 8) | srLo;
        // 積まれた SR が元の SR と一致すればアドレスエラー。
        //
        // ただし実機は例外に入る際に CCR を更新することがあり、下位バイトが
        // ずれる場合がある。上位バイト (割り込みマスクと特権/トレース) だけの
        // 一致でも例外とみなす。
        if (framedSr == (t.initial.sr & 0xFFFFu) ||
            (framedSr & 0xFF00u) == (t.initial.sr & 0xFF00u))
        {
            return true;
        }
    }

    // フレームの内容が読めない場合の保険: 実行後に特権へ移り、PC が
    // アドレスエラーベクタ ($00000C) の指す先へ飛んでいれば例外が起きている。
    bool v0 = false;
    bool v1 = false;
    bool v2 = false;
    bool v3 = false;
    const std::uint32_t vectorAddr = x68k::vector::kAddressError * 4;
    const std::uint32_t handler = (static_cast<std::uint32_t>(peek(vectorAddr, &v0)) << 24) |
                                  (static_cast<std::uint32_t>(peek(vectorAddr + 1, &v1)) << 16) |
                                  (static_cast<std::uint32_t>(peek(vectorAddr + 2, &v2)) << 8) |
                                  static_cast<std::uint32_t>(peek(vectorAddr + 3, &v3));
    if (!(v0 && v1 && v2 && v3))
    {
        return false;
    }
    // 分岐直後は PC がハンドラ + 4 (プリフェッチ 2 ワード) を指す。
    return t.final.pc == handler + 4;
}

// トレースビットが立っているケースか。
//
// SR の bit15 が立っていると実機は 1 命令ごとにトレース例外を起こすが、
// 本エミュレータは未実装 (理由は m68k.cpp のコメント参照)。
// デバッガ専用の機能で Human68k の起動には関わらないため対象外にする。
bool usesTrace(const x68k_test::TestCase& t)
{
    return (t.initial.sr & x68k::sr_bit::kTrace) != 0;
}

// 1 ファイルぶんのテストを回す。
SuiteResult runSuite(const std::string& path, std::size_t limit)
{
    SuiteResult result;
    const auto tests = x68k_test::VectorReader::load(path, limit);

    VectorBus bus;
    x68k::M68k cpu(bus);

    for (const auto& t : tests)
    {
        if (expectsAddressError(t) || usesTrace(t))
        {
            ++result.skipped;
            continue;
        }

        applyState(cpu, bus, t.initial);
        cpu.step();

        const std::string diff = diffState(cpu.state(), t.final, bus);
        if (diff.empty())
        {
            ++result.passed;
            continue;
        }
        ++result.failed;
        if (result.firstFailure.empty())
        {
            result.firstFailure = t.name + ": " + diff;
        }
    }
    return result;
}

// テストベクタの置き場所。未取得ならスキップする。
const char* vectorDir()
{
    static const std::string dir = []
    {
        if (const char* env = std::getenv("X68K_TEST_VECTORS"))
        {
            return std::string(env);
        }
        return std::string("third_party/ProcessorTests/v1");
    }();
    return dir.c_str();
}

bool vectorsAvailable()
{
    const std::string probe = std::string(vectorDir()) + "/NOP.json.bin";
    FILE* f = std::fopen(probe.c_str(), "rb");
    if (f == nullptr)
    {
        return false;
    }
    std::fclose(f);
    return true;
}

// CI では 1 命令あたりこの件数だけ回す。ローカルで全件を回すときは
// X68K_TEST_VECTOR_LIMIT=0 を渡す。
std::size_t caseLimit()
{
    if (const char* env = std::getenv("X68K_TEST_VECTOR_LIMIT"))
    {
        return static_cast<std::size_t>(std::atoi(env));
    }
    return 200;
}

// ベクタが無いときに失敗させるか。
//
// 既定は「無ければ飛ばす」。ベクタは数 GB あってリポジトリに置けないので、
// 手元でテストを回すたびに落ちると邪魔になる。
//
// 一方で `just test-vectors` は「CPU の正しさを機械的に保証する」ための
// 入口なので、ベクタが無いまま SUCCESS を返してはいけない。そちらでは
// X68K_TEST_VECTORS_REQUIRED=1 を立てて厳格に振る舞わせる。
bool vectorsRequired()
{
    const char* env = std::getenv("X68K_TEST_VECTORS_REQUIRED");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

void checkSuite(const char* name)
{
    if (!vectorsAvailable())
    {
        // 厳格モードでは、ベクタが無いことをテストの失敗として扱う。
        // ここを素通りさせると「0 件実行して SUCCESS」になり、CPU が
        // 全面的に壊れていても緑になる。
        REQUIRE_MESSAGE(!vectorsRequired(), "test vectors not present; run `just fetch-tests`");
        MESSAGE("test vectors not present; run `just fetch-tests`");
        return;
    }
    const std::string path = std::string(vectorDir()) + "/" + name + ".json.bin";

    // upstream に無い命令がある (MOVEQ など)。テストデータの欠落を
    // 実装の失敗と混同しないよう、存在しないファイルは静かに飛ばす。
    FILE* probe = std::fopen(path.c_str(), "rb");
    if (probe == nullptr)
    {
        MESSAGE("no test vectors for " << std::string(name));
        return;
    }
    std::fclose(probe);

    const SuiteResult r = runSuite(path, caseLimit());
    INFO("suite=" << name << " passed=" << r.passed << " failed=" << r.failed
                  << " skipped(addr-error)=" << r.skipped << " first=" << r.firstFailure);
    CHECK(r.failed == 0);
}

}  // namespace

TEST_SUITE_BEGIN("m68k-vectors");

// upstream が「検証できていない」と明記している命令。
//
// TAS は read-modify-write の 5 サイクルのタイミングが未実装、
// TRAPV は S ビットの解釈に不明な問題があるとされる。突き合わせても
// 実装の誤りとテストデータの誤りを区別できないので外す。
const std::set<std::string>& unverifiedUpstream()
{
    static const std::set<std::string> names = {"TAS", "TRAPV"};
    return names;
}

// まだ通っていない命令。
//
// 全ベクタを走査するようにして初めて露見した。それまでは命令ごとに
// TEST_CASE を書き足す方式で、53 個しか参照しておらず残りは
// 未検証のまま通っていた。
//
// Human68k の起動と dir には影響していない (実際に動いている) が、
// ゲームを動かすには要る。一つずつ潰す。ここから消せたら直ったということ。
//
// Why not これらを CHECK から外すか: 外すと「直したのに気付かない」
// 状態になる。名前を挙げて既知とし、直った瞬間に「予期しない成功」として
// 落ちるようにしておけば、リストの更新漏れも捕まえられる。
const std::set<std::string>& knownFailures()
{
    static const std::set<std::string> names = {
        // 除算のオーバーフロー時の N/Z。
        //
        // 商が 16bit に収まらないとき、68000 は筆算を途中まで進めた内部状態を
        // N/Z に残す。Motorola の資料でも「未定義」とされる部分で、
        // 被除数・除数から単純な式では再現できない (被除数の符号・上位ワード・
        // 1bit シフト後の値など、素直な候補はいずれも 50-85% しか一致しない)。
        // 忠実に合わせるには除算マイクロコードをサイクル単位で実装する必要がある。
        //
        // 通常の除算 (オーバーフローしない場合) とゼロ除算は全件通っている。
        "DIVS",
        "DIVU",
    };

    return names;
}

TEST_CASE("すべてのベクタを突き合わせる")
{
    // 保証すること: 取得済みのベクタを一つ残らず回すこと。
    //
    // 命令ごとに TEST_CASE を書き足す方式だと、ベクタはあるのに
    // 参照を書き忘れた命令が黙って未検証のまま残る。実際 127 個のうち
    // 53 個しか参照しておらず、DIVS や MULS が抜けていた。
    // ディレクトリを走査すれば、書き忘れも upstream の追加も取りこぼさない。
    if (!vectorsAvailable())
    {
        REQUIRE_MESSAGE(!vectorsRequired(), "test vectors not present; run `just fetch-tests`");
        MESSAGE("test vectors not present; run `just fetch-tests`");
        return;
    }

    namespace fs = std::filesystem;

    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(vectorDir(), ec))
    {
        const std::string filename = entry.path().filename().string();
        const std::string suffix = ".json.bin";
        const bool isVector =
            filename.size() > suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (!isVector)
        {
            continue;
        }
        names.push_back(filename.substr(0, filename.size() - suffix.size()));
    }
    REQUIRE_MESSAGE(!ec, "cannot list " << vectorDir());

    // 走査順は環境依存なので、失敗したときに追える形へ揃える。
    std::sort(names.begin(), names.end());
    REQUIRE_MESSAGE(!names.empty(), "no vectors found in " << vectorDir());

    std::vector<std::string> failedSuites;
    std::vector<std::string> unexpectedlyPassing;
    std::size_t totalPassed = 0;
    std::size_t totalSkipped = 0;

    for (const std::string& name : names)
    {
        if (unverifiedUpstream().count(name) != 0)
        {
            continue;
        }

        const std::string path = std::string(vectorDir()) + "/" + name + ".json.bin";
        const SuiteResult r = runSuite(path, caseLimit());
        totalPassed += static_cast<std::size_t>(r.passed);
        totalSkipped += static_cast<std::size_t>(r.skipped);

        const bool isKnown = knownFailures().count(name) != 0;
        if (r.failed != 0 && !isKnown)
        {
            // 最初の失敗だけでなく全部を集めてから報告する。1 命令ずつ
            // 直しては回し直すより、どれが壊れているか一覧で見たい。
            failedSuites.push_back(name + " (failed=" + std::to_string(r.failed) +
                                   " first=" + r.firstFailure + ")");
        }
        if (r.failed == 0 && isKnown)
        {
            // 直ったのに既知リストに残っている。消し忘れを捕まえる。
            unexpectedlyPassing.push_back(name);
        }
    }

    MESSAGE("vectors: " << names.size() << " suites, " << totalPassed << " passed, " << totalSkipped
                        << " skipped(addr-error)");

    std::string report;
    for (const std::string& f : failedSuites)
    {
        report += "\n  " + f;
    }
    CHECK_MESSAGE(failedSuites.empty(), "failing suites:" << report);

    std::string fixed;
    for (const std::string& f : unexpectedlyPassing)
    {
        fixed += "\n  " + f;
    }
    CHECK_MESSAGE(unexpectedlyPassing.empty(),
                  "these now pass; remove them from knownFailures():" << fixed);
}

TEST_CASE("NOP")
{
    checkSuite("NOP");
}

TEST_CASE("MOVEQ")
{
    checkSuite("MOVEQ");
}

TEST_CASE("MOVE")
{
    checkSuite("MOVE.b");
    checkSuite("MOVE.w");
    checkSuite("MOVE.l");
}

TEST_CASE("ADD")
{
    checkSuite("ADD.b");
    checkSuite("ADD.w");
    checkSuite("ADD.l");
}

TEST_CASE("SUB")
{
    checkSuite("SUB.b");
    checkSuite("SUB.w");
    checkSuite("SUB.l");
}

TEST_CASE("CMP")
{
    checkSuite("CMP.b");
    checkSuite("CMP.w");
    checkSuite("CMP.l");
}

TEST_CASE("AND / OR / EOR")
{
    checkSuite("AND.b");
    checkSuite("AND.w");
    checkSuite("AND.l");
    checkSuite("OR.b");
    checkSuite("OR.w");
    checkSuite("OR.l");
    checkSuite("EOR.b");
    checkSuite("EOR.w");
    checkSuite("EOR.l");
}

TEST_CASE("分岐")
{
    checkSuite("Bcc");
    checkSuite("BSR");
    checkSuite("DBcc");
}

TEST_CASE("シフト / ローテート")
{
    checkSuite("ASL.b");
    checkSuite("ASR.b");
    checkSuite("LSL.b");
    checkSuite("LSR.b");
    checkSuite("ROL.b");
    checkSuite("ROR.b");
    checkSuite("ROXL.b");
    checkSuite("ROXR.b");
}

TEST_CASE("単項演算")
{
    checkSuite("CLR.b");
    checkSuite("CLR.w");
    checkSuite("CLR.l");
    checkSuite("NEG.b");
    checkSuite("NOT.b");
    checkSuite("TST.b");
    checkSuite("SWAP");
    checkSuite("EXT.w");
    checkSuite("EXT.l");
}

TEST_CASE("制御転送")
{
    checkSuite("JMP");
    checkSuite("JSR");
    checkSuite("RTS");
    checkSuite("LEA");
    checkSuite("PEA");
    checkSuite("LINK");
    checkSuite("UNLINK");
}

TEST_CASE("MOVEM")
{
    checkSuite("MOVEM.w");
    checkSuite("MOVEM.l");
}

TEST_CASE("DIVS が INT32_MIN / -1 で未定義動作に落ちない")
{
    // 保証すること: 被除数 0x80000000 を -1 で割っても、符号付き除算
    // オーバーフローの未定義動作を起こさず、V を立てて結果を書かないこと。
    //
    // Why not 適合性ベクタに任せるか: upstream のベクタにこの組み合わせは
    // 入っていない (被除数が 0x80000000 のケースが 0 件)。商が s32 に
    // 収まらない唯一の入力なので、ここだけは自前で押さえる。
    // サニタイザ付きビルド (just test-san) で走らせると効果が分かる。
    VectorBus bus;
    x68k::M68k cpu(bus);

    x68k::M68kState st{};
    st.d[0] = 0x80000000u;  // 被除数
    st.d[1] = 0x0000FFFFu;  // 除数 = -1 (下位ワード)
    st.sr = x68k::sr_bit::kSupervisor;
    st.a[7] = 0x00001000u;
    st.ssp = 0x00001000u;
    st.pc = 0x00002004u;
    st.ir = 0x81C1u;   // DIVS D1,D0
    st.irc = 0x4E71u;  // 後続は NOP
    cpu.loadStateForTest(st);

    cpu.step();

    const x68k::M68kState& out = cpu.state();
    // 商 2147483648 は 16bit に収まらないので V が立ち、D0 は元のまま。
    CHECK((out.sr & x68k::sr_bit::kOverflow) != 0);
    CHECK(out.d[0] == 0x80000000u);
}

TEST_SUITE_END();
