// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioInternalTxTiming.hpp"
#include "ASFWDriver/Common/TimingUtils.hpp"

#include <variant>

namespace {

using namespace ASFW::Audio::Families::BeBoB::MAudio;

constexpr StartEpoch kEpoch{91};

/// Cycles between an OUTPUT_LAST completion and the re-anchor target.
constexpr uint32_t kEffectiveLeadCycles = kInternalTxLeadCycles;

[[nodiscard]] constexpr uint32_t CycleTimer(uint32_t seconds,
                                             uint32_t cycle,
                                             uint32_t offset = 0) {
    return ASFW::Timing::encodeCycleTimer(seconds, cycle, offset);
}

TEST(MAudioInternalTxTimingTests, RefusesAnyGeometryOtherThanInternal48kEightFrames) {
    InternalTxTiming timing;

    EXPECT_FALSE(timing.Arm(kEpoch, 44'100, kInternalTxSytInterval));
    EXPECT_TRUE(std::holds_alternative<InternalTxTimingFailed>(timing.State()));
    EXPECT_FALSE(timing.IsArmed());

    EXPECT_FALSE(timing.Arm(kEpoch, kInternalTxTimingRateHz, 16));
    EXPECT_TRUE(std::holds_alternative<InternalTxTimingFailed>(timing.State()));

    EXPECT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));
    EXPECT_TRUE(std::holds_alternative<InternalTxTimingRunning>(timing.State()));
}

TEST(MAudioInternalTxTimingTests, StartsAtZeroAndRepeatsTheVendorThreeDataOneNoDataCadence) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    constexpr bool expectedData[] = {true, true, true, false, true};
    // 4096 ticks per DATA packet, carried at the 3072-tick cycle modulus, so
    // the offset runs 1024, 2048, 0 and the cycle field reaches 1, 2, 4.
    // Linux derives the same 4096 as TICKS_PER_SECOND * syt_interval / rate
    // (amdtp-stream.c:306) and emits the identical 0/1024/2048/NO_INFO phase
    // sequence from calculate_syt_offset() (amdtp-stream.c:425-461).
    constexpr uint16_t expectedSyt[] = {0x1400, 0x2800, 0x4000, 0xFFFF, 0x5400};
    for (uint64_t index = 0; index < std::size(expectedData); ++index) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        EXPECT_EQ(plan.sequence, index);
        EXPECT_EQ(plan.isData, expectedData[index]);
        EXPECT_EQ(plan.dataBlocks,
                  expectedData[index] ? kInternalTxSytInterval : 0U);
        EXPECT_EQ(plan.syt, expectedSyt[index]);
        EXPECT_TRUE(timing.CommitPacket(plan, plan.isData));
    }
    // Four committed DATA packets: 4 * 4096 = 16384 ticks = 5 cycles + 1024.
    EXPECT_EQ(timing.RunningCycleTimer(), CycleTimer(0, 5, 1'024));
}

TEST(MAudioInternalTxTimingTests, SafeNoDataFallbackConsumesCadenceAndStillAdvancesTheClock) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    // Degrading a planned DATA packet withholds fabricated PCM but must not
    // stall the timestamp base, or the group advances 2 * 4096 ticks across
    // four cycles of real time and every later SYT inherits the deficit.
    constexpr uint16_t expectedSyt[] = {0x1400, 0x2800, 0x4000};
    const uint32_t expectedRunning[] = {
        CycleTimer(0, 1, 1'024), CycleTimer(0, 2, 2'048), CycleTimer(0, 4, 0)};
    for (uint32_t index = 0; index < 3; ++index) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        ASSERT_TRUE(plan.isData);
        EXPECT_EQ(plan.syt, expectedSyt[index]);
        EXPECT_TRUE(timing.CommitPacket(plan, false));
        EXPECT_EQ(timing.RunningCycleTimer(), expectedRunning[index]);
    }

    // The cadence NO-DATA packet is the one that genuinely holds the clock:
    // it occupies a cycle the vendor accumulator never advances across.
    InternalTxPacketPlan noData{};
    ASSERT_TRUE(timing.PreviewNextPacket(noData));
    EXPECT_FALSE(noData.isData);
    EXPECT_EQ(noData.syt, 0xFFFFU);
    EXPECT_TRUE(timing.CommitPacket(noData, false));
    EXPECT_EQ(timing.RunningCycleTimer(), CycleTimer(0, 4, 0));

    InternalTxPacketPlan retry{};
    ASSERT_TRUE(timing.PreviewNextPacket(retry));
    EXPECT_TRUE(retry.isData);
    EXPECT_EQ(retry.syt, 0x5400U);
}

