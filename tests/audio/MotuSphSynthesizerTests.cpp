// SPDX-License-Identifier: Apache-2.0
#include "Audio/Wire/MOTU/MotuSphSynthesizer.hpp"
#include <gtest/gtest.h>
#include <array>
using namespace ASFW::Encoding::Motu;

TEST(MotuSynthesis, SeedUsesFullExecutionTimestampAndLeadAndCannotBeOverwritten) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000, 510));
    ASSERT_TRUE(synth.SeedOnce(247 + 3 * kTicksPerCycle));
    EXPECT_FALSE(synth.SeedOnce(42));
    EXPECT_EQ(TickFromSph(synth.NextSph()), 247U + 3 * kTicksPerCycle);
    EXPECT_EQ(TickFromSph(synth.NextSph()), 247U + 3 * kTicksPerCycle + 512U);
}
TEST(MotuSynthesis, FractionalClockAtEveryRateCrossesSecondWrapWithoutLosingFraction) {
    for (uint32_t rate : {44100U,48000U,88200U,96000U,176400U,192000U}) {
        SCOPED_TRACE(rate);
        SphSynthesizer synth;
        ASSERT_TRUE(synth.Configure(rate));
        ASSERT_TRUE(synth.SeedOnce(kTicksPerSecond - 73));
        const uint64_t step = SphSynthesizer::NominalStepQ32(rate);
        for (uint64_t frame = 0; frame < uint64_t{rate} * 2; ++frame) {
            const auto expected = (uint64_t{kTicksPerSecond - 73} + ((frame * step) >> 32)) % kTicksPerSecond;
            ASSERT_EQ(TickFromSph(synth.NextSph()), expected);
        }
    }
}
TEST(MotuSynthesis, NominalClockLocksOnlyAfterOneRxDecisionInterval) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000));
    ASSERT_TRUE(synth.Observe(1,1,8,4096));
    EXPECT_FALSE(synth.Current().locked);
    EXPECT_FALSE(synth.Observe(1,2,512,512*512));
    ASSERT_TRUE(synth.Observe(1,3,520,520*512));
    EXPECT_TRUE(synth.Current().locked);
    EXPECT_FALSE(synth.Current().outputMuted);
}
TEST(MotuSynthesis, RateUsesRxFramesWhenTxAdvancesAtADifferentCadence) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000));
    ASSERT_TRUE(synth.SeedOnce(0));
    ASSERT_TRUE(synth.Observe(1,1,8,8*512));
    for (int i=0;i<1000;++i) (void)synth.NextSph();
    ASSERT_TRUE(synth.Observe(1,2,520,520*512));
    EXPECT_EQ(synth.Current().measuredStepQ32, 512*SphSynthesizer::kOneQ32);
    EXPECT_EQ(synth.Current().phaseErrorTicks, 0);
}
TEST(MotuSynthesis, AbsolutePhaseSeedsOnceWithoutDroppingAnExistingLock) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000,510));
    ASSERT_TRUE(synth.SeedOnce(9216));
    ASSERT_TRUE(synth.Observe(1,1,8,4096));
    ASSERT_TRUE(synth.Observe(1,2,520,520*512));
    ASSERT_TRUE(synth.Current().locked);
    ASSERT_TRUE(synth.Observe(1,3,1032,1032*512,900,900));
    EXPECT_EQ(synth.Current().phaseErrorTicks,-390);
    EXPECT_TRUE(synth.Current().locked);
    EXPECT_EQ(synth.Current().seedReReferences,1U);
    ASSERT_TRUE(synth.Observe(1,4,1544,1544*512,1200,1200));
    EXPECT_FALSE(synth.Current().phaseReferenceReset);
    EXPECT_EQ(synth.Current().seedReReferences,1U);
    EXPECT_EQ(synth.Current().phaseErrorTicks,-390);
}
TEST(MotuSynthesis, LongerDecisionIntervalUsesActualElapsedFrames) {
    for (uint64_t elapsed : {512U,1024U,2048U}) {
        SphSynthesizer synth;
        ASSERT_TRUE(synth.Configure(48000,510));
        ASSERT_TRUE(synth.Observe(1,1,8,4096,810,810));
        ASSERT_TRUE(synth.Observe(1,2,8+elapsed,(8+elapsed)*512));
        EXPECT_EQ(synth.Current().stepQ32,512*SphSynthesizer::kOneQ32-
                  300*SphSynthesizer::kOneQ32/static_cast<int64_t>(elapsed));
    }
}
TEST(MotuSynthesis, AcquisitionLimitsAllowMeasuredVendorExcursion) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000,510));
    ASSERT_TRUE(synth.Observe(1,1,8,4096,2010,2010));
    ASSERT_TRUE(synth.Observe(1,2,520,520*512));
    EXPECT_LT(synth.Current().stepQ32,512*SphSynthesizer::kOneQ32*998/1000);
    EXPECT_FALSE(synth.Current().stepClamped);
}
TEST(MotuSynthesis, RejectsRepeatedRegressingAndZeroObservationsWithoutChangingStep) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000));
    ASSERT_TRUE(synth.Observe(1,1,8,4096));
    const auto before=synth.Current();
    EXPECT_FALSE(synth.Observe(1,1,8,4096));
    EXPECT_FALSE(synth.Observe(1,2,7,5000));
    EXPECT_FALSE(synth.Observe(1,2,20,4000));
    EXPECT_FALSE(synth.Observe(0,2,20,5000));
    EXPECT_EQ(synth.Current().stepQ32,before.stepQ32);
    EXPECT_EQ(synth.Current().decisions,before.decisions);
}
TEST(MotuSynthesis, HardResyncMutesAndAccountsForRepairOnlyOnce) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000));
    ASSERT_TRUE(synth.SeedOnce(10000));
    ASSERT_TRUE(synth.Observe(1,1,8,4096));
    ASSERT_TRUE(synth.Observe(1,2,520,520*512));
    ASSERT_TRUE(synth.Current().locked);
    ASSERT_TRUE(synth.Observe(1,3,1032,1032*512+13000));
    EXPECT_TRUE(synth.Current().outputMuted);
    EXPECT_EQ(synth.Current().phaseRepairs,1U);
    EXPECT_EQ(synth.Current().txCorrectionQ32,13000*SphSynthesizer::kOneQ32);
    const int64_t step=synth.Current().stepQ32;
    for (int i=0;i<512;++i) (void)synth.NextSph();
    const int64_t expectedCorrection=(step-512*SphSynthesizer::kOneQ32)*512;
    ASSERT_TRUE(synth.Observe(1,4,1544,1544*512+13000+expectedCorrection/SphSynthesizer::kOneQ32));
    EXPECT_EQ(synth.Current().phaseRepairs,1U);
    EXPECT_TRUE(synth.Current().locked);
}
TEST(MotuSynthesis, GenerationAndResetDiscardOldPresentationClock) {
    SphSynthesizer synth;
    ASSERT_TRUE(synth.Configure(48000));
    ASSERT_TRUE(synth.SeedOnce(0));
    ASSERT_TRUE(synth.Observe(1,1,8,4096));
    ASSERT_TRUE(synth.Observe(2,1,8,4096));
    EXPECT_FALSE(synth.Current().seeded);
    EXPECT_FALSE(synth.Current().locked);
    synth.Reset();
    EXPECT_EQ(synth.Current().streamGeneration,0U);
    EXPECT_TRUE(synth.Current().outputMuted);
    EXPECT_EQ(synth.Current().txCorrectionQ32,0);
    EXPECT_FALSE(synth.Configure(0));
    EXPECT_FALSE(synth.Configure(32000));
}
TEST(MotuSynthesis, DecisionTimeScalesAcrossRatesAndWholeDataPackets) {
    for (const auto [rate,frames] : std::array<std::pair<uint32_t,uint64_t>,6>{{
        {44100,472},{48000,512},{88200,944},{96000,1024},{176400,1888},{192000,2048}}}) {
        SphSynthesizer synth;ASSERT_TRUE(synth.Configure(rate));
        EXPECT_EQ(synth.DecisionIntervalFrames(),frames);
    }
}
