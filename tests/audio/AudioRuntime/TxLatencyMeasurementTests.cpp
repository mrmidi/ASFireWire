// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Runtime/TxLatencyMeasurement.hpp"
#include <gtest/gtest.h>

using namespace ASFW::Audio::Runtime;

TEST(TxLatencyMeasurementTests, RationalMathExactness) {
    ASFW::Timing::initializeHostTimebase();

    // 3072 bus ticks @ 24.576 MHz is exactly 125 microseconds = 125,000 nanoseconds.
    EXPECT_EQ(BusTicksToNanos(3072), 125'000ULL);
    EXPECT_EQ(BusTicksToMicros(3072), 125ULL);

    // 24576 bus ticks is exactly 1 millisecond = 1,000,000 nanoseconds.
    EXPECT_EQ(BusTicksToNanos(24576), 1'000'000ULL);
    EXPECT_EQ(BusTicksToMicros(24576), 1000ULL);

    // 0 bus ticks = 0 ns / 0 us.
    EXPECT_EQ(BusTicksToNanos(0), 0ULL);
    EXPECT_EQ(BusTicksToMicros(0), 0ULL);
}

TEST(TxLatencyMeasurementTests, ComputeTransmitBoundsValid) {
    ASFW::Timing::initializeHostTimebase();

    ASFW::Isoch::IsochTxClockPairSample pair{};
    // Correlation anchor: cycle 1000, second 2
    // cycleTimer32: second[2:0] in bits [31:25], cycle[12:0] in bits [24:12], offset[11:0] in [11:0]
    const uint32_t corrCycleTimer = (2U << 25) | (1000U << 12) | 0U;
    pair.cycleTimer32 = corrCycleTimer;
    pair.hostTimeMid = 10'000'000ULL; // arbitrary host ticks
    pair.bracketTicks = 12;            // bracket host ticks

    // Completion stamp: 5 cycles after anchor (cycle 1005, second 2)
    const uint32_t compCycleTimer = (2U << 25) | (1005U << 12) | 0U;

    const auto bounds = ComputeTransmitBounds(compCycleTimer, pair, 100);
    EXPECT_TRUE(bounds.valid);
    EXPECT_EQ(bounds.failureReason, TxLatencyUnresolvedReason::None);
    EXPECT_GT(bounds.cycleStartHost, pair.hostTimeMid);
    EXPECT_LE(bounds.txEarliestHost, bounds.cycleStartHost);
    EXPECT_GT(bounds.txLatestHost, bounds.cycleStartHost);
    EXPECT_GT(bounds.uncertaintyHostTicks, 0U);
}

TEST(TxLatencyMeasurementTests, ComputeTransmitBoundsStaleCorrelationRejected) {
    ASFW::Timing::initializeHostTimebase();

    ASFW::Isoch::IsochTxClockPairSample pair{};
    const uint32_t corrCycleTimer = (0U << 25) | (0U << 12) | 0U;
    pair.cycleTimer32 = corrCycleTimer;
    pair.hostTimeMid = 5'000'000ULL;
    pair.bracketTicks = 10;

    // Delta of 2 seconds (16000 cycles) exceeds kMaxCorrelationAgeBusTicks (1 second = 8000 cycles)
    const uint32_t compCycleTimer = (2U << 25) | (0U << 12) | 0U;

    const auto bounds = ComputeTransmitBounds(compCycleTimer, pair, 100);
    EXPECT_FALSE(bounds.valid);
    EXPECT_EQ(bounds.failureReason, TxLatencyUnresolvedReason::StaleCorrelation);
}

TEST(TxLatencyMeasurementTests, ClassifyTxLatencySampleOutcomes) {
    ASFW::Timing::initializeHostTimebase();

    TransmitBounds txBounds{};
    txBounds.valid = true;
    txBounds.txEarliestHost = 100'000;
    txBounds.txLatestHost = 110'000;
    txBounds.cycleStartHost = 105'000;
    txBounds.uncertaintyHostTicks = 500;

    TxLatencyUnresolvedReason reason = TxLatencyUnresolvedReason::None;
    constexpr uint16_t kSuccessAck = 0x11; // ack_complete (17)

    // 1. Success case: ack_complete (0x11), resolved, published before tx, identity proven, no substitution.
    auto outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::Resolved, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Matched);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::None);

    // 2. Event code 0 (evt_no_status) is unresolved (unrecognized event code)
    outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::Resolved, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::UnrecognizedEventCode);

    // 3. Hardware failure event codes -> TransmitFailed
    // Underrun (0x04)
    outcome = ClassifyTxLatencySample(
        0x04, true, false, PublicationCoverageResult::Resolved, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::TransmitFailed);
    // Missing ack (0x03)
    outcome = ClassifyTxLatencySample(
        0x03, true, false, PublicationCoverageResult::Resolved, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::TransmitFailed);
    // Bus reset (0x1D)
    outcome = ClassifyTxLatencySample(
        0x1D, true, false, PublicationCoverageResult::Resolved, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::TransmitFailed);

    // 4. Stale correlation
    TransmitBounds staleBounds{};
    staleBounds.valid = false;
    staleBounds.failureReason = TxLatencyUnresolvedReason::StaleCorrelation;
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::Resolved, staleBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::StaleCorrelation);

    // 5. Missing image provenance (aged out)
    outcome = ClassifyTxLatencySample(
        kSuccessAck, false, false, PublicationCoverageResult::Resolved, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::AgedOut);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::ProvenanceAgedOut);

    // 6. Substitution
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, true, PublicationCoverageResult::Resolved, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Substituted);

    // 7. Coverage Pending
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::Pending, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::CoveragePending);

    // 8. Coverage Gap
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::Gap, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::CoverageGap);

    // 9. Epoch Mismatch
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::EpochMismatch, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::EpochMismatch);

    // 10. Aged Out
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::AgedOut, txBounds, 85'000, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::AgedOut);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::PublicationAgedOut);

    // 11. Overlapping intervals: P=[95'000, 105'000], T=[100'000, 110'000].
    // Since T_latest (110'000) >= P_earliest (95'000), physically possible -> Matched.
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::Resolved, txBounds, 95'000, 105'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Matched);

    // 12. Physically impossible: T_latest (110'000) < P_earliest (115'000) -> Invalid.
    outcome = ClassifyTxLatencySample(
        kSuccessAck, true, false, PublicationCoverageResult::Resolved, txBounds, 115'000, 120'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Invalid);
}
