// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MC68000 インタプリタ。
//
// 実装方針:
//   - デコードは「命令語の上位 4bit で 1 次分岐 → グループ内で switch」の 2 段。
//     65536 エントリの関数ポインタ表 (256KB) は ESP32-S3 の内部 SRAM 512KB に対して
//     重すぎるため採らない。
//   - 未実装の命令に当たったら halted を立てて止める。IPL-ROM を走らせて
//     「落ちた命令から実装する」という開発ループを回すため。仕様書を先に読み込むより速い。
//   - サイクル数は命令ごとの固定値 + 実効アドレス計算の加算という粒度。
//     バスサイクル単位の精度は追わない (Human68k のコンソールには不要で、
//     ESP32-S3 の性能に余裕がない)。
//
// プリフェッチについて:
//   68000 は実行中の命令の先を 2 ワード読んでいる。テストベクタが初期状態・
//   最終状態としてプリフェッチキューを持つため、エミュレータ側も同じ形で
//   保持しないと突き合わせられない。ir が現在の命令語、irc が次のワード。

#ifndef X68K_CORE_CPU_M68K_H
#define X68K_CORE_CPU_M68K_H

#include "m68k_types.h"

// ホットパスを内部 SRAM (IRAM) へ置くための印。
//
// ESP32-S3 では実行コードの既定の置き場所が Flash で、キャッシュミスのたびに
// SPI 越しの読み出しが挟まる。命令ディスパッチのように「毎命令必ず通る」
// 関数はここが効く。IRAM へ置けばキャッシュを介さず内部 SRAM から直接実行
// できるので、ミスの分がまるごと消える。
//
// Why not <esp_attr.h> の IRAM_ATTR をそのまま使うか: core/ は ESP32 非依存
// でなければならず (ホストでテストとエミュレータを回すのが開発速度の前提)、
// esp_ 系のヘッダを含めた時点で core-guard が落ちる。IRAM_ATTR の実体は
// セクション属性 1 つなので、同じものを自前で書けば依存を持ち込まずに済む。
//
// Why not ESP_PLATFORM ではなく __XTENSA__ で分けるか: __XTENSA__ は Xtensa
// 向けのツールチェーンなら何でも立つ。ESP-IDF のリンカスクリプトが無い環境で
// .iram1 を指定すると配置先の無いセクションになる。ESP-IDF が必ず定義する
// ESP_PLATFORM を使えば「IDF でビルドされている」ことを直接表せる。
//
// ホストでは空に展開されるので、テストもエミュレータも今までどおり動く。
#if defined(ESP_PLATFORM)
#define X68K_HOT_PATH __attribute__((section(".iram1")))
#else
#define X68K_HOT_PATH
#endif

namespace x68k
{

class M68k
{
public:
    explicit M68k(Bus& bus) : bus_(bus) {}

    // 68000 の外部アドレスバスは 24bit。上位 8bit は出ないので、
    // アドレス計算の結果は必ずここで折り返す。
    static constexpr u32 kAddrMask = 0x00FFFFFFu;

    // ベクタ $000 から SSP、$004 から PC を読んで初期化する。
    void reset();

    // 命令を 1 つ実行し、消費したサイクル数を返す。
    // halted または stopped の場合は何もせず 0 を返す。
    X68K_HOT_PATH u32 step();

    // 割り込みを要求する。level は 1-7 (7 はマスク不可)。
    // 実際に受け付けられるかは SR の割り込みマスクによる。
    //
    // vectorNumber に 0 以外を渡すと、自動ベクタ (24+level) ではなく
    // その番号のベクタを使う。X68000 の MFP は自分のベクタ番号を返す
    // デバイスなので、これを使わないと未定義割り込みのハンドラへ飛んでしまう。
    void requestInterrupt(u32 level, u32 vectorNumber = 0);

    // RESET 命令が実行されたときに呼ばれる。
    //
    // 68000 の RESET は RESET 信号を外部へ出すだけで CPU 自身は何もしないが、
    // X68000 ではこれを受けてメモリコントローラが $000000 の ROM 写像を解除する。
    // IPL-ROM は起動直後にこれを実行して通常のメモリ配置へ切り替える。
    // 機種固有の反応なので、CPU からは外へ通知するだけにする。
    using ResetCallback = void (*)(void* context);
    void setResetCallback(ResetCallback callback, void* context)
    {
        resetCallback_ = callback;
        resetContext_ = context;
    }

