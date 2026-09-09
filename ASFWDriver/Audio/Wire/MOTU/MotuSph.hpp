// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuSph.hpp - Source-packet-header timestamp math for MOTU streams.
//
// Every MOTU data block begins with a 32-bit SPH quadlet expressing a
// presentation time on the 24.576 MHz cycle timeline: a 13-bit cycle count
// (0..7999, wrapping each second) and a 12-bit intra-cycle tick offset
// (0..3071). The host must capture device SPH timing and later replay it
// (rebased) on the transmit stream; the pure arithmetic for both directions
// lives here. Cross-validated with Linux amdtp-motu.c:19-25,303-393.

#pragma once

#include <cstdint>

namespace ASFW::Encoding::Motu {

inline constexpr uint32_t kTicksPerCycle = 3072;
inline constexpr uint32_t kCyclesPerSecond = 8000;
inline constexpr uint32_t kTicksPerSecond = kTicksPerCycle * kCyclesPerSecond;

inline constexpr uint32_t kSphCycleShift = 12;
inline constexpr uint32_t kSphCycleMask = 0x01fff000;
inline constexpr uint32_t kSphOffsetMask = 0x00000fff;

/// Compose an SPH quadlet (host order) from an absolute tick on the
/// one-second timeline (amdtp-motu.c:384).
[[nodiscard]] constexpr uint32_t SphFromTick(uint32_t tick) noexcept {
    const uint32_t wrapped = tick % kTicksPerSecond;
    return ((wrapped / kTicksPerCycle) << kSphCycleShift) |
           (wrapped % kTicksPerCycle);
}

/// Absolute tick on the one-second timeline from an SPH quadlet
/// (amdtp-motu.c:316-317).
[[nodiscard]] constexpr uint32_t TickFromSph(uint32_t sph) noexcept {
    return ((sph & kSphCycleMask) >> kSphCycleShift) * kTicksPerCycle +
           (sph & kSphOffsetMask);
}

/// Offset of an SPH presentation time relative to a base tick, accounting for
/// wrap at the one-second boundary (amdtp-motu.c:312-321). Used when caching
/// receive-side timing for later transmit replay.
[[nodiscard]] constexpr uint32_t TickOffsetFromBase(uint32_t sph,
                                                    uint32_t baseTick) noexcept {
    uint32_t tick = TickFromSph(sph);
    if (tick < baseTick) {
        tick += kTicksPerSecond;
    }
    return tick - baseTick;
}

/// Rebase a cached tick offset onto a new base for transmit replay
/// (amdtp-motu.c:382-384).
[[nodiscard]] constexpr uint32_t ReplaySph(uint32_t cachedOffset,
                                           uint32_t baseTick) noexcept {
    return SphFromTick((baseTick + cachedOffset) % kTicksPerSecond);
}

} // namespace ASFW::Encoding::Motu
