#include <gtest/gtest.h>

#include "../ASFWDriver/Isoch/Core/ExternalSyncDiscipline48k.hpp"

using ASFW::Isoch::Core::ExternalSyncDiscipline48k;

namespace {
uint16_t EncodeSytFromTick(int32_t tick) {
    constexpr int32_t kDomain = ExternalSyncDiscipline48k::kTickDomain;
    int32_t normalized = tick % kDomain;
    if (normalized < 0) {
        normalized += kDomain;
    }
    const uint16_t cycle4 = static_cast<uint16_t>((normalized / ExternalSyncDiscipline48k::kTicksPerCycle) & 0x0F);
    const uint16_t ticks12 = static_cast<uint16_t>(normalized % ExternalSyncDiscipline48k::kTicksPerCycle);
    return static_cast<uint16_t>((cycle4 << 12) | ticks12);
}

void PrimeBaseline(ExternalSyncDiscipline48k& discipline, int32_t phaseTicks) {
    for (uint32_t i = 0; i < ExternalSyncDiscipline48k::kBaselineWindow; ++i) {
        (void)discipline.Update(/*enabled=*/true,
                                EncodeSytFromTick(0),
                                EncodeSytFromTick(phaseTicks));
    }
}
} // namespace

TEST(ExternalSyncDiscipline48k, LearnsBaselineAfterStableWindow) {
    ExternalSyncDiscipline48k discipline;

    for (uint32_t i = 0; i < ExternalSyncDiscipline48k::kBaselineWindow - 1; ++i) {
        auto result = discipline.Update(/*enabled=*/true,
                                        EncodeSytFromTick(0),
                                        EncodeSytFromTick(480));
        EXPECT_TRUE(result.active);
        EXPECT_FALSE(result.locked);
        EXPECT_EQ(result.correctionTicks, 0);
    }

    auto lockResult = discipline.Update(/*enabled=*/true,
                                        EncodeSytFromTick(0),
                                        EncodeSytFromTick(480));
    EXPECT_TRUE(lockResult.locked);
    EXPECT_EQ(lockResult.correctionTicks, 0);
}

TEST(ExternalSyncDiscipline48k, DeadbandProducesNoCorrection) {
    ExternalSyncDiscipline48k discipline;
    PrimeBaseline(discipline, /*phaseTicks=*/500);

    auto result = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(0),
                                    EncodeSytFromTick(520));
    EXPECT_TRUE(result.locked);
    EXPECT_EQ(result.correctionTicks, 0);
    EXPECT_EQ(result.phaseErrorTicks, 20);
}

TEST(ExternalSyncDiscipline48k, CorrectionUsesSignedOneTickStep) {
    ExternalSyncDiscipline48k disciplinePos;
    PrimeBaseline(disciplinePos, /*phaseTicks=*/400);
    auto positive = disciplinePos.Update(/*enabled=*/true,
                                         EncodeSytFromTick(0),
                                         EncodeSytFromTick(500));
    EXPECT_EQ(positive.correctionTicks, +1);

    ExternalSyncDiscipline48k disciplineNeg;
    PrimeBaseline(disciplineNeg, /*phaseTicks=*/400);
    auto negative = disciplineNeg.Update(/*enabled=*/true,
                                         EncodeSytFromTick(0),
                                         EncodeSytFromTick(300));
    EXPECT_EQ(negative.correctionTicks, -1);
}

TEST(ExternalSyncDiscipline48k, PhaseDetectorIgnoresWholePacketIntervals) {
    ExternalSyncDiscipline48k discipline;
    PrimeBaseline(discipline, /*phaseTicks=*/500);

    // 500 -> 500 + N*4096 should be treated as the same phase (mod packet interval).
    auto base = discipline.Update(/*enabled=*/true,
                                  EncodeSytFromTick(0),
                                  EncodeSytFromTick(500));
    EXPECT_EQ(base.phaseErrorTicks, 0);
    EXPECT_EQ(base.correctionTicks, 0);

    auto plus1 = discipline.Update(/*enabled=*/true,
                                   EncodeSytFromTick(0),
                                   EncodeSytFromTick(500 + ExternalSyncDiscipline48k::kPacketIntervalTicks));
    EXPECT_EQ(plus1.phaseErrorTicks, 0);
    EXPECT_EQ(plus1.correctionTicks, 0);

    auto minus2 = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(0),
                                    EncodeSytFromTick(500 - 2 * ExternalSyncDiscipline48k::kPacketIntervalTicks));
    EXPECT_EQ(minus2.phaseErrorTicks, 0);
    EXPECT_EQ(minus2.correctionTicks, 0);
}