    [[nodiscard]] M68kState& state()
    {
        return st_;
    }
    [[nodiscard]] const M68kState& state() const
    {
        return st_;
    }

    // SR を書き換える。S ビットが変わる場合は A7 を USP/SSP と入れ替える。
    // 命令実装から直接 sr を代入すると A7 の切り替えを忘れるので、必ずこれを通す。
    void setSr(u16 value);

    // テストベクタから状態を流し込むときに使う。プリフェッチも含めて外から
    // 完全に指定したいので、setSr のような副作用を挟まない。
    void loadStateForTest(const M68kState& s);

    // 仮想関数を通さずに直接触ってよいメインメモリの窓を教える。
    //
    // Why これが要るか: Bus::read16 は virtual なので、命令フェッチとオペランド
    // 読みのたびに vtable 経由の間接呼び出しになる。呼び先が確定しないため
    // インライン展開もできず、短い関数なのに呼び出し規約の分だけ必ず払う。
    // CoreS3 の実測ではスライスの 80% が run() の中で、その大半がこの経路。
    //
    // 窓は「先頭から length バイトが base の指す配列そのもの」であること。
    // SystemBus が同じ配列を指しているので、DMA (Machine::dmaMemRead) が
    // バス経由で触っても同じ実体に当たる。写しではないのでコヒーレンシの
    // 問題が起きない。
    //
    // Why not CPU に写しを持たせないか: DMAC と CPU が同じ番地を触るので、
    // 写しにすると SASI の転送結果が CPU から見えない (あるいはその逆)。
    // ポインタを共有する形なら、そもそも同期する対象が無い。
    //
    // Why not M68k をバス型でテンプレート化しないか: テストベクタの検証は
    // 疎な連想配列のバス実装を渡して回している (test/test_m68k_vectors.cpp)。
    // テンプレート化すると CPU コアのインスタンスがバス実装ごとに増え、
    // 「CPU だけを切り離して検証する」という Bus の存在意義が薄れる。
    // 窓を教えるだけなら、教えなければ今までどおり全部が仮想関数を通る。
    //
    // romAtZero は「$000000 に IPL-ROM が写像されている」間 true。写像中は
    // 窓の読み出しが RAM ではなく ROM 側に当たるので、fast path を止める。
    void setFastRam(u8* base, u32 length)
    {
        fastRam_ = base;
        fastRamLimit_ = base != nullptr ? length : 0;
    }

    // $000000 の ROM 写像が外れたかどうかを伝える。
    //
    // 写像中は読み出しが ROM に当たるため、RAM の窓を使ってはいけない。
    // SystemBus::setRomMappedAtZero と必ず対で呼ぶ。
    void setFastRamReadable(bool readable)
    {
        fastRamReadable_ = readable;
    }

    // 仮想関数を通さずに読んでよい IPL-ROM の窓を教える。
    //
    // Why メイン RAM と別に持つか: CPU のメモリアクセスを実際に数えたら、
    // IPL-ROM が全体の 79% で最頻だった (メイン RAM は 17.7%)。
    // 内訳は docs/knowledge/cores3-emulator-runtime.md にある。
    // 起動処理も IOCS も本体は ROM 内にあり、命令フェッチがそこへ集中する。
    //
    // ROM は書けないので読み出しだけを通す。busBase は「この窓の先頭が
    // 68000 のアドレス空間のどこに見えるか」で、X68000 では $FE0000。
    //
    // Why not アドレスを m68k.h に定数で持たないか: 68000 コアは機種の
    // アドレス配置 (memmap.h) を知らないでいるべきで、その独立性こそ
    // Bus を挟んでいる理由。窓の位置は必ず SystemBus から教わる。
    void setFastRom(const u8* base, u32 busBase, u32 length)
    {
        fastRom_ = base;
        fastRomBase_ = busBase;
        fastRomLength_ = base != nullptr ? length : 0;
    }

private:
    // fast path を通してよいアクセスか。
    //
    // 2 バイトとも窓に収まることを見る。境界をまたぐワードは
    // 配列の外を触るので、必ず遅い経路 (バス) に落とす。
    [[nodiscard]] bool fastRamHasWord(u32 a) const
    {
        return fastRam_ != nullptr && a + 1 < fastRamLimit_;
    }
    [[nodiscard]] bool fastRamHasByte(u32 a) const
    {
        return fastRam_ != nullptr && a < fastRamLimit_;
    }

