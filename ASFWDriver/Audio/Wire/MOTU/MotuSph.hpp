// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuSph.hpp - Source-packet-header timestamp math for MOTU streams.
//
// Every MOTU data block begins with a 32-bit SPH quadlet expressing a
// presentation time on the 24.576 MHz cycle timeline: a 13-bit cycle count
// (0..7999, wrapping each second) and a 12-bit intra-cycle tick offset
// (0..3071). The pure arithmetic for both directions lives here.
// Cross-validated with Linux amdtp-motu.c:19-25,303-393.

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
/// wrap at the one-second boundary (amdtp-motu.c:312-321).
[[nodiscard]] constexpr uint32_t TickOffsetFromBase(uint32_t sph,
                                                    uint32_t baseTick) noexcept {
    uint32_t tick = TickFromSph(sph);
    if (tick < baseTick) {
        tick += kTicksPerSecond;
    }
    return tick - baseTick;
}

/// Any signed tick count, wrapped onto the one-second timeline.
[[nodiscard]] constexpr uint32_t WrapTick(int64_t tick) noexcept {
    constexpr int64_t kSecond = kTicksPerSecond;
    int64_t wrapped = tick % kSecond;
    if (wrapped < 0) {
        wrapped += kSecond;
    }
    return static_cast<uint32_t>(wrapped);
}

/// Signed distance from `from` to `to` on the one-second timeline, taken the
/// short way round: (-half a second, +half a second]. A step across a second
/// rollover therefore reads as the few thousand ticks it really is.
[[nodiscard]] constexpr int64_t TickDifference(int64_t to, int64_t from) noexcept {
    constexpr int64_t kSecond = kTicksPerSecond;
    const int64_t difference = static_cast<int64_t>(WrapTick(to - from));
    return difference > kSecond / 2 ? difference - kSecond : difference;
}

/// An SPH quadlet that can be a presentation time: nothing above the cycle
/// count (bits 31:25 clear), a cycle count below 8000 and an offset below
/// 3072. SphFromTick() produces nothing else.
[[nodiscard]] constexpr bool IsWellFormedSph(uint32_t sph) noexcept {
    return (sph & ~(kSphCycleMask | kSphOffsetMask)) == 0 &&
           ((sph & kSphCycleMask) >> kSphCycleShift) < kCyclesPerSecond &&
           (sph & kSphOffsetMask) < kTicksPerCycle;
}

static_assert(IsWellFormedSph(SphFromTick(kTicksPerSecond - 1)));
static_assert(!IsWellFormedSph(0x02000000u));
static_assert(!IsWellFormedSph(8000u << kSphCycleShift));
static_assert(!IsWellFormedSph(kTicksPerCycle));
static_assert(WrapTick(-1) == kTicksPerSecond - 1);
static_assert(TickDifference(10, kTicksPerSecond - 10) == 20);
static_assert(TickDifference(kTicksPerSecond - 10, 10) == -20);

} // namespace ASFW::Encoding::Motu
