// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOLib.h>
#include "../AM824/AM824Encoder.hpp"
#include <cstdint>

namespace ASFW::Encoding::RawPcm24In32 {

[[nodiscard]] constexpr uint32_t Encode(int32_t pcmSample) noexcept {
    const int32_t normalized = NormalizeSigned24In32LowAligned(pcmSample);
    return OSSwapHostToBigInt32(static_cast<uint32_t>(normalized));
}

[[nodiscard]] constexpr uint32_t EncodeSilence() noexcept {
    return 0u;
}

} // namespace ASFW::Encoding::RawPcm24In32