TEST(MAudioInternalTxTimingTests, RejectsStalePreviewAndDuplicateCompletionStamp) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    InternalTxPacketPlan plan{};
    ASSERT_TRUE(timing.PreviewNextPacket(plan));
    EXPECT_TRUE(timing.CommitPacket(plan, true));
    EXPECT_FALSE(timing.CommitPacket(plan, true));

    const auto first = timing.ObserveTxCompletion(1, CycleTimer(0, 0));
    EXPECT_TRUE(first.accepted);
    const auto duplicate = timing.ObserveTxCompletion(1, CycleTimer(0, 1));
    EXPECT_FALSE(duplicate.accepted);
    const auto invalid = timing.ObserveTxCompletion(2, CycleTimer(0, 8'000));
    EXPECT_FALSE(invalid.accepted);
}

TEST(MAudioInternalTxTimingTests, PreservesTheVendorThreeToFiveCyclePhaseWindowExactly) {
    struct Case final {
        uint32_t completionCycle;
        int32_t expectedPhase;
        bool reanchored;
    };
    // Phase is measured from the zero-seeded running timer against
    // target = completion + effective lead, so choosing a phase pins the
    // completion cycle exactly.  Deriving it keeps this a test of the [3, 6)
    // window rather than of the lead constant's current value; the four cases
    // make the target land at 7997, 7996, 7995, and 7998 either way.
    const auto completionFor = [](int32_t phase) {
        return ASFW::Timing::kCyclesPerSecond - static_cast<uint32_t>(phase) -
               kEffectiveLeadCycles;
    };
    const Case cases[] = {
        {completionFor(3), 3, false},
        {completionFor(4), 4, false},
        {completionFor(5), 5, false},
        {completionFor(2), 2, true},
    };

    for (const auto& item : cases) {
        InternalTxTiming timing;
        ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                               kInternalTxSytInterval));
        const auto observed = timing.ObserveTxCompletion(
            1, CycleTimer(0, item.completionCycle));
        EXPECT_TRUE(observed.accepted);
        EXPECT_EQ(observed.phaseCycles, item.expectedPhase);
        EXPECT_EQ(observed.reanchored, item.reanchored);
        if (item.reanchored) {
            const auto fields =
                ASFW::Timing::decodeCycleTimer(observed.runningCycleTimer);
            EXPECT_EQ(fields.seconds, 1U);
            EXPECT_EQ(fields.cycle, 2U);
        } else {
            EXPECT_EQ(observed.runningCycleTimer, 0U);
        }
    }
}

TEST(MAudioInternalTxTimingTests, ReanchorsFromActualCompletionAtLeadPlusFourAcrossTimerWrap) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    const auto observed = timing.ObserveTxCompletion(
        1, CycleTimer(127, 7'999));
    ASSERT_TRUE(observed.accepted);
    ASSERT_TRUE(observed.reanchored);
    const auto fields = ASFW::Timing::decodeCycleTimer(observed.runningCycleTimer);
    EXPECT_EQ(fields.seconds, 0U);
    EXPECT_EQ(fields.cycle,
              (7'999U + kEffectiveLeadCycles +
               kInternalTxReanchorExtraCycles) %
                  ASFW::Timing::kCyclesPerSecond);

    InternalTxPacketPlan plan{};
    ASSERT_TRUE(timing.PreviewNextPacket(plan));
    EXPECT_TRUE(plan.isData);
    // Re-anchor established completion+683+4 as the running base with the old
    // zero offset preserved; the vendor advances one 4096-tick SYT increment
    // more before writing a DATA packet header, so the offset lands at 1024.
    EXPECT_EQ(plan.syt,
              static_cast<uint16_t>(
                  CycleTimer(0, fields.cycle + 1, 1'024) & 0xFFFFU));
}

TEST(MAudioInternalTxTimingTests, DisarmMakesFurtherPacketAndCompletionEventsInert) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));
    timing.Disarm();

    EXPECT_TRUE(std::holds_alternative<InternalTxTimingStopped>(timing.State()));
    EXPECT_FALSE(timing.IsArmed());
    InternalTxPacketPlan plan{};
    EXPECT_FALSE(timing.PreviewNextPacket(plan));
    EXPECT_FALSE(timing.ObserveTxCompletion(1, CycleTimer(0, 0)).accepted);
}

} // namespace
