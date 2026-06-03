// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOLib.h>
#include <cstdint>
#include <optional>

namespace ASFW::Encoding::RawPcm24In32 {

[[nodiscard]] constexpr std::optional<int32_t> Decode(uint32_t quadlet_be) noexcept {
    const uint32_t q = OSSwapBigToHostInt32(quadlet_be);
    
    // Sign-extend 24-bit to 32-bit
    int32_t sample = static_cast<int32_t>(q & 0x00FFFFFFu);
    if ((sample & 0x00800000u) != 0u) {
        sample |= 0xFF000000u;
    }
    return sample;
}

} // namespace ASFW::Encoding::RawPcm24In32
