// SPDX-License-Identifier: Apache-2.0
#include "Audio/Wire/MOTU/MotuSphSynthesizer.hpp"
#include <array>
#include <gtest/gtest.h>
using namespace ASFW::Encoding::Motu;
namespace {
using Observation = SphSynthesizer::Observation;
Observation At(uint64_t frame, uint64_t update, int64_t phase = 0, uint64_t behind = 0) {
    return {.generation = 1,
            .updates = update,
            .frames = frame + 8,
            .ticks = static_cast<int64_t>(frame + 8) * 512,
            .rxFirstFrame = 10000 + frame - behind,
            .txFirstFrame = 10000 + frame,
            .sphTicks =
                WrapTick((static_cast<int64_t>(frame) - static_cast<int64_t>(behind)) * 512 -
                         13 * 512 + phase),
            .replayEpoch = 1};
}
void Advance(SphSynthesizer& synth, uint64_t frames) {
    for (uint64_t f = 0; f < frames; ++f)
        (void)synth.NextSph();
}
void Arm(SphSynthesizer& synth, int64_t seed = 0) {
    ASSERT_TRUE(synth.Configure(48000));
    synth.BeginGeneration(1);
    ASSERT_TRUE(synth.SeedOnce(seed));
}
} // namespace
TEST(MotuSynthesis, SeedUsesFullExecutionTimestampAndLeadAndCannotBeOverwritten) {
    SphSynthesizer synth;
    Arm(synth, 247 + 3 * kTicksPerCycle);
    EXPECT_FALSE(synth.SeedOnce(42));
    EXPECT_EQ(TickFromSph(synth.NextSph()), 247U + 3 * kTicksPerCycle);
    EXPECT_EQ(TickFromSph(synth.NextSph()), 247U + 3 * kTicksPerCycle + 512U);
}
TEST(MotuSynthesis, FirstObservationAfterExternalSeedOnlyEstablishesRateReference) {
    SphSynthesizer synth;
    Arm(synth, 1000000);
    ASSERT_TRUE(synth.Observe(At(0, 1, 1001440)));
    EXPECT_EQ(TickFromSph(synth.NextSph()), 1000000U);
    EXPECT_EQ(synth.Current().phaseRepairs, 0U);
    EXPECT_EQ(synth.Current().stepQ32, 512 * SphSynthesizer::kOneQ32);
}
TEST(MotuSynthesis, FractionalClockAtEveryRateRoundsAndCrossesSecondWrap) {
    for (uint32_t rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
        SCOPED_TRACE(rate);
        SphSynthesizer synth;
        ASSERT_TRUE(synth.Configure(rate));
        ASSERT_TRUE(synth.SeedOnce(kTicksPerSecond - 73));
        const uint64_t step = SphSynthesizer::NominalStepQ32(rate);
        for (uint64_t f = 0; f < uint64_t{rate} * 2; ++f) {
            const uint32_t expected =
                WrapTick(kTicksPerSecond - 73 + ((f * step + (uint64_t{1} << 31)) >> 32));
            ASSERT_EQ(TickFromSph(synth.NextSph()), expected);
        }
    }
}
TEST(MotuSynthesis, FreshAbsoluteObservationCorrectsAgainAfterLock) {
    SphSynthesizer synth;
    Arm(synth);
    ASSERT_TRUE(synth.Observe(At(0, 1)));
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(512, 2)));
    ASSERT_TRUE(synth.Current().locked);
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(1024, 3, 500)));
    EXPECT_EQ(synth.Current().phaseErrorTicks, 500);
    EXPECT_EQ(synth.Current().stepQ32,
              512 * SphSynthesizer::kOneQ32 + 500 * SphSynthesizer::kOneQ32 / (512 * 4));
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(1536, 4, 0)));
    EXPECT_EQ(synth.Current().phaseErrorTicks, -125);
    EXPECT_EQ(synth.Current().phaseRepairs, 0U);
}
TEST(MotuSynthesis, RateUsesRxFramesWhenTxAdvancesAtDifferentCadence) {
    SphSynthesizer synth;
    Arm(synth);
    ASSERT_TRUE(synth.Observe(At(0, 1)));
    Advance(synth, 1000);
    auto sample = At(1000, 2);
    sample.frames = 520;
    sample.ticks = 520 * 512;
    ASSERT_TRUE(synth.Observe(sample));
    EXPECT_EQ(synth.Current().measuredStepQ32, 512 * SphSynthesizer::kOneQ32);
    EXPECT_EQ(synth.Current().phaseErrorTicks, 0);
}
TEST(MotuSynthesis, FrameCorrespondenceIsIndependentOfCadenceDelayAndSecondWrap) {
    // Sample the same device clock with differing RX history ages. The phase
    // prediction uses exact frame identities, not rounded cycle/packet delays.
    for (uint64_t behind : {0U, 8U, 16U, 24U, 32U, 256U, 4096U}) {
        SCOPED_TRACE(behind);
        SphSynthesizer synth;
        Arm(synth, kTicksPerSecond - 500);
        ASSERT_TRUE(synth.Observe(At(0, 1, kTicksPerSecond - 500, behind)));
        for (uint64_t update = 2; update < 12; ++update) {
            Advance(synth, 512);
            ASSERT_TRUE(
                synth.Observe(At((update - 1) * 512, update, kTicksPerSecond - 500, behind)));
            EXPECT_EQ(synth.Current().phaseErrorTicks, 0);
        }
    }
}
TEST(MotuSynthesis, LongerUpdateUsesActualElapsedFrames) {
    for (uint64_t elapsed : {512U, 1024U, 2048U}) {
        SphSynthesizer synth;
        Arm(synth, 300);
        ASSERT_TRUE(synth.Observe(At(0, 1)));
        Advance(synth, elapsed);
        ASSERT_TRUE(synth.Observe(At(elapsed, 2)));
        EXPECT_EQ(synth.Current().stepQ32,
                  512 * SphSynthesizer::kOneQ32 -
                      300 * SphSynthesizer::kOneQ32 / static_cast<int64_t>(elapsed));
    }
}
TEST(MotuSynthesis, AcquisitionCanUseMeasuredVendorExcursionAndClampExtremeRequest) {
    SphSynthesizer synth;
    Arm(synth, 1500);
    ASSERT_TRUE(synth.Observe(At(0, 1)));
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(512, 2)));
    EXPECT_LT(synth.Current().stepQ32, 512 * SphSynthesizer::kOneQ32 * 998 / 1000);
    EXPECT_FALSE(synth.Current().stepClamped);
    Advance(synth, 512);
    auto sample = At(1024, 3);
    sample.ticks += 512 * 128;
    ASSERT_TRUE(synth.Observe(sample));
    EXPECT_TRUE(synth.Current().stepClamped);
}
TEST(MotuSynthesis, RejectsStaleGenerationDuplicateAndInvalidProjection) {
    SphSynthesizer synth;
    Arm(synth);
    auto sample = At(0, 1);
    ASSERT_TRUE(synth.Observe(sample));
    EXPECT_FALSE(synth.Observe(sample));
    sample = At(512, 2);
    sample.generation = 2;
    EXPECT_FALSE(synth.Observe(sample));
    sample.generation = 1;
    sample.txFirstFrame += 48000;
    EXPECT_FALSE(synth.Observe(sample));
    sample = At(512, 2);
    sample.sphTicks = kTicksPerSecond;
    EXPECT_FALSE(synth.Observe(sample));
    EXPECT_EQ(synth.Current().stepQ32, 512 * SphSynthesizer::kOneQ32);
}
TEST(MotuSynthesis, HardPhaseRepairMutesOnceAndRelocksAgainstFreshStamp) {
    SphSynthesizer synth;
    Arm(synth);
    ASSERT_TRUE(synth.Observe(At(0, 1)));
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(512, 2)));
    ASSERT_FALSE(synth.Current().outputMuted);
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(1024, 3, 13000)));
    EXPECT_TRUE(synth.Current().outputMuted);
    EXPECT_EQ(synth.Current().phaseRepairs, 1U);
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(1536, 4, 13000)));
    EXPECT_TRUE(synth.Current().locked);
    EXPECT_FALSE(synth.Current().outputMuted);
    EXPECT_EQ(synth.Current().phaseRepairs, 1U);
    EXPECT_EQ(synth.Current().phaseErrorTicks, 0);
}
TEST(MotuSynthesis, OnePacketMappingErrorIsBelowHardRepairThreshold) {
    SphSynthesizer synth;
    Arm(synth);
    ASSERT_TRUE(synth.Observe(At(0, 1)));
    Advance(synth, 512);
    auto sample = At(512, 2);
    sample.rxFirstFrame += 8;
    ASSERT_TRUE(synth.Observe(sample));
    EXPECT_EQ(synth.Current().phaseErrorTicks, -4096);
    EXPECT_FALSE(synth.Current().hardResyncRequired);
}
TEST(MotuSynthesis, ReplayRebindRestartsRateReferenceWithoutReplacingExternalClock) {
    SphSynthesizer synth;
    Arm(synth);
    ASSERT_TRUE(synth.Observe(At(0, 1)));
    Advance(synth, 512);
    ASSERT_TRUE(synth.Observe(At(512, 2)));
    Advance(synth, 512);
    auto sample = At(1024, 3);
    sample.replayEpoch = 2;
    ASSERT_TRUE(synth.Observe(sample));
    EXPECT_EQ(synth.Current().referenceCause, MotuSphReferenceCause::kRxRebind);
    EXPECT_TRUE(synth.Current().outputMuted);
    EXPECT_EQ(TickFromSph(synth.NextSph()), 1024U * 512);
}
TEST(MotuSynthesis, GenerationResetDiscardsOldClockAndInvalidRateFailsConfiguration) {
    SphSynthesizer synth;
    Arm(synth);
    ASSERT_TRUE(synth.Observe(At(0, 1)));
    synth.BeginGeneration(2);
    EXPECT_FALSE(synth.Current().seeded);
    EXPECT_TRUE(synth.Current().outputMuted);
    EXPECT_FALSE(synth.Observe(At(512, 2)));
    EXPECT_FALSE(synth.Configure(0));
    EXPECT_FALSE(synth.Configure(32000));
}
TEST(MotuSynthesis, DecisionTimeScalesAcrossRatesAndWholeDataPackets) {
    for (const auto [rate, frames] :
         std::array<std::pair<uint32_t, uint64_t>, 6>{{{44100, 472},
                                                       {48000, 512},
                                                       {88200, 944},
                                                       {96000, 1024},
                                                       {176400, 1888},
                                                       {192000, 2048}}}) {
        SphSynthesizer synth;
        ASSERT_TRUE(synth.Configure(rate));
        EXPECT_EQ(synth.DecisionIntervalFrames(), frames);
        EXPECT_EQ(synth.PresentationOffsetFrames(), 13U);
    }
}

