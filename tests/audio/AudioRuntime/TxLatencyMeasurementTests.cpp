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

    // 1. Success case: Resolved, published before tx, identity proven, no substitution.
    auto outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::Resolved, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Matched);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::None);

    // 2. Hardware event error
    outcome = ClassifyTxLatencySample(
        0x12, true, false, PublicationCoverageResult::Resolved, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::UnrecognizedEventCode);

    // 3. Stale correlation
    TransmitBounds staleBounds{};
    staleBounds.valid = false;
    staleBounds.failureReason = TxLatencyUnresolvedReason::StaleCorrelation;
    outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::Resolved, staleBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::StaleCorrelation);

    // 4. Missing image provenance (aged out)
    outcome = ClassifyTxLatencySample(
        0, false, false, PublicationCoverageResult::Resolved, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::AgedOut);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::ProvenanceAgedOut);

    // 5. Substitution
    outcome = ClassifyTxLatencySample(
        0, true, true, PublicationCoverageResult::Resolved, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Substituted);

    // 6. Coverage Pending
    outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::Pending, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::CoveragePending);

    // 7. Coverage Gap
    outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::Gap, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::CoverageGap);

    // 8. Epoch Mismatch
    outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::EpochMismatch, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Unresolved);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::EpochMismatch);

    // 9. Aged Out
    outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::AgedOut, txBounds, 90'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::AgedOut);
    EXPECT_EQ(reason, TxLatencyUnresolvedReason::PublicationAgedOut);

    // 10. Physically impossible (transmission latest strictly before publication)
    outcome = ClassifyTxLatencySample(
        0, true, false, PublicationCoverageResult::Resolved, txBounds, 120'000, reason);
    EXPECT_EQ(outcome, TxLatencyOutcome::Invalid);
}
