// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kei Nakayama

#ifndef X68K_CORE_DEV_OPM_ENVELOPE_H
#define X68K_CORE_DEV_OPM_ENVELOPE_H

#include <array>
#include <cstdint>

namespace x68k::detail
{

inline constexpr unsigned kEnvelopeFractionBits = 4;

inline constexpr std::array<std::uint32_t, 4> envelopeStepBases(std::uint32_t scaleQ16)
{
    std::array<std::uint32_t, 4> bases{};
    for (unsigned i = 0; i < bases.size(); ++i)
    {
        bases[i] = static_cast<std::uint32_t>(
            ((std::uint64_t{4u + i} << kEnvelopeFractionBits) * scaleQ16) >> 16);
    }
    return bases;
}

}  // namespace x68k::detail

#endif  // X68K_CORE_DEV_OPM_ENVELOPE_H
