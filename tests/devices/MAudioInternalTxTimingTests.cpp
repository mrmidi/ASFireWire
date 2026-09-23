#include <gtest/gtest.h>

#include "Audio/Protocols/BeBoB/MAudioInternalTxTiming.hpp"

namespace ASFW::Audio::BeBoB {
namespace {

TEST(MAudioInternalTxTimingTests, UsesSixFramesPerCycleAndEightBlockInterval) {
    EXPECT_EQ(kMAudioInternalTxSampleRateHz / 8'000,
              kMAudioInternalTxFramesPerCycle);
    EXPECT_EQ(kMAudioInternalTxSytInterval, 8);
}

TEST(MAudioInternalTxTimingTests, PreviewsAndCommitsThreeDataThenNoData) {
    MAudioInternalTxTiming timing;
    ASSERT_TRUE(timing.Arm());

    for (uint64_t sequence = 0; sequence < 8; ++sequence) {
        MAudioInternalTxTiming::PacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        EXPECT_EQ(plan.sequence, sequence);
        EXPECT_EQ(plan.cadenceCycle, sequence);
        const bool expectedData = sequence % 4 != 3;
        EXPECT_EQ(plan.isData, expectedData);
        EXPECT_EQ(plan.dataBlocks, expectedData ? 8 : 0);
        EXPECT_TRUE(expectedData
                        ? timing.CommitPacket(plan, true)
                        : timing.CommitPacket(plan, false));
    }
}

TEST(MAudioInternalTxTimingTests, DataSlotCanFallBackToNoDataWithoutShiftingCadence) {
    MAudioInternalTxTiming timing;
    ASSERT_TRUE(timing.Arm());

    MAudioInternalTxTiming::PacketPlan plan{};
    ASSERT_TRUE(timing.PreviewNextPacket(plan));
    ASSERT_TRUE(plan.isData);
    ASSERT_TRUE(timing.CommitPacket(plan, false));

    ASSERT_TRUE(timing.PreviewNextPacket(plan));
    EXPECT_EQ(plan.sequence, 1);
    EXPECT_EQ(plan.cadenceCycle, 1);
    EXPECT_TRUE(plan.isData);
    EXPECT_TRUE(timing.CommitPacket(plan, true));
}

TEST(MAudioInternalTxTimingTests, RejectsStalePlansAndDataInNoDataSlot) {
    MAudioInternalTxTiming timing;
    ASSERT_TRUE(timing.Arm());

    MAudioInternalTxTiming::PacketPlan first{};
    ASSERT_TRUE(timing.PreviewNextPacket(first));
    auto stale = first;
    ++stale.sequence;
    EXPECT_FALSE(timing.CommitPacket(stale, true));
    ASSERT_TRUE(timing.CommitPacket(first, true));

    for (unsigned i = 0; i < 2; ++i) {
        MAudioInternalTxTiming::PacketPlan plan{};
        ASSERT_TRUE(timing.PreviewNextPacket(plan));
        ASSERT_TRUE(timing.CommitPacket(plan, true));
    }
    MAudioInternalTxTiming::PacketPlan noData{};
    ASSERT_TRUE(timing.PreviewNextPacket(noData));
    EXPECT_FALSE(noData.isData);
    EXPECT_FALSE(timing.CommitPacket(noData, true));
    EXPECT_TRUE(timing.CommitPacket(noData, false));
}

TEST(MAudioInternalTxTimingTests, SytUsesTransmitCycleAndBlockingTransferDelay) {
    EXPECT_EQ(MAudioInternalTxSyt(0, 10, 12'800), 0xE200);
    EXPECT_EQ(MAudioInternalTxSyt(1'024, 10, 12'800), 0xE600);
    EXPECT_EQ(MAudioInternalTxSyt(2'048, 10, 12'800), 0xEA00);
}

} // namespace
} // namespace ASFW::Audio::BeBoB