    // IPL-ROM の窓に size バイトとも収まるか。収まれば ROM 内オフセットを返す。
    // またぐアクセスは遅い経路 (バス) に落として境界を正しく見せる。
    [[nodiscard]] bool fastRomHas(u32 a, u32 size, u32& offsetOut) const
    {
        if (fastRom_ == nullptr || a < fastRomBase_)
        {
            return false;
        }
        const u32 off = a - fastRomBase_;
        if (off + size > fastRomLength_)
        {
            return false;
        }
        offsetOut = off;
        return true;
    }

    // --- プリフェッチ --------------------------------------------------------
    // 命令語を 1 ワード取り出し、キューを 1 つ進める。
    //
    // **全命令が必ず 1 回以上通る**。中身はキューをずらして 1 ワード読むだけ
    // なので、.cpp 側に置くと実体より呼び出しの方が高くつく。
    //
    // 読み出し先は IPL-ROM かメイン RAM のどちらか (実測で ROM 79% /
    // RAM 17.7%)。その 2 つは窓が張ってあるので、ここで直接引く。
    // 窓の外 (I/O やバスエラー領域) から命令を読むことは通常起きないが、
    // 起きたときのために .cpp の read16 へ落とす。
    u16 fetch()
    {
        const u16 value = st_.ir;
        st_.ir = st_.irc;

        // PC は既に「次に読むアドレス」を指している。
        const u32 a = st_.pc & kAddrMask;
        if (fastRamReadable_ && fastRam_ != nullptr && a + 1 < fastRamLimit_)
        {
            st_.irc = static_cast<u16>((fastRam_[a] << 8) | fastRam_[a + 1]);
        }
        else if (u32 off = 0; fastRomHas(a, 2, off))
        {
            st_.irc = static_cast<u16>((fastRom_[off] << 8) | fastRom_[off + 1]);
        }
        else
        {
            st_.irc = read16(a);
        }
        st_.pc = st_.pc + 2;
        return value;
    }
    // プリフェッチキューを PC の位置から埋め直す (分岐後など)。
    X68K_HOT_PATH void refillPrefetch(u32 newPc);

    // --- メモリアクセス ------------------------------------------------------
    // ワード/ロングの奇数アドレスアクセスはアドレスエラーになる。
    //
    // 命令フェッチとオペランドの読み書きが全部ここを通る。実測で 1 スライス
    // (20000 サイクル) の 8 割が run() の中なので、この経路が最も効く。
    X68K_HOT_PATH u8 read8(u32 addr);
    X68K_HOT_PATH u16 read16(u32 addr);
    X68K_HOT_PATH u32 read32(u32 addr);
    X68K_HOT_PATH void write8(u32 addr, u8 value);
    X68K_HOT_PATH void write16(u32 addr, u16 value);
    X68K_HOT_PATH void write32(u32 addr, u32 value);

    // --- 例外 ----------------------------------------------------------------
    // 積む PC の基準が 2 通りある。TRAP / CHK / DIVU の 0 除算 / 割り込みは
    // 「次の命令」を積み、不当命令・A-line・F-line・特権違反は「例外を起こした
    // 命令そのもの」を積む。後者は faulting = true。
    void takeException(u32 vectorNumber, bool faulting = false);
    void takeAddressError(u32 addr, bool isRead);
    void takeBusError(u32 addr, bool isRead);
    // アドレスエラーとバスエラーは同じ 14 バイトフレームを積む。
    // 違うのはベクタ番号だけなので共通化する。
    void takeGroup0Exception(u32 vectorNumber, u32 addr, bool isRead);
    [[nodiscard]] bool requirePrivilege();

