// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama
//
// SingleStepTests/m68000 のテストベクタを読むためのデコーダ。
//
// upstream は .json.bin という独自バイナリを配り、decode.py で JSON に変換して
// 使うことを想定している。ここでは変換を挟まずバイナリを直接読む。
// JSON にすると数倍に膨れ、パーサも要るため。形式は upstream の decode.py に
// 合わせてある。
//
// 形式のうち、実装で引っかかりやすい点:
//   - state の pc は MAME の m_au 由来で「実行開始位置 + 4」を指す。
//     これはプリフェッチで 2 ワード先読みした後の PC と一致する。
//   - RAM はワード単位で格納され、1 エントリが上位/下位の 2 バイトに展開される。
//   - prefetch[0] が現在の命令語 (ir)、prefetch[1] が次のワード (irc)。

#ifndef X68K_TEST_M68K_TEST_VECTORS_H
#define X68K_TEST_M68K_TEST_VECTORS_H

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace x68k_test
{

struct VectorState
{
    std::uint32_t d[8]{};
    std::uint32_t a[7]{};  // a0-a6 (a7 は usp/ssp として別に持つ)
    std::uint32_t usp = 0;
    std::uint32_t ssp = 0;
    std::uint32_t sr = 0;
    std::uint32_t pc = 0;
    std::uint16_t prefetch[2]{};
    // アドレスと値の組。バイト単位。
    std::vector<std::pair<std::uint32_t, std::uint8_t>> ram;
};

struct TestCase
{
    std::string name;
    VectorState initial;
    VectorState final;
    std::uint32_t cycleCount = 0;
};

class VectorReader
{
public:
    // ファイル全体を読み、テストケースを取り出す。
    // limit を指定するとその件数で打ち切る (CI ではサブセットだけ回すため)。
    static std::vector<TestCase> load(const std::string& path, std::size_t limit = 0)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            throw std::runtime_error("cannot open " + path);
        }
        std::vector<char> buf((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

        VectorReader r(buf);
        const std::uint32_t magic = r.u32();
        if (magic != 0x1A3F5D71u)
        {
            throw std::runtime_error("bad file magic in " + path);
        }
        const std::uint32_t count = r.u32();

        std::vector<TestCase> tests;
        const std::size_t want = limit == 0 ? count : std::min<std::size_t>(limit, count);
        tests.reserve(want);
        for (std::size_t i = 0; i < want; ++i)
        {
            tests.push_back(r.readTest());
        }
        return tests;
    }

private:
    explicit VectorReader(const std::vector<char>& buf) : buf_(buf) {}

    std::uint8_t u8()
    {
        need(1);
        return static_cast<std::uint8_t>(buf_[ptr_++]);
    }

    std::uint16_t u16()
    {
        need(2);
        std::uint16_t v = 0;
        std::memcpy(&v, buf_.data() + ptr_, 2);
        ptr_ += 2;
        return v;
    }

    std::uint32_t u32()
    {
        need(4);
        std::uint32_t v = 0;
        std::memcpy(&v, buf_.data() + ptr_, 4);
        ptr_ += 4;
        return v;
    }

    void need(std::size_t n) const
    {
        if (ptr_ + n > buf_.size())
        {
            throw std::runtime_error("truncated test vector file");
        }
    }

    std::string readName()
    {
        u32();  // numbytes (未使用)
        const std::uint32_t magic = u32();
        if (magic != 0x89ABCDEFu)
        {
            throw std::runtime_error("bad name magic");
        }
        const std::uint32_t len = u32();
        need(len);
        std::string s(buf_.data() + ptr_, len);
        ptr_ += len;
        return s;
    }

    VectorState readState()
    {
        u32();  // numbytes
        const std::uint32_t magic = u32();
        if (magic != 0x01234567u)
        {
            throw std::runtime_error("bad state magic");
        }

        VectorState st;
        for (auto& d : st.d)
        {
            d = u32();
        }
        for (auto& a : st.a)
        {
            a = u32();
        }
        st.usp = u32();
        st.ssp = u32();
        st.sr = u32();
        st.pc = u32();
        st.prefetch[0] = static_cast<std::uint16_t>(u32());
        st.prefetch[1] = static_cast<std::uint16_t>(u32());

        const std::uint32_t numRams = u32();
        st.ram.reserve(numRams * 2);
        for (std::uint32_t i = 0; i < numRams; ++i)
        {
            const std::uint32_t addr = u32();
            const std::uint16_t data = u16();
            // ワードは上位バイトが先。
            st.ram.emplace_back(addr, static_cast<std::uint8_t>(data >> 8));
            st.ram.emplace_back(addr | 1u, static_cast<std::uint8_t>(data & 0xFFu));
        }
        return st;
    }

    void skipTransactions()
    {
        u32();  // numbytes
        const std::uint32_t magic = u32();
        if (magic != 0x456789ABu)
        {
            throw std::runtime_error("bad transaction magic");
        }
        lastCycleCount_ = u32();
        const std::uint32_t numTransactions = u32();
        for (std::uint32_t i = 0; i < numTransactions; ++i)
        {
            const std::uint8_t kind = u8();
            u32();  // cycles
            if (kind == 0)
            {
                continue;  // idle
            }
            // fc, addr_bus, data_bus, UDS, LDS
            for (int f = 0; f < 5; ++f)
            {
                u32();
            }
        }
    }

    TestCase readTest()
    {
        u32();  // numbytes
        const std::uint32_t magic = u32();
        if (magic != 0xABC12367u)
        {
            throw std::runtime_error("bad test magic");
        }

        TestCase t;
        t.name = readName();
        t.initial = readState();
        t.final = readState();
        // バスサイクルの精度は追わないので、トランザクションは読み飛ばす。
        skipTransactions();
        t.cycleCount = lastCycleCount_;
        return t;
    }

    const std::vector<char>& buf_;
    std::size_t ptr_ = 0;
    std::uint32_t lastCycleCount_ = 0;
};

}  // namespace x68k_test

#endif  // X68K_TEST_M68K_TEST_VECTORS_H
