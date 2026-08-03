// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// MC68000 インタプリタの基盤部分。命令の実装は m68k_ops_*.cpp にある。

#include "m68k.h"

namespace x68k
{
namespace
{

// アドレスバスは 24bit。上位 8bit は出力されない。
//
// 重要: このマスクは「バスへ出す瞬間」にだけ適用する。
// アドレスレジスタと PC は 32bit をそのまま保持する (68000 の内部は 32bit で、
// 外へ出るときに上位 8bit が捨てられるだけ)。レジスタ側でマスクすると
// A0 に $F9C321E4 を入れて読み出したときに $00C321E4 が返るという
// 実機と違う挙動になり、テストベクタが軒並み落ちる。
constexpr u32 kAddrMask = 0x00FFFFFFu;

// サイズ指定の内部表現。1/2/4 バイトをそのまま使う。
constexpr u32 kByte = 1;
constexpr u32 kWord = 2;
constexpr u32 kLong = 4;

// 値をサイズに合わせて切り詰める。
constexpr u32 truncate(u32 value, u32 size)
{
    if (size == kByte)
    {
        return value & 0xFFu;
    }
    if (size == kWord)
    {
        return value & 0xFFFFu;
    }
    return value;
}

// サイズの最上位ビット。符号判定に使う。
constexpr u32 signBit(u32 size)
{
    return 1u << (size * 8 - 1);
}

}  // namespace

// --- リセット --------------------------------------------------------------

void M68k::reset()
{
    st_ = M68kState{};
    // リセット時は特権モードで割り込みマスク 7。
    st_.sr = sr_bit::kSupervisor | sr_bit::kIntMask;

    st_.ssp = read32(0);
    st_.a[7] = st_.ssp;
    const u32 pc = read32(4);
    refillPrefetch(pc);

    pendingIrq_ = 0;
}

// --- SR の書き換え ---------------------------------------------------------

void M68k::setSr(u16 value)
{
    const bool wasSupervisor = st_.isSupervisor();
    // 68000 が実装していないビットは常に 0。
    const u16 next = value & sr_bit::kImplemented;
    const bool nowSupervisor = (next & sr_bit::kSupervisor) != 0;

    if (wasSupervisor != nowSupervisor)
    {
        // A7 は現在有効なスタックポインタを指す。特権が切り替わる瞬間に
        // 控えと入れ替える。ここを忘れるとユーザスタックと
        // スーパーバイザスタックが混ざり、原因の分かりにくい暴走になる。
        if (wasSupervisor)
        {
            st_.ssp = st_.a[7];
            st_.a[7] = st_.usp;
        }
        else
        {
            st_.usp = st_.a[7];
            st_.a[7] = st_.ssp;
        }
    }

    st_.sr = next;
}

void M68k::loadStateForTest(const M68kState& s)
{
    st_ = s;
    pendingIrq_ = 0;
}

// --- プリフェッチ ----------------------------------------------------------

// PC の定義について:
//   ir と irc は既に読み込み済みのワードで、PC は「次に読むアドレス」を指す。
//   つまり ir が命令語 X にあるとき PC は X+4 になる。
//   テストベクタの pc は MAME の m_au 由来で同じ定義なので、これに合わせている。
//   ここを 1 ワードずらすと、分岐先も PC 相対アドレッシングも全部ずれる。

u16 M68k::fetch()
{
    const u16 value = st_.ir;
    st_.ir = st_.irc;
    // PC は既に「次に読むアドレス」なので、そこから読んで 1 ワード進める。
    st_.irc = read16(st_.pc);
    st_.pc = st_.pc + 2;
    return value;
}

void M68k::refillPrefetch(u32 newPc)
{
    // 奇数アドレスへの分岐はアドレスエラー。
    //
    // Why ここで先に判定するか: read16 の中で例外に入ると、PC を設定する前に
    // PC を使う順序になり、例外フレームに壊れた PC が積まれる。
    if ((newPc & 1) != 0)
    {
        takeAddressError(newPc, true);
        return;
    }

    // PC は 32bit のまま保持し、バスへ出すときだけ 24bit にする。
    //
    // X68000 の IOCS は未初期化のベクタに「上位バイト = ベクタ番号」を埋めて
    // おり ($43FF0540 のような値)、そこへ飛ぶと上位バイトが付いたまま PC に
    // 入る。実機のアドレスバスは 24bit なのでフェッチ先は $FF0540 になり、
    // IOCS 側はそこで「不正ベクタ」として処理する仕組み。
    // PC 自体を丸めてしまうとテストベクタ (32bit の PC を期待する) が
    // 落ちるので、丸めるのはアクセスの瞬間だけにする。
    st_.ir = bus_.read16(newPc & kAddrMask);
    st_.irc = bus_.read16((newPc + 2) & kAddrMask);
    st_.pc = newPc + 4;
}

// --- メモリアクセス --------------------------------------------------------

u8 M68k::read8(u32 addr)
{
    return bus_.read8(addr & kAddrMask);
}

u16 M68k::read16(u32 addr)
{
    const u32 a = addr & kAddrMask;
    if ((a & 1) != 0)
    {
        takeAddressError(a, true);
        return 0;
    }
    return bus_.read16(a);
}

u32 M68k::read32(u32 addr)
{
    const u32 a = addr & kAddrMask;
    if ((a & 1) != 0)
    {
        takeAddressError(a, true);
        return 0;
    }
    // 68000 のバスは 16bit なのでロングは 2 回に分かれる。上位が先。
    const u32 hi = bus_.read16(a);
    const u32 lo = bus_.read16((a + 2) & kAddrMask);
    return (hi << 16) | lo;
}

void M68k::write8(u32 addr, u8 value)
{
    bus_.write8(addr & kAddrMask, value);
}

void M68k::write16(u32 addr, u16 value)
{
    const u32 a = addr & kAddrMask;
    if ((a & 1) != 0)
    {
        takeAddressError(a, false);
        return;
    }
    bus_.write16(a, value);
}

void M68k::write32(u32 addr, u32 value)
{
    const u32 a = addr & kAddrMask;
    if ((a & 1) != 0)
    {
        takeAddressError(a, false);
        return;
    }
    bus_.write16(a, static_cast<u16>(value >> 16));
    bus_.write16((a + 2) & kAddrMask, static_cast<u16>(value & 0xFFFFu));
}

// --- 例外 ------------------------------------------------------------------

void M68k::takeException(u32 vectorNumber)
{
    const u16 oldSr = st_.sr;

    // 例外処理は必ず特権モードで行う。S を立ててから積むことで、
    // スタックポインタがスーパーバイザ側に切り替わる。
    setSr(static_cast<u16>((oldSr | sr_bit::kSupervisor) & ~sr_bit::kTrace));

    // スタックフレーム: PC (ロング) と SR (ワード) を積む。
    //
    // 積む PC は「例外を起こした命令の次」。プリフェッチの分だけ手前に戻す
    // (アドレスエラーの framePc と同じ理屈)。
    //
    // ここは bus_ を直に叩く。write32 経由だと例外処理の途中でアドレスエラー
    // 判定に入り、フレームが二重に積まれて SP が壊れる。
    //
    // 重要: アドレスに 0x00FFFFFE のマスクをかけてはいけない。RTE 側は
    // read32(a7) で読み戻すので、書き込み位置と読み出し位置が食い違う。
    // 実際これで戻り先の上位バイトにゴミが乗り、PC が壊れていた。
    const u32 framePc = st_.pc - 4;
    st_.a[7] = st_.a[7] - 4;
    const u32 pcSlot = st_.a[7] & 0x00FFFFFFu;
    bus_.write16(pcSlot, static_cast<u16>(framePc >> 16));
    bus_.write16((pcSlot + 2) & 0x00FFFFFFu, static_cast<u16>(framePc & 0xFFFFu));
    st_.a[7] = st_.a[7] - 2;
    bus_.write16(st_.a[7] & 0x00FFFFFFu, oldSr);

    const u32 vectorAddr = vectorNumber * 4;
    const u32 handler =
        (static_cast<u32>(bus_.read16(vectorAddr)) << 16) | bus_.read16(vectorAddr + 2);
    refillPrefetch(handler);
}

void M68k::takeAddressError(u32 addr, bool isRead)
{
    // アドレスエラーの入れ子は実機でも起きる (ハンドラのベクタ自体が奇数を指す等)。
    // ただし無限に潜ると SP を食い潰すだけなので、段数で打ち切る。
    // 実機はここでダブルバスフォルトとなり停止する。
    constexpr int kMaxNesting = 2;
    if (addressErrorDepth_ >= kMaxNesting)
    {
        st_.halted = true;
        return;
    }
    ++addressErrorDepth_;

    // グループ 0 例外は 7 ワード (14 バイト) の拡張スタックフレームを積む。
    // 積む順序は下位アドレス側から: status word / アクセスアドレス / 命令語 / SR / PC。
    const u16 oldSr = st_.sr;
    setSr(static_cast<u16>((oldSr | sr_bit::kSupervisor) & clearMask(sr_bit::kTrace)));

    // フレームに積む PC は「例外を起こした命令の次のワード」。
    //
    // PC の位置関係: step() が命令語を fetch した時点で 1 ワード進み、
    // さらにプリフェッチが 2 ワード先読みしている。よって命令の終端は pc - 4。
    const u32 framePc = st_.pc - 4;

    // フレームの書き込みは bus_ を直に叩く。write16 経由だと、SP が奇数のときに
    // 再びアドレスエラー判定へ入って無限に潜る。
    st_.a[7] = st_.a[7] - 4;
    bus_.write16(st_.a[7] & 0x00FFFFFFu, static_cast<u16>(framePc >> 16));
    bus_.write16((st_.a[7] + 2) & 0x00FFFFFFu, static_cast<u16>(framePc & 0xFFFFu));
    st_.a[7] = st_.a[7] - 2;
    bus_.write16(st_.a[7] & 0x00FFFFFFu, oldSr);
    st_.a[7] = st_.a[7] - 2;
    bus_.write16(st_.a[7] & 0x00FFFFFFu, st_.ir);
    st_.a[7] = st_.a[7] - 4;
    bus_.write16(st_.a[7] & 0x00FFFFFFu, static_cast<u16>(addr >> 16));
    bus_.write16((st_.a[7] + 2) & 0x00FFFFFFu, static_cast<u16>(addr & 0xFFFFu));
    st_.a[7] = st_.a[7] - 2;
    // 機能コードとアクセス種別。R/W ビットは読み出しで 1。
    const u16 info = static_cast<u16>((isRead ? 0x0010u : 0x0000u) | 0x0005u);
    bus_.write16(st_.a[7] & 0x00FFFFFFu, info);

    // ベクタの読み出しは bus_ を直に叩く (read32 を使うと奇数判定でここへ再入する)。
    // ハンドラへの分岐は refillPrefetch を通す。分岐先が奇数なら入れ子の
    // アドレスエラーになるのが実機の挙動で、上の段数ガードが打ち切る。
    const u32 vectorAddr = vector::kAddressError * 4;
    const u32 handler =
        (static_cast<u32>(bus_.read16(vectorAddr)) << 16) | bus_.read16(vectorAddr + 2);
    refillPrefetch(handler);

    --addressErrorDepth_;
}

bool M68k::requirePrivilege()
{
    if (st_.isSupervisor())
    {
        return true;
    }
    takeException(vector::kPrivilegeViolation);
    return false;
}

// --- 実効アドレス ----------------------------------------------------------

u32 M68k::effectiveAddress(u32 mode, u32 reg, u32 size)
{
    switch (mode)
    {
        case 2:  // (An)
            return st_.a[reg];

        case 3:  // (An)+
        {
            const u32 addr = st_.a[reg];
            // A7 をバイト単位で触るときは 2 増減する。スタックポインタが
            // 奇数になるとアドレスエラーになるための特例。
            const u32 step = (reg == 7 && size == kByte) ? 2u : size;
            st_.a[reg] = addr + step;
            return addr;
        }

        case 4:  // -(An)
        {
            const u32 step = (reg == 7 && size == kByte) ? 2u : size;
            st_.a[reg] = st_.a[reg] - step;
            return st_.a[reg];
        }

        case 5:  // (d16,An)
        {
            const s16 disp = static_cast<s16>(fetch());
            return st_.a[reg] + static_cast<u32>(static_cast<s32>(disp));
        }

        case 6:  // (d8,An,Xn)
        {
            const u16 ext = fetch();
            const u32 xn = (ext & 0x8000u) != 0 ? st_.a[(ext >> 12) & 7u] : st_.d[(ext >> 12) & 7u];
            // 拡張ワードの bit11 が 0 ならインデックスはワードサイズ (符号拡張)。
            const u32 index = (ext & 0x0800u) != 0
                                  ? xn
                                  : static_cast<u32>(static_cast<s32>(static_cast<s16>(xn)));
            const s8 disp = static_cast<s8>(ext & 0xFFu);
            return st_.a[reg] + index + static_cast<u32>(static_cast<s32>(disp));
        }

        case 7:
            switch (reg)
            {
                case 0:  // (xxx).W: 符号拡張される
                    return static_cast<u32>(static_cast<s32>(static_cast<s16>(fetch())));

                case 1:  // (xxx).L
                {
                    const u32 hi = fetch();
                    const u32 lo = fetch();
                    return (hi << 16) | lo;
                }

                case 2:  // (d16,PC)
                {
                    // PC 相対の基準は拡張ワード自身のアドレス。
                    //
                    // ここに来た時点で PC は「拡張ワードの次のワード」を指している。
                    // step() が命令語を fetch() した時点で 1 ワード進み、プリフェッチ
                    // 2 ワードぶんの先読みと合わせて、拡張ワードは pc - 4 にある。
                    const u32 base = st_.pc - 4;
                    const s16 disp = static_cast<s16>(fetch());
                    return base + static_cast<u32>(static_cast<s32>(disp));
                }

                case 3:  // (d8,PC,Xn)
                {
                    const u32 base = st_.pc - 4;
                    const u16 ext = fetch();
                    const u32 xn =
                        (ext & 0x8000u) != 0 ? st_.a[(ext >> 12) & 7u] : st_.d[(ext >> 12) & 7u];
                    const u32 index =
                        (ext & 0x0800u) != 0
                            ? xn
                            : static_cast<u32>(static_cast<s32>(static_cast<s16>(xn)));
                    const s8 disp = static_cast<s8>(ext & 0xFFu);
                    return base + index + static_cast<u32>(static_cast<s32>(disp));
                }

                default:
                    break;
            }
            break;

        default:
            break;
    }

    // mode 0/1 (Dn/An) はアドレスを持たない。呼び出し側の誤りなので停止させる。
    st_.halted = true;
    return 0;
}

u32 M68k::readEa(u32 mode, u32 reg, u32 size)
{
    if (mode == 0)
    {
        return truncate(st_.d[reg], size);
    }
    if (mode == 1)
    {
        // An はワード指定でも符号拡張された 32bit として読まれる。
        return size == kWord ? static_cast<u32>(static_cast<s32>(static_cast<s16>(st_.a[reg])))
                             : st_.a[reg];
    }
    if (mode == 7 && reg == 4)
    {
        // #immediate
        if (size == kLong)
        {
            const u32 hi = fetch();
            const u32 lo = fetch();
            return (hi << 16) | lo;
        }
        const u32 value = fetch();
        // バイトの即値は下位バイトに入っている。
        return size == kByte ? (value & 0xFFu) : value;
    }

    const u32 addr = effectiveAddress(mode, reg, size);
    if (size == kByte)
    {
        return read8(addr);
    }
    if (size == kWord)
    {
        return read16(addr);
    }
    return read32(addr);
}

void M68k::writeEa(u32 mode, u32 reg, u32 size, u32 value)
{
    if (mode == 0)
    {
        // Dn への書き込みはサイズぶんだけを差し替える。上位は保存される。
        if (size == kByte)
        {
            st_.d[reg] = (st_.d[reg] & 0xFFFFFF00u) | (value & 0xFFu);
        }
        else if (size == kWord)
        {
            st_.d[reg] = (st_.d[reg] & 0xFFFF0000u) | (value & 0xFFFFu);
        }
        else
        {
            st_.d[reg] = value;
        }
        return;
    }
    if (mode == 1)
    {
        // An への書き込みは常に 32bit。ワード指定なら符号拡張される。
        st_.a[reg] =
            size == kWord ? static_cast<u32>(static_cast<s32>(static_cast<s16>(value))) : value;
        return;
    }

    const u32 addr = effectiveAddress(mode, reg, size);
    writeEaToAddr(mode, reg, size, addr, value);
}

u32 M68k::readEaForModify(u32 mode, u32 reg, u32 size, u32& addrOut)
{
    addrOut = 0;
    if (mode == 0)
    {
        return truncate(st_.d[reg], size);
    }
    if (mode == 1)
    {
        return st_.a[reg];
    }

    // ポインタの増減を伴うモードでは、ここで一度だけアドレスを確定させる。
    // 読みと書きで別々に effectiveAddress を呼ぶと (An)+ が二重に進む。
    addrOut = effectiveAddress(mode, reg, size);
    if (size == kByte)
    {
        return read8(addrOut);
    }
    if (size == kWord)
    {
        return read16(addrOut);
    }
    return read32(addrOut);
}

void M68k::writeEaToAddr(u32 mode, u32 reg, u32 size, u32 addr, u32 value)
{
    if (mode == 0)
    {
        writeEa(mode, reg, size, value);
        return;
    }
    if (mode == 1)
    {
        st_.a[reg] =
            size == kWord ? static_cast<u32>(static_cast<s32>(static_cast<s16>(value))) : value;
        return;
    }

    if (size == kByte)
    {
        write8(addr, static_cast<u8>(value));
    }
    else if (size == kWord)
    {
        write16(addr, static_cast<u16>(value));
    }
    else
    {
        write32(addr, value);
    }
}

// --- フラグ ----------------------------------------------------------------

void M68k::setLogicFlags(u32 value, u32 size)
{
    // 論理演算は V と C をクリアし、N と Z だけを結果から作る。X は変化しない。
    u16 sr = static_cast<u16>(
        st_.sr & ~(sr_bit::kNegative | sr_bit::kZero | sr_bit::kOverflow | sr_bit::kCarry));
    const u32 v = truncate(value, size);
    if (v == 0)
    {
        sr |= sr_bit::kZero;
    }
    if ((v & signBit(size)) != 0)
    {
        sr |= sr_bit::kNegative;
    }
    st_.sr = sr;
}

bool M68k::testCondition(u32 cond) const
{
    const bool c = (st_.sr & sr_bit::kCarry) != 0;
    const bool v = (st_.sr & sr_bit::kOverflow) != 0;
    const bool z = (st_.sr & sr_bit::kZero) != 0;
    const bool n = (st_.sr & sr_bit::kNegative) != 0;

    switch (cond)
    {
        case 0x0:
            return true;  // T
        case 0x1:
            return false;  // F
        case 0x2:
            return !c && !z;  // HI
        case 0x3:
            return c || z;  // LS
        case 0x4:
            return !c;  // CC (HS)
        case 0x5:
            return c;  // CS (LO)
        case 0x6:
            return !z;  // NE
        case 0x7:
            return z;  // EQ
        case 0x8:
            return !v;  // VC
        case 0x9:
            return v;  // VS
        case 0xA:
            return !n;  // PL
        case 0xB:
            return n;  // MI
        case 0xC:
            return n == v;  // GE
        case 0xD:
            return n != v;  // LT
        case 0xE:
            return !z && (n == v);  // GT
        default:
            return z || (n != v);  // LE
    }
}

// --- 割り込み --------------------------------------------------------------

void M68k::requestInterrupt(u32 level, u32 vectorNumber)
{
    if (level > pendingIrq_)
    {
        pendingIrq_ = level;
        pendingVector_ = vectorNumber;
    }
}

// --- 実行 ------------------------------------------------------------------

u32 M68k::unimplemented(u16 op)
{
    // IPL-ROM を走らせて「落ちた命令から実装する」開発ループのための停止。
    // 実機には無い状態なので、ここに来たら必ずログを見て実装を足すこと。
    st_.halted = true;
    st_.ir = op;
    return 0;
}

u32 M68k::step()
{
    if (st_.halted)
    {
        return 0;
    }

    // 割り込みは命令境界でのみ受け付ける。レベル 7 はマスク不可。
    const bool irqPending = pendingIrq_ != 0;
    const bool irqAllowed = pendingIrq_ == 7 || pendingIrq_ > st_.interruptMask();
    if (irqPending && irqAllowed)
    {
        const u32 level = pendingIrq_;
        const u32 vectorNumber =
            pendingVector_ != 0 ? pendingVector_ : (vector::kAutoVectorBase + level);
        pendingIrq_ = 0;
        pendingVector_ = 0;
        st_.stopped = false;
        // ベクタ番号を自分で返すデバイス (X68000 の MFP など) は
        // requestInterrupt でその番号を渡してくる。渡されなければ自動ベクタ。
        takeException(vectorNumber);
        setSr(static_cast<u16>((st_.sr & clearMask(sr_bit::kIntMask)) | (level << 8)));
        return 44;
    }

    if (st_.stopped)
    {
        // STOP 中は割り込みが来るまで時間だけが進む。
        return 4;
    }

    const u16 op = fetch();
    const u32 cycles = execute(op);
    st_.cycles += cycles;
    return cycles;
}

// トレース例外について:
//   SR の bit15 が立っていると、実機は 1 命令ごとにトレース例外を起こす。
//   本エミュレータは未実装。
//
//   Why not 実装するか: テストベクタは「1 命令実行した直後の状態」を期待値と
//   しており、そこにトレース例外のフレームは含まれていない (例外処理は次の
//   ステップ扱い)。ここで例外に入ると全命令のスイートが SSP のずれで落ちる。
//   実機の正確なタイミングを再現するには命令実行と例外処理を分離する必要が
//   あるが、Human68k の起動と通常のプログラム実行でトレースは使われないため
//   (デバッガ専用の機能)、PoC の段階では投資対効果が見合わない。

u32 M68k::execute(u16 op)
{
    switch (op >> 12)
    {
        case 0x0:
            return groupImmediate(op);
        case 0x1:
            return groupMove(op, kByte);
        case 0x2:
            return groupMove(op, kLong);
        case 0x3:
            return groupMove(op, kWord);
        case 0x4:
            return groupMisc(op);
        case 0x5:
            return groupQuickAlu(op);
        case 0x6:
            return groupBranch(op);
        case 0x7:
            return groupMoveq(op);
        case 0x8:
            return groupOrDiv(op);
        case 0x9:
            return groupSub(op);
        case 0xB:
            return groupCmpEor(op);
        case 0xC:
            return groupAndMul(op);
        case 0xD:
            return groupAdd(op);
        case 0xE:
            return groupShift(op);
        case 0xA:
            // A-line: 未実装命令として OS に処理を委ねる仕組み。
            takeException(vector::kLineA);
            return 34;
        default:
            // F-line: コプロセッサ用。X68000 では FPU が無ければ例外。
            takeException(vector::kLineF);
            return 34;
    }
}

}  // namespace x68k