TEST(MotuSynthesis, ContinuousFeedbackTracksDriftAndPhaseStepAtEverySupportedRate) {
    for (uint32_t rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
        for (int64_t driftPpm : {-150, 150}) {
            SCOPED_TRACE(rate);
            SCOPED_TRACE(driftPpm);
            SphSynthesizer synth;
            ASSERT_TRUE(synth.Configure(rate));
            synth.BeginGeneration(1);
            const int64_t period =
                SphSynthesizer::NominalStepQ32(rate) * (1000000 + driftPpm) / 1000000;
            const auto stamp = [period](uint64_t frame) {
                return (static_cast<int64_t>(frame) * period + SphSynthesizer::kOneQ32 / 2) /
                       SphSynthesizer::kOneQ32;
            };
            constexpr int64_t origin = kTicksPerSecond - 1000;
            ASSERT_TRUE(synth.SeedOnce(origin + stamp(13) + 1500));
            const uint32_t packetFrames =
                ASFW::Encoding::AmdtpRateGeometryForSampleRate(rate)->sytIntervalFrames;
            for (uint64_t frame = 0; frame < uint64_t{rate} * 3; ++frame) {
                const int64_t phaseStep = frame >= rate ? 500 : 0;
                if (frame % packetFrames == 0) {
                    (void)synth.Observe({.generation = 1,
                                         .updates = frame / packetFrames + 1,
                                         .frames = frame + packetFrames,
                                         .ticks = stamp(frame + packetFrames) + phaseStep,
                                         .rxFirstFrame = frame,
                                         .txFirstFrame = frame,
                                         .sphTicks = WrapTick(origin + stamp(frame) + phaseStep),
                                         .replayEpoch = 1});
                }
                const int64_t transmitted = TickFromSph(synth.NextSph());
                if (frame > uint64_t{rate} * 2) {
                    ASSERT_NEAR(TickDifference(transmitted,
                                               WrapTick(origin + stamp(frame + 13) + phaseStep)),
                                0, 4)
                        << "frame " << frame;
                }
            }
            EXPECT_TRUE(synth.Current().locked);
            EXPECT_FALSE(synth.Current().outputMuted);
            EXPECT_EQ(synth.Current().phaseRepairs, 0U);
        }
    }
}
