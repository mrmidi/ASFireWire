// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioInternalTxTiming.hpp"
#include "ASFWDriver/Common/TimingUtils.hpp"

#include <variant>
#include <utility>

namespace {

using namespace ASFW::Audio::Families::BeBoB::MAudio;

constexpr StartEpoch kEpoch{91};
constexpr uint32_t kSytWindowTicks = 16U * ASFW::Timing::kTicksPerCycle;

[[nodiscard]] constexpr uint32_t SytTicks(const uint16_t syt) {
    return ((syt >> 12) & 0x0FU) * ASFW::Timing::kTicksPerCycle +
           (syt & 0x0FFFU);
}

[[nodiscard]] constexpr uint32_t LeadTicks(const uint16_t syt,
                                           const uint32_t transmitCycle) {
    return (SytTicks(syt) -
            (transmitCycle & 0x0FU) * ASFW::Timing::kTicksPerCycle +
            kSytWindowTicks) %
           kSytWindowTicks;
}

TEST(MAudioInternalTxTimingTests, ArmsOnlyWhenGeometryMatchesTheRateFamily) {
    InternalTxTiming timing;

    EXPECT_FALSE(timing.Arm(kEpoch, 44'100, 8));
    EXPECT_TRUE(std::holds_alternative<InternalTxTimingFailed>(timing.State()));
    EXPECT_FALSE(timing.IsArmed());

    EXPECT_TRUE(timing.Arm(kEpoch, 48'000, kInternalTxSytInterval));
    EXPECT_EQ(timing.TransferDelayTicks(), 12'800U);
    EXPECT_TRUE(timing.Arm(kEpoch, 96'000, 16));
    EXPECT_EQ(timing.TransferDelayTicks(), 12'800U);
    EXPECT_TRUE(timing.Arm(kEpoch, 192'000, 32));
    EXPECT_EQ(timing.TransferDelayTicks(), 12'800U);
}

TEST(MAudioInternalTxTimingTests, FortyEightKRetainsThePriorThreeDataOneNoDataCadence) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, 48'000, kInternalTxSytInterval));

    constexpr bool expectedData[] = {true, true, true, false, true};
    constexpr uint16_t expectedOffsets[] = {0, 1'024, 2'048,
                                             ::ASFW::Protocols::Audio::AMDTP::kNoSytOffset,
                                             0};
    for (uint64_t index = 0; index < std::size(expectedData); ++index) {
        InternalTxPacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        EXPECT_EQ(plan.sequence, index);
        EXPECT_EQ(plan.cadenceCycle, index);
        EXPECT_EQ(plan.isData, expectedData[index]);
        EXPECT_EQ(plan.dataBlocks, expectedData[index] ? kInternalTxSytInterval : 0U);
        EXPECT_EQ(plan.sytOffsetTicks, expectedOffsets[index]);
        EXPECT_TRUE(timing.CommitPacket(plan, plan.isData));
    }
}

TEST(MAudioInternalTxTimingTests, SytLeadUsesTheRateDependentTransferDelay) {
    for (const auto [rate, interval] :
         {std::pair{48'000U, uint8_t{8}}, std::pair{96'000U, uint8_t{16}},
          std::pair{192'000U, uint8_t{32}}}) {
        InternalTxTiming timing;
        ASSERT_TRUE(timing.Arm(kEpoch, rate, interval));
        for (uint32_t transmitCycle : {0U, 7U, 15U, 16U, 4'383U}) {
            InternalTxPacketPlan plan{};
            ASSERT_TRUE(timing.PreviewNextPacket(plan));
            if (plan.isData) {
                const uint16_t syt = ComputeInternalTxSyt(
                    plan.sytOffsetTicks, transmitCycle, timing.TransferDelayTicks());
                EXPECT_EQ(LeadTicks(syt, transmitCycle),
                          plan.sytOffsetTicks + timing.TransferDelayTicks());
            }
            ASSERT_TRUE(timing.CommitPacket(plan, plan.isData));
        }
    }
}

TEST(MAudioInternalTxTimingTests, NoDataFallbackStillAdvancesTheSharedCadence) {
    InternalTxTiming fallback;
    InternalTxTiming reference;
    ASSERT_TRUE(fallback.Arm(kEpoch, 96'000, 16));
    ASSERT_TRUE(reference.Arm(kEpoch, 96'000, 16));

    for (uint32_t cycle = 0; cycle < 128; ++cycle) {
        InternalTxPacketPlan fallbackPlan{};
        InternalTxPacketPlan referencePlan{};
        ASSERT_TRUE(fallback.PreviewNextPacket(fallbackPlan));
        ASSERT_TRUE(reference.PreviewNextPacket(referencePlan));
        EXPECT_EQ(fallbackPlan.isData, referencePlan.isData);
        EXPECT_EQ(fallbackPlan.sytOffsetTicks, referencePlan.sytOffsetTicks);
        ASSERT_TRUE(fallback.CommitPacket(fallbackPlan, false));
        ASSERT_TRUE(reference.CommitPacket(referencePlan, referencePlan.isData));
    }
}

TEST(MAudioInternalTxTimingTests, RejectsAStalePreviewAndDisarmsCleanly) {
    InternalTxTiming timing;
    ASSERT_TRUE(timing.Arm(kEpoch, 48'000, kInternalTxSytInterval));

    InternalTxPacketPlan plan{};
    ASSERT_TRUE(timing.PreviewNextPacket(plan));
    EXPECT_TRUE(timing.CommitPacket(plan, plan.isData));
    EXPECT_FALSE(timing.CommitPacket(plan, plan.isData));

    timing.Disarm();
    EXPECT_TRUE(std::holds_alternative<InternalTxTimingStopped>(timing.State()));
    EXPECT_FALSE(timing.IsArmed());
    EXPECT_FALSE(timing.PreviewNextPacket(plan));
}

} // namespace