    // --- 実効アドレス --------------------------------------------------------
    // mode/reg から実効アドレスを計算する。size はディスプレースメント計算と
    // -(An)/(An)+ の増減幅に効く (バイトで A7 を触ると 2 増減する特例がある)。
    X68K_HOT_PATH u32 effectiveAddress(u32 mode, u32 reg, u32 size);
    // レジスタ直接 (mode 0/1) だけをここで捌き、それ以外は .cpp の
    // 実効アドレス計算へ回す。
    //
    // MOVE は全命令の 19.2% で、その転送元・転送先はレジスタ直接が最頻。
    // プロファイルでは groupMove の readEa 行 (179) と writeEa 行 (151) が
    // 命令実装の中で最大の 2 項目だった。どちらも .cpp 側にあり、
    // ESP32-S3 では実呼び出しになる。
    //
    // Why not 全部インラインにしないか: mode 2-7 は拡張ワードの読み出しや
    // ポインタの増減を含み、展開すると呼び出し側 (各命令グループ) が
    // 一斉に膨らむ。I-cache を押し出して逆効果になりうる。
    // レジスタ直接は「配列を 1 つ引いて型を切る」だけなので、
    // 呼び出しの方が高くつく。
    //
    // size は m68k_alu.h の kByte=1 / kWord=2 / kLong=4 (バイト数)。
    // ここは m68k_alu.h を include せずに済ませたいので数値で書くが、
    // **0/1/2 ではない**。一度そう思い込んで書き、適合性ベクタが
    // 25 件落ちた (MOVE.b の転送元が常に 0 になった)。
    u32 readEa(u32 mode, u32 reg, u32 size)
    {
        if (mode == 0)
        {
            const u32 v = st_.d[reg];
            if (size == 1)  // kByte
            {
                return v & 0xFFu;
            }
            if (size == 2)  // kWord
            {
                return v & 0xFFFFu;
            }
            return v;
        }
        if (mode == 1)
        {
            // An はワード指定でも符号拡張された 32bit として読まれる。
            const u32 v = st_.a[reg];
            if (size == 2)  // kWord
            {
                return static_cast<u32>(static_cast<s32>(static_cast<s16>(v)));
            }
            return v;
        }
        return readEaSlow(mode, reg, size);
    }

    void writeEa(u32 mode, u32 reg, u32 size, u32 value)
    {
        if (mode == 0)
        {
            // Dn への書き込みはサイズぶんだけを差し替える。上位は保存される。
            u32& d = st_.d[reg];
            if (size == 1)  // kByte
            {
                d = (d & 0xFFFFFF00u) | (value & 0xFFu);
                return;
            }
            if (size == 2)  // kWord
            {
                d = (d & 0xFFFF0000u) | (value & 0xFFFFu);
                return;
            }
            d = value;
            return;
        }
        if (mode == 1)
        {
            // An への書き込みは常に 32bit。ワード指定なら符号拡張される。
            st_.a[reg] =
                size == 2 ? static_cast<u32>(static_cast<s32>(static_cast<s16>(value))) : value;
            return;
        }
        writeEaSlow(mode, reg, size, value);
    }

    X68K_HOT_PATH u32 readEaSlow(u32 mode, u32 reg, u32 size);
    X68K_HOT_PATH void writeEaSlow(u32 mode, u32 reg, u32 size, u32 value);
    // 書き込み先の実効アドレスを一度だけ計算して使い回すための版。
    // ADD.b (An)+,D0 のように「読んでから同じ場所へ書く」命令で、
    // ポインタを二重に進めてしまう事故を防ぐ。
    u32 readEaForModify(u32 mode, u32 reg, u32 size, u32& addrOut);
    void writeEaToAddr(u32 mode, u32 reg, u32 size, u32 addr, u32 value);

    // --- フラグ --------------------------------------------------------------
    X68K_HOT_PATH void setLogicFlags(u32 value, u32 size);
    [[nodiscard]] X68K_HOT_PATH bool testCondition(u32 cond) const;

