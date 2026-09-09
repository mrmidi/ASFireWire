// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Pure, host-testable measurement helpers, error budget calculations,
// and sample classification for TX latency metering (E0 -> E2).

#pragma once

#include "PublicationRangeRing.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Hardware/OHCIEventCodes.hpp"
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

[[nodiscard]] constexpr bool IsOHCITransmitSuccess(uint16_t eventCode) noexcept {
    return eventCode == static_cast<uint16_t>(ASFW::Async::OHCIEventCode::kAckComplete);
}

[[nodiscard]] constexpr bool IsOHCITransmitFailure(uint16_t eventCode) noexcept {
    switch (static_cast<ASFW::Async::OHCIEventCode>(eventCode & 0x1Fu)) {
        case ASFW::Async::OHCIEventCode::kEvtLongPacket:
        case ASFW::Async::OHCIEventCode::kEvtMissingAck:
        case ASFW::Async::OHCIEventCode::kEvtUnderrun:
        case ASFW::Async::OHCIEventCode::kEvtOverrun:
        case ASFW::Async::OHCIEventCode::kEvtDescriptorRead:
        case ASFW::Async::OHCIEventCode::kEvtDataRead:
        case ASFW::Async::OHCIEventCode::kEvtDataWrite:
        case ASFW::Async::OHCIEventCode::kEvtBusReset:
        case ASFW::Async::OHCIEventCode::kEvtTimeout:
        case ASFW::Async::OHCIEventCode::kEvtTcodeErr:
        case ASFW::Async::OHCIEventCode::kEvtUnknown:
        case ASFW::Async::OHCIEventCode::kEvtFlushed:
        case ASFW::Async::OHCIEventCode::kAckBusyX:
        case ASFW::Async::OHCIEventCode::kAckBusyA:
        case ASFW::Async::OHCIEventCode::kAckBusyB:
        case ASFW::Async::OHCIEventCode::kAckTardy:
        case ASFW::Async::OHCIEventCode::kAckDataError:
        case ASFW::Async::OHCIEventCode::kAckTypeError:
            return true;
        default:
            return false;
    }
}

/// Pure classification of a sampled transmission.
[[nodiscard]] inline TxLatencyOutcome ClassifyTxLatencySample(
    uint16_t eventCode,
    bool pcmIdentityProven,
    bool isSubstitution,
    PublicationCoverageResult coverageResult,
    const TransmitBounds& txBounds,
    uint64_t pubEarliestHostTicks,
    uint64_t pubLatestHostTicks,
    TxLatencyUnresolvedReason& outReason) noexcept {
    outReason = TxLatencyUnresolvedReason::None;

    // 1. Descriptor reports transmission status (xferStatus event code).
    // OHCI Table 3-2: ack_complete (0x11) indicates successful transmission.
    if (!IsOHCITransmitSuccess(eventCode)) {
        if (IsOHCITransmitFailure(eventCode)) {
            return TxLatencyOutcome::TransmitFailed;
        }
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
    // With publication bracket [pubEarliest, pubLatest] and transmission bounds [txEarliest, txLatest]:
    // waitMin = txEarliest - pubLatest
    // waitMax = txLatest   - pubEarliest
    // Ordering is physically impossible only when txLatestHost < pubEarliestHostTicks (waitMax < 0).
    // An overlapping bracket where txEarliestHost < pubLatestHost <= txLatestHost
    // is a valid short wait (waitMin < 0 <= waitMax), not Invalid.
    if (txBounds.txLatestHost < pubEarliestHostTicks) {
        return TxLatencyOutcome::Invalid;
    }

    return TxLatencyOutcome::Matched;
}

} // namespace ASFW::Audio::Runtime
