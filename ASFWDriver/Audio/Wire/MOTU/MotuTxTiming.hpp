// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuTxTiming.hpp - SPH helpers shared by the MOTU transmit stamper and the
// receive diagnostics.
//
// The transmit SPH itself comes from the SPH regulator (MotuTxTimingStamper,
// MotuDeviceTiming.hpp); this header keeps the per-packet arithmetic around it.

#pragma once

#include "MotuBlockCodec.hpp"
#include "MotuSph.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Encoding::Motu {

/// Base tick for a packet, from its whole-cycle count (amdtp-motu.c:379).
[[nodiscard]] constexpr uint32_t BaseTickForCycle(uint32_t cycleCount) noexcept {
    return (cycleCount % kCyclesPerSecond) * kTicksPerCycle;
}

/// Zero the SPH quadlet of every data block in a prepared packet.
/// Used when presentation timing is unavailable (e.g. before the transmit
/// clock is seeded) so that no data block goes out carrying stale DMA memory.
inline uint32_t WritePacketSphZero(std::span<uint8_t> payload,
                                   uint32_t dbs,
                                   uint32_t dataBlocks,
                                   uint32_t cipHeaderBytes = 8U) noexcept {
    if (dbs == 0 || dataBlocks == 0) {
        return 0;
    }
    const uint64_t blockBytes = static_cast<uint64_t>(dbs) * 4ULL;
    uint32_t blocksZeroed = 0;
    for (uint32_t block = 0; block < dataBlocks; ++block) {
        const uint64_t blockStart =
            static_cast<uint64_t>(cipHeaderBytes) + static_cast<uint64_t>(block) * blockBytes;
        if (blockStart + 4ULL > payload.size()) {
            break;
        }
        WriteSph(payload.subspan(static_cast<size_t>(blockStart), 4), 0U);
        ++blocksZeroed;
    }
    return blocksZeroed;
}

} // namespace ASFW::Encoding::Motu
