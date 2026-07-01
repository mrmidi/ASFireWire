#pragma once

#include "../../Common/TimingUtils.hpp"

#include <DriverKit/IOLib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ASFW::Isoch::Rx {

struct ExpandedReceiveTimestamp final {
    uint32_t cycleTimer{0};
    int64_t ageTicks{0};
};

[[nodiscard]] inline bool DecodeReceiveTimestamp(const uint8_t* packet,
                                                 size_t length,
                                                 uint16_t& outTimestamp) noexcept {
    if (!packet || length < 4) {
        return false;
    }

    uint32_t trailerLE = 0;
    std::memcpy(&trailerLE, packet + length - 4, sizeof(trailerLE));
    const uint32_t trailer = OSSwapLittleToHostInt32(trailerLE);
    // Trailer: [xferStatus:16] [timestamp:16]
    outTimestamp = static_cast<uint16_t>(trailer & 0xFFFFu);
    return true;
}

[[nodiscard]] inline bool ExpandReceiveTimestamp(
    uint16_t timestamp,
    uint32_t referenceCycleTimer,
    ExpandedReceiveTimestamp& out) noexcept {
    const auto reference =
        ASFW::Timing::decodeCycleTimer(referenceCycleTimer);
    const uint32_t timestampSeconds = (timestamp >> 13) & 0x7u;
    const uint32_t timestampCycle = timestamp & 0x1FFFu;
    if (timestampCycle >= ASFW::Timing::kCyclesPerSecond) {
        return false;
    }

    const int64_t referenceTicks =
        ASFW::Timing::tstampToOffsets(reference);
    const uint32_t candidateSeconds =
        (reference.seconds & ~0x7u) | timestampSeconds;
    const int64_t candidateTicks =
        ASFW::Timing::tstampToOffsets(
            candidateSeconds, timestampCycle, 0);

    constexpr int64_t kEightSecondTicks =
        8LL * static_cast<int64_t>(ASFW::Timing::kTicksPerSecond);
    constexpr int64_t kHalfDomainTicks = kEightSecondTicks / 2;
    int64_t deltaTicks = candidateTicks - referenceTicks;
    if (deltaTicks > kHalfDomainTicks) {
        deltaTicks -= kEightSecondTicks;
    } else if (deltaTicks < -kHalfDomainTicks) {
        deltaTicks += kEightSecondTicks;
    }

    // The reference is sampled before a whole completed-descriptor batch is
    // drained. Hardware can complete later packets while software walks the
    // batch, so a small positive delta is legitimate. Reject only timestamps
    // too far from the reference to belong to the active receive ring.
    constexpr int64_t kMaxCorrelationDistanceTicks =
        static_cast<int64_t>(ASFW::Timing::kTicksPerSecond);
    if (deltaTicks < -kMaxCorrelationDistanceTicks ||
        deltaTicks > kMaxCorrelationDistanceTicks) {
        return false;
    }

    constexpr int64_t kFullDomainTicks =
        static_cast<int64_t>(ASFW::Timing::kFWTimeWrapSeconds) *
        static_cast<int64_t>(ASFW::Timing::kTicksPerSecond);
    int64_t expandedTicks = (referenceTicks + deltaTicks) % kFullDomainTicks;
    if (expandedTicks < 0) {
        expandedTicks += kFullDomainTicks;
    }

    const uint32_t seconds = static_cast<uint32_t>(
        expandedTicks / ASFW::Timing::kTicksPerSecond);
    const int64_t secondTicks =
        expandedTicks % ASFW::Timing::kTicksPerSecond;
    const uint32_t cycle = static_cast<uint32_t>(
        secondTicks / ASFW::Timing::kTicksPerCycle);
    const uint32_t offset = static_cast<uint32_t>(
        secondTicks % ASFW::Timing::kTicksPerCycle);

    out.cycleTimer =
        (seconds << ASFW::Timing::kCycleTimerSecondsShift) |
        (cycle << ASFW::Timing::kCycleTimerCyclesShift) |
        offset;
    // Positive age means the packet preceded the reference; negative age
    // means it completed after the pre-drain reference was sampled.
    out.ageTicks = -deltaTicks;
    return true;
}

[[nodiscard]] inline uint64_t FireWireTicksToNanos(
    uint64_t ticks) noexcept {
    const __uint128_t scaled =
        static_cast<__uint128_t>(ticks) *
        ASFW::Timing::kNanosPerSecond;
    return static_cast<uint64_t>(
        scaled / ASFW::Timing::kTicksPerSecond);
}

} // namespace ASFW::Isoch::Rx
