// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Pure, host-testable measurement helpers, error budget calculations,
// and sample classification for TX latency metering (E0 -> E2).

#pragma once

#include "PublicationRangeRing.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Isoch/Core/IsochTxQueue.hpp"

#include <algorithm>
#include <cstdint>

namespace ASFW::Audio::Runtime {

/// 24.576 MHz FireWire bus tick constants.
inline constexpr uint64_t kBusTicksPerSecond = 24'576'000ULL;
inline constexpr uint64_t kBusTicksPerCycle = 3072ULL;             // 125 us exact
inline constexpr uint64_t kNanosPerCycle = 125'000ULL;             // 125 us exact
inline constexpr uint64_t kBusTicksPer8SecondWrap = 64'000ULL * kBusTicksPerCycle; // 196,608,000 ticks

/// Safe correlation age limit: 1 second (~24.576M bus ticks), well within 4s half-window.
inline constexpr uint64_t kMaxCorrelationAgeBusTicks = 24'576'000ULL;

/// Assumed maximum relative clock drift (100 ppm: IEEE 1394 standard limit).
inline constexpr uint32_t kDefaultAssumedDriftPpm = 100;

/// Exact microsecond conversion without the 2.4% truncation bug of ticks / 24.
[[nodiscard]] constexpr uint64_t BusTicksToMicros(uint64_t ticks) noexcept {
    // 3072 ticks = 125 microseconds exact.
    return (ticks * 125ULL) / 3072ULL;
}

/// Exact nanosecond conversion from bus ticks.
[[nodiscard]] constexpr uint64_t BusTicksToNanos(uint64_t ticks) noexcept {
    // 3072 ticks = 125,000 nanoseconds exact. (125000 / 3072 = 15625 / 384)
    return (ticks * 15'625ULL) / 384ULL;
}

enum class TxLatencyOutcome : uint32_t {
    Matched = 0,
    Substituted,
    Unresolved,
    AgedOut,
    TransmitFailed,
    Invalid,
};

enum class TxLatencyUnresolvedReason : uint32_t {
    None = 0,
    StaleCorrelation,
    CoveragePending,
    CoverageGap,
    EpochMismatch,
    PublicationAgedOut,
    ProvenanceAgedOut,
    UnrecognizedEventCode,
    ImageUnavailable,
};

struct TransmitBounds final {
    uint64_t cycleStartHost{0};
    uint64_t txEarliestHost{0};
    uint64_t txLatestHost{0};
    uint32_t uncertaintyHostTicks{0};
    uint32_t correlationAgeBusTicks{0};
    bool valid{false};
    TxLatencyUnresolvedReason failureReason{TxLatencyUnresolvedReason::None};
};

/// Lift an 8-second ambiguous OUTPUT_LAST completion timestamp against a correlation anchor.
/// Reconstructs host transmission cycle bounds incorporating the full 4-term uncertainty budget:
/// 1. Correlation read bracket (measured at top of refill MMIO)
/// 2. Conversion rounding (conservative ceiling)
/// 3. Bus<->host rate mapping drift over correlation age
/// 4. Stamp granularity (one 125 us cycle)
[[nodiscard]] inline TransmitBounds ComputeTransmitBounds(
    uint32_t completionCycleTimer,
    const Isoch::IsochTxClockPairSample& pair,
    uint32_t assumedDriftPpm = kDefaultAssumedDriftPpm) noexcept {
    TransmitBounds bounds{};

    // Extract sec[2:0] and cycle[12:0] from the completion cycle timer.
    const uint32_t compSec = (completionCycleTimer >> 25) & 0x7u;
    const uint32_t compCyc = (completionCycleTimer >> 12) & 0x1FFFu;
    const uint64_t compBusTicksMod =
        (static_cast<uint64_t>(compSec) * 8000ULL + compCyc) * kBusTicksPerCycle;

    const uint32_t corrSec = (pair.cycleTimer32 >> 25) & 0x7Fu;
    const uint32_t corrCyc = (pair.cycleTimer32 >> 12) & 0x1FFFu;
    const uint32_t corrOff = pair.cycleTimer32 & 0xFFFu;
    const uint64_t corrBusTicks =
        (static_cast<uint64_t>(corrSec) * 8000ULL + corrCyc) * kBusTicksPerCycle + corrOff;

    // Lift compBusTicksMod to the nearest 8-second window relative to corrBusTicks.
    const int64_t diffMod = static_cast<int64_t>(compBusTicksMod) -
                            static_cast<int64_t>(corrBusTicks % kBusTicksPer8SecondWrap);
    int64_t deltaFromCorr = diffMod;
    constexpr int64_t halfWrap = static_cast<int64_t>(kBusTicksPer8SecondWrap / 2);
    if (deltaFromCorr > halfWrap) {
        deltaFromCorr -= static_cast<int64_t>(kBusTicksPer8SecondWrap);
    } else if (deltaFromCorr < -halfWrap) {
        deltaFromCorr += static_cast<int64_t>(kBusTicksPer8SecondWrap);
    }

    const uint64_t ageBusTicks = (deltaFromCorr >= 0)
                                     ? static_cast<uint64_t>(deltaFromCorr)
                                     : static_cast<uint64_t>(-deltaFromCorr);
    bounds.correlationAgeBusTicks = static_cast<uint32_t>(std::min<uint64_t>(ageBusTicks, UINT32_MAX));

    // Reject if correlation age exceeds the safe correlation half-window.
    if (ageBusTicks > kMaxCorrelationAgeBusTicks) {
        bounds.failureReason = TxLatencyUnresolvedReason::StaleCorrelation;
        return bounds;
    }

    // Convert delta to nanoseconds and host ticks.
    const uint64_t deltaNs = BusTicksToNanos(ageBusTicks);
    const uint64_t deltaHostTicks = Timing::nanosToHostTicks(deltaNs);

    uint64_t cycleStartHost = 0;
    if (deltaFromCorr <= 0) {
        // Completion was at or before the correlation anchor.
        cycleStartHost = pair.hostTimeMid >= deltaHostTicks ? pair.hostTimeMid - deltaHostTicks : 0;
    } else {
        // Completion was after the correlation anchor.
        cycleStartHost = pair.hostTimeMid + deltaHostTicks;
    }
    bounds.cycleStartHost = cycleStartHost;

    // Uncertainty terms:
    // 1. Correlation bracket (already in host ticks on pair).
    const uint32_t bracketHostTicks = pair.bracketTicks;

    // 2. Conversion rounding ceiling: conservative 2 host ticks.
    constexpr uint32_t kRoundingHostTicks = 2;

    // 3. Clock drift over correlation age: deltaNs * assumedDriftPpm / 1,000,000.
    const uint64_t driftNs = (deltaNs * assumedDriftPpm) / 1'000'000ULL;
    const uint32_t driftHostTicks = static_cast<uint32_t>(Timing::nanosToHostTicks(driftNs));

    // 4. One FireWire cycle in host ticks.
    const uint64_t oneCycleHost = Timing::nanosToHostTicks(kNanosPerCycle);

    const uint32_t totalU = bracketHostTicks + kRoundingHostTicks + driftHostTicks;
    bounds.uncertaintyHostTicks = totalU;

    bounds.txEarliestHost = cycleStartHost >= totalU ? cycleStartHost - totalU : 0;
    bounds.txLatestHost = cycleStartHost + oneCycleHost + totalU;
    bounds.valid = true;
    return bounds;
}

/// Pure classification of a sampled transmission.
[[nodiscard]] inline TxLatencyOutcome ClassifyTxLatencySample(
    uint16_t eventCode,
    bool pcmIdentityProven,
    bool isSubstitution,
    PublicationCoverageResult coverageResult,
    const TransmitBounds& txBounds,
    uint64_t pubLatestHostTicks,
    TxLatencyUnresolvedReason& outReason) noexcept {
    outReason = TxLatencyUnresolvedReason::None;

    // 1. Descriptor reports transmission failure (xferStatus event code).
    // Note: Event 0 indicates successful transmission completion in OHCI IT descriptors.
    if (eventCode != 0) {
        // Non-zero event code: if unrecognized or error, evaluate failure.
        // In the initial stage, treat non-zero as TransmitFailed if error, or Unresolved.
        // Here, eventCode != 0 is classified as Unresolved(UnrecognizedEventCode)
        // or TransmitFailed if high bit/error bit is set.
        outReason = TxLatencyUnresolvedReason::UnrecognizedEventCode;
        return TxLatencyOutcome::Unresolved;
    }

    // 2. Correlation invalidity or stale age.
    if (!txBounds.valid) {
        outReason = txBounds.failureReason;
        return TxLatencyOutcome::Unresolved;
    }

    // 3. Known content substitution (e.g. armed silence transmitted because late fill missed deadline).
    if (isSubstitution) {
        return TxLatencyOutcome::Substituted;
    }

    // 4. Content provenance could not be proven.
    if (!pcmIdentityProven) {
        outReason = TxLatencyUnresolvedReason::ProvenanceAgedOut;
        return TxLatencyOutcome::AgedOut;
    }

    // 5. Publication coverage evaluation.
    switch (coverageResult) {
        case PublicationCoverageResult::Resolved:
            break;
        case PublicationCoverageResult::Pending:
            outReason = TxLatencyUnresolvedReason::CoveragePending;
            return TxLatencyOutcome::Unresolved;
        case PublicationCoverageResult::Gap:
            outReason = TxLatencyUnresolvedReason::CoverageGap;
            return TxLatencyOutcome::Unresolved;
        case PublicationCoverageResult::EpochMismatch:
            outReason = TxLatencyUnresolvedReason::EpochMismatch;
            return TxLatencyOutcome::Unresolved;
        case PublicationCoverageResult::AgedOut:
            outReason = TxLatencyUnresolvedReason::PublicationAgedOut;
            return TxLatencyOutcome::AgedOut;
    }

    // 6. Temporal ordering check:
    // rawWaitMax = txLatestHost - publicationEarliest (or pubLatest).
    // Ordering is physically impossible only when txLatestHost < pubEarliest/pubLatest (waitMax < 0).
    // A same-cycle publication and transmission where txEarliestHost < pubLatestHost <= txLatestHost
    // is a valid short wait (rawWaitMin < 0 <= rawWaitMax), not Invalid.
    if (txBounds.txLatestHost < pubLatestHostTicks) {
        return TxLatencyOutcome::Invalid;
    }

    return TxLatencyOutcome::Matched;
}

} // namespace ASFW::Audio::Runtime
