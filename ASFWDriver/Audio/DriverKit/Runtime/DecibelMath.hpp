// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DecibelMath.hpp - dB -> amplitude for a level realised in the host.
//
// DriverKit ships no libm, so 2^y is built from the float exponent field plus a short
// series. Worst error against std::pow is about 0.00001 dB over the -80...0 dB range of a
// volume control.
//
// The arithmetic comes from the MOTU register-DSP volume encoding of mrmidi/ASFireWire#119.

#pragma once

#include <cstdint>

namespace ASFW::Audio {

namespace detail {

/// 2^y. The integer part comes from the exponent field and the fraction from a Taylor
/// series for e^u over u in [0, ln 2).
[[nodiscard]] constexpr float Exp2(float y) noexcept {
    if (!(y > -126.0f)) { // also catches NaN
        return 0.0f;
    }
    if (y >= 127.0f) {
        return 3.4028235e38f;
    }
    auto n = static_cast<int32_t>(y);
    float fraction = y - static_cast<float>(n);
    if (fraction < 0.0f) { // truncation is not floor for negatives
        fraction += 1.0f;
        --n;
    }
    const float u = fraction * 0.6931472f; // ln 2
    float term = 1.0f;
    float sum = 1.0f;
    for (int i = 1; i <= 8; ++i) {
        term *= u / static_cast<float>(i);
        sum += term;
    }
    const auto exponentBits = static_cast<uint32_t>(127 + n) << 23U;
    return sum * __builtin_bit_cast(float, exponentBits);
}

inline constexpr float kDbPerOctave = 6.0205999f; // 20 * log10(2)

} // namespace detail

/// 10^(dB / 20).
[[nodiscard]] constexpr float DecibelsToAmplitude(float decibels) noexcept {
    return detail::Exp2(decibels / detail::kDbPerOctave);
}

} // namespace ASFW::Audio
