// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioInternalTxTiming.hpp"
#include "ASFWDriver/Common/TimingUtils.hpp"

#include <variant>

namespace {

using namespace ASFW::Audio::Families::BeBoB::MAudio;

constexpr StartEpoch kEpoch{91};

/// A SYT spans 16 cycles; collapse one to ticks inside that window.
[[nodiscard]] constexpr uint32_t SytTicks(uint16_t syt) {
    return ((syt >> 12) & 0x0FU) * ASFW::Timing::kTicksPerCycle +
           (syt & 0x0FFFU);
}

constexpr uint32_t kSytWindowTicks =
    16U * ASFW::Timing::kTicksPerCycle; // 49152

/// Ticks by which a SYT leads the cycle its packet is transmitted in.
[[nodiscard]] constexpr uint32_t LeadTicks(uint16_t syt,
                                           uint32_t transmitCycle) {
    return (SytTicks(syt) -
            (transmitCycle & 0x0FU) * ASFW::Timing::kTicksPerCycle +
            kSytWindowTicks) %
           kSytWindowTicks;
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

TEST(MAudioInternalTxTimingTests, RepeatsTheVendorThreeDataOneNoDataCadenceWithLinuxPhases) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));
    // Arm seeds the phase the cadence NO-DATA holds, so a group's first DATA
    // packet emits 0 and the wrap lands on the NO-DATA.
    EXPECT_EQ(timing.SytPhaseTicks(), kInternalTxSytPhaseSeedTicks);

    constexpr bool expectedData[] = {true, true, true, false, true};
    // Linux emits the identical 0/1024/2048/NO_INFO sequence from
    // calculate_syt_offset (amdtp-stream.c:425-461) with a 1024-tick state.
    constexpr uint32_t expectedPhase[] = {0, 1'024, 2'048, 2'048, 0};
    for (uint64_t index = 0; index < std::size(expectedData); ++index) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        EXPECT_EQ(plan.sequence, index);
        EXPECT_EQ(plan.isData, expectedData[index]);
        EXPECT_EQ(plan.dataBlocks,
                  expectedData[index] ? kInternalTxSytInterval : 0U);
        EXPECT_EQ(plan.sytPhaseTicks, expectedPhase[index]);
        EXPECT_TRUE(timing.CommitPacket(plan, plan.isData));
        EXPECT_EQ(timing.SytPhaseTicks(), expectedPhase[index]);
    }
}

TEST(MAudioInternalTxTimingTests, SytLeadsItsOwnTransmitCycleByTheTransferDelay) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    // The lead must stay inside one SYT increment above the transfer delay --
    // 4.17 to 5.17 cycles -- for any transmit cycle, including across the
    // 16-cycle field wrap.  The superseded accumulator model measured 12.0 to
    // 12.67 cycles on a bus capture, leaving ~3 cycles of headroom.
    for (uint32_t transmitCycle : {0U, 7U, 15U, 16U, 4'383U, 7'999U}) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        ASSERT_TRUE(plan.isData);
        const uint16_t syt =
            ComputeInternalTxSyt(plan.sytPhaseTicks, transmitCycle);
        const uint32_t lead = LeadTicks(syt, transmitCycle);
        EXPECT_EQ(lead, plan.sytPhaseTicks + kInternalTxTransferDelayTicks);
        EXPECT_GE(lead, kInternalTxTransferDelayTicks);
        EXPECT_LT(lead, kInternalTxTransferDelayTicks +
                            kInternalTxSytIncrementTicks);
        ASSERT_TRUE(timing.CommitPacket(plan, true));
        if (!timing.PreviewNextPacket(plan)) {
            FAIL();
        }
        if (!plan.isData) { // consume the cadence NO-DATA
            ASSERT_TRUE(timing.CommitPacket(plan, false));
        }
    }
}

TEST(MAudioInternalTxTimingTests, ConsecutiveDataSytsAdvanceExactlyOneIncrementAcrossTheNoDataGap) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    // One isochronous packet per cycle, so the packet's index is its cycle.
    // The delta between successive DATA timestamps is what encodes the sample
    // rate: it must be one SYT increment whether the packets are adjacent or
    // separated by the cadence NO-DATA.  That is the whole reason the phase
    // wrap has to coincide with the NO-DATA's two-cycle gap.
    constexpr uint32_t kFirstCycle = 4'383;
    bool havePrevious = false;
    uint32_t previousSyt = 0;
    for (uint32_t index = 0; index < 12; ++index) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        const uint32_t transmitCycle = kFirstCycle + index;
        if (plan.isData) {
            const uint32_t sytTicks = SytTicks(
                ComputeInternalTxSyt(plan.sytPhaseTicks, transmitCycle));
            if (havePrevious) {
                EXPECT_EQ((sytTicks - previousSyt + kSytWindowTicks) %
                              kSytWindowTicks,
                          kInternalTxSytIncrementTicks)
                    << "at packet " << index;
            }
            previousSyt = sytTicks;
            havePrevious = true;
        }
        ASSERT_TRUE(timing.CommitPacket(plan, plan.isData));
    }
}

TEST(MAudioInternalTxTimingTests, SafeNoDataFallbackConsumesCadenceAndStillAdvancesThePhase) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    // Degrading a planned DATA packet withholds fabricated PCM but must not
    // stall the phase, or the wrap slips off the NO-DATA slot permanently.
    constexpr uint32_t expectedPhase[] = {0, 1'024, 2'048};
    for (uint32_t index = 0; index < 3; ++index) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        ASSERT_TRUE(plan.isData);
        EXPECT_EQ(plan.sytPhaseTicks, expectedPhase[index]);
        EXPECT_TRUE(timing.CommitPacket(plan, false));
        EXPECT_EQ(timing.SytPhaseTicks(), expectedPhase[index]);
    }

    // The cadence NO-DATA packet is the one that genuinely holds the phase.
    InternalTxPacketPlan noData{};
    ASSERT_TRUE(timing.PreviewNextPacket(noData));
    EXPECT_FALSE(noData.isData);
    EXPECT_EQ(noData.sytPhaseTicks, noData.basePhaseTicks);
    EXPECT_TRUE(timing.CommitPacket(noData, false));
    EXPECT_EQ(timing.SytPhaseTicks(), 2'048U);

    InternalTxPacketPlan resumed{};
    ASSERT_TRUE(timing.PreviewNextPacket(resumed));
    EXPECT_TRUE(resumed.isData);
    EXPECT_EQ(resumed.sytPhaseTicks, 0U);
}

TEST(MAudioInternalTxTimingTests, RejectsAStalePreview) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));

    InternalTxPacketPlan plan{};
    ASSERT_TRUE(timing.PreviewNextPacket(plan));
    EXPECT_TRUE(timing.CommitPacket(plan, true));
    EXPECT_FALSE(timing.CommitPacket(plan, true));
}

TEST(MAudioInternalTxTimingTests, DisarmMakesFurtherPacketEventsInert) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, kInternalTxTimingRateHz,
                           kInternalTxSytInterval));
    timing.Disarm();

    EXPECT_TRUE(std::holds_alternative<InternalTxTimingStopped>(timing.State()));
    EXPECT_FALSE(timing.IsArmed());
    InternalTxPacketPlan plan{};
    EXPECT_FALSE(timing.PreviewNextPacket(plan));
}

} // namespace
