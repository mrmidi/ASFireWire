#include <gtest/gtest.h>

#include "AudioWire/AMDTP/TimingUtils.hpp"

using namespace ASFW::Timing;

TEST(TimingUtils, OffsetDomainConstants) {
    EXPECT_EQ(kTicksPerSample48k, 512u);
    EXPECT_EQ(kSytInterval48k, 8u);
    EXPECT_EQ(kSytPacketStepTicks48k, 4096u);
    EXPECT_EQ(kEightSecondTicks, 196'608'000);
    EXPECT_EQ(kSytFieldDomainTicks, 49'152);
}

TEST(TimingUtils, TstampToOffsetsCollapsesSecondsCycleOffset) {
    EXPECT_EQ(tstampToOffsets(0, 0, 0), 0);
    EXPECT_EQ(tstampToOffsets(0, 978, 0), int64_t(3072) * 978);
    EXPECT_EQ(tstampToOffsets(0, 978, 4096), int64_t(3072) * 978 + 4096);
    EXPECT_EQ(tstampToOffsets(1, 0, 0), int64_t(kTicksPerSecond));
    EXPECT_EQ(tstampToOffsets(2, 1, 5), int64_t(3072) * (1 + 8000 * 2) + 5);
}

TEST(TimingUtils, ExtendTstampReconstructsPresentationTimestamp) {
    // SYT 0x79FE = [cycle4=7][offset=0x9FE]. Base cycle 978 -> nearest
    // cycle >= 978 with low nibble 7 is 983.
    EXPECT_EQ(extendTstamp(978, 0x79FE), int64_t(3072) * 983 + 0x9FE);
}

TEST(TimingUtils, ExtendTstampMatchesAcceptedAnchorPresentationLead) {
    EXPECT_EQ(extendTstamp(978, 0x3400), tstampToOffsets(0, 978, 4096));
}

TEST(TimingUtils, ExtOffsetDiffSignedShortestPath) {
    EXPECT_EQ(extOffsetDiff(28158, 24062), 4096);
    EXPECT_EQ(extOffsetDiff(24062, 28158), -4096);
    EXPECT_EQ(extOffsetDiff(100, 100), 0);
}

TEST(TimingUtils, ExtOffsetDiffWrapsAcrossEightSecondDomain) {
    EXPECT_EQ(extOffsetDiff(10, kEightSecondTicks - 10), 20);
    EXPECT_EQ(extOffsetDiff(kEightSecondTicks - 10, 10), -20);
}

TEST(TimingUtils, SYTDiffInOffsetsConsecutiveDataPackets) {
    EXPECT_EQ(SYTDiffInOffsets(0x91FE, 0x79FE), 4096);
    EXPECT_EQ(SYTDiffInOffsets(0x79FE, 0x91FE), -4096);
}

TEST(TimingUtils, SYTDiffInOffsetsWrapsAcrossSixteenCycleField) {
    EXPECT_EQ(SYTDiffInOffsets(0x1300, 0xFB00), 4096);
}
