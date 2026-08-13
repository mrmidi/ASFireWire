// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioInternalTxTiming.hpp"
#include "ASFWDriver/Common/TimingUtils.hpp"

#include <variant>

namespace {

using namespace ASFW::Audio::Families::BeBoB::MAudio;

constexpr StartEpoch kEpoch{91};

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
    constexpr uint16_t expectedSyt[] = {0x1000, 0x2000, 0x3000, 0xFFFF, 0x4000};
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
    EXPECT_EQ(timing.RunningCycleTimer(), CycleTimer(0, 4));
}

TEST(MAudioInternalTxTimingTests, SafeNoDataFallbackConsumesCadenceButNotSyt) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    for (uint32_t index = 0; index < 3; ++index) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        ASSERT_TRUE(plan.isData);
        EXPECT_EQ(plan.syt, 0x1000U);
        EXPECT_TRUE(timing.CommitPacket(plan, false));
        EXPECT_EQ(timing.RunningCycleTimer(), 0U);
    }

    InternalTxPacketPlan noData{};
    ASSERT_TRUE(timing.PreviewNextPacket(noData));
    EXPECT_FALSE(noData.isData);
    EXPECT_EQ(noData.syt, 0xFFFFU);
    EXPECT_TRUE(timing.CommitPacket(noData, false));

    InternalTxPacketPlan retry{};
    ASSERT_TRUE(timing.PreviewNextPacket(retry));
    EXPECT_TRUE(retry.isData);
    EXPECT_EQ(retry.syt, 0x1000U);
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
    // With the zero seed, target = completion + 683.  These completion cycles
    // make target land at 7997, 7996, 7995, and 7998 respectively.
    constexpr Case cases[] = {
        {7'314, 3, false},
        {7'313, 4, false},
        {7'312, 5, false},
        {7'315, 2, true},
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
              (7'999U + kInternalTxLeadCycles +
               kInternalTxReanchorExtraCycles) %
                  ASFW::Timing::kCyclesPerSecond);

    InternalTxPacketPlan plan{};
    ASSERT_TRUE(timing.PreviewNextPacket(plan));
    EXPECT_TRUE(plan.isData);
    // Re-anchor established completion+683+4 as the running base; the vendor
    // advances once more before writing a DATA packet header.
    EXPECT_EQ(plan.syt,
              static_cast<uint16_t>(CycleTimer(0, fields.cycle + 1) & 0xFFFFU));
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