    // --- 命令グループ --------------------------------------------------------
    // 戻り値は消費サイクル数。未実装なら halted を立てて 0 を返す。
    // どのグループを IRAM へ置くかは、ホストの `just run --stats` で採った
    // 実行頻度で決めた (IPL-ROM から Human68k のプロンプトまで 2 億サイクル、
    // 2055 万命令)。内訳は misc 25.2% / 分岐 22.3% / 即値 13.8% /
    // MOVE.b 11.5% / MOVE.w 4.7% / ADDQ 等 3.8% / AND・MUL 3.7% /
    // ADD 3.5% / MOVE.l 3.0% / シフト 2.8% / CMP・EOR 2.0% / MOVEQ 1.9%。
    // 上位 4 つで 7 割を超えるが、groupMove は 3 サイズが 1 関数を共有する
    // ので合わせて 19.2% になる。残りの OR/DIV と SUB は 1% 未満なので
    // Flash に置いたままにして IRAM を節約する。
    X68K_HOT_PATH u32 execute(u16 op);
    X68K_HOT_PATH u32 groupMove(u16 op, u32 size);  // 19.2% (b/w/l 合計)
    X68K_HOT_PATH u32 groupImmediate(u16 op);  // 0000: ORI/ANDI/SUBI/ADDI/EORI/CMPI/BTST 等 13.8%
    X68K_HOT_PATH u32 groupMisc(u16 op);  // 0100: MOVEM/LEA/JMP/JSR/CLR/NEG/NOT/TST/EXT 等 25.2%
    X68K_HOT_PATH u32 groupQuickAlu(u16 op);  // 0101: ADDQ/SUBQ/Scc/DBcc 3.8%
    X68K_HOT_PATH u32 groupBranch(u16 op);    // 0110: Bcc/BRA/BSR 22.3%
    X68K_HOT_PATH u32 groupMoveq(u16 op);     // 0111 1.9%
    u32 groupOrDiv(u16 op);                   // 1000: OR/DIVU/DIVS/SBCD 0.9% (Flash のまま)
    u32 groupSub(u16 op);                     // 1001: SUB/SUBA/SUBX 0.9% (Flash のまま)
    X68K_HOT_PATH u32 groupCmpEor(u16 op);    // 1011: CMP/CMPA/CMPM/EOR 2.0%
    X68K_HOT_PATH u32 groupAndMul(u16 op);    // 1100: AND/MULU/MULS/ABCD/EXG 3.7%
    X68K_HOT_PATH u32 groupAdd(u16 op);       // 1101: ADD/ADDA/ADDX 3.5%
    // ABCD と SBCD。命令語の形式が同じで補正の向きだけが違うのでまとめる。
    // memoryMode は -(Ay),-(Ax) 形式か、isAdd は ABCD か SBCD か。
    u32 execBcdAddSub(u16 op, bool memoryMode, bool isAdd);

    X68K_HOT_PATH u32 groupShift(u16 op);  // 2.8%
    // メモリに対する 1 ビットシフト。命令語の形式がレジスタ版と違う。
    u32 memoryShift(u16 op);  // 1110: ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR

    // 未実装命令に当たったときの共通処理。
    u32 unimplemented(u16 op);

    Bus& bus_;
    // 仮想関数を通さずに触れるメインメモリ。所有しない (bus_ と同じ実体を指す)。
    u8* fastRam_ = nullptr;
    u32 fastRamLimit_ = 0;
    // 仮想関数を通さずに読める IPL-ROM。所有しない。
    const u8* fastRom_ = nullptr;
    u32 fastRomBase_ = 0;
    u32 fastRomLength_ = 0;
    // $000000 の ROM 写像が外れているか。写像中は読み出しを bus_ に任せる。
    bool fastRamReadable_ = false;
    M68kState st_;
    // 保留中の割り込みレベル (0 = なし)。
    u32 pendingIrq_ = 0;
    // 保留中の割り込みが使うベクタ番号 (0 = 自動ベクタ)。
    u32 pendingVector_ = 0;
    // アドレスエラー処理の入れ子の深さ。
    // ハンドラのベクタ自体が奇数を指す場合など、実機でも入れ子は起きる。
    // 無限に潜ると SP を食い潰すだけなので段数で打ち切る。
    int addressErrorDepth_ = 0;

    // RESET 命令の通知先。
    ResetCallback resetCallback_ = nullptr;
    void* resetContext_ = nullptr;
};

}  // namespace x68k

#endif  // X68K_CORE_CPU_M68K_H
