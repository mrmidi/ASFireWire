// SPDX-License-Identifier: Apache-2.0
#include "Audio/DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "Audio/DriverKit/Runtime/MotuRxSphClockSnapshot.hpp"
#include "Common/TimingUtils.hpp"
#include <gtest/gtest.h>
using namespace ASFW::Audio::Runtime;
using namespace ASFW::Encoding::Motu;

TEST(MotuRxSphRateMeterTests, RawClockMeasuresEveryRateAcrossSecondWrap) {
    for (uint32_t rate : {44100U,48000U,88200U,96000U,176400U,192000U}) {
        SCOPED_TRACE(rate); MotuRxSphRateMeter meter;
        const uint32_t blocks=rate/48000*8+8;
        for (uint64_t packet=0;packet<=2000;++packet) {
            const int64_t ticks=static_cast<int64_t>(packet*blocks*kTicksPerSecond/rate);
            meter.Observe(SphFromTick(WrapTick(kTicksPerSecond-100+ticks)),blocks,rate);
        }
        const auto result=meter.CumulativeSnapshot();
        ASSERT_TRUE(result.valid); EXPECT_EQ(result.frames,2000ULL*blocks);
        EXPECT_EQ(result.ticks,static_cast<int64_t>(2000ULL*blocks*kTicksPerSecond/rate));
        EXPECT_EQ(meter.Rejected(),0U);
    }
}
TEST(MotuRxSphRateMeterTests, UsesPreviousPacketFrameCountForVariablePackets) {
    MotuRxSphRateMeter meter;
    meter.Observe(SphFromTick(0),8,48000);
    meter.Observe(SphFromTick(4096),16,48000);
    meter.Observe(SphFromTick(12288),8,48000);
    EXPECT_EQ(meter.CumulativeSnapshot().frames,24U);
    EXPECT_EQ(meter.CumulativeSnapshot().ticks,12288);
}
TEST(MotuRxSphRateMeterTests, MissingNoDataAndRejectedStepsDoNotInventClockProgress) {
    MotuRxSphRateMeter meter;
    meter.Observe(SphFromTick(0),8,48000);
    meter.Observe(SphFromTick(4096),8,48000);
    meter.Observe(0,0,48000);
    meter.Observe(SphFromTick(8192),8,48000);
    const auto before=meter.CumulativeSnapshot();
    meter.Observe(SphFromTick(8192),8,48000);
    meter.Observe(SphFromTick(4096),8,48000);
    meter.Observe(SphFromTick(90000),8,48000);
    EXPECT_EQ(meter.CumulativeSnapshot().frames,before.frames);
    EXPECT_EQ(meter.CumulativeSnapshot().ticks,before.ticks);
    EXPECT_EQ(meter.Rejected(),3U);
    meter.Observe(SphFromTick(94096),8,48000);
    EXPECT_EQ(meter.CumulativeSnapshot().ticks,before.ticks+4096);
}
TEST(MotuRxSphRateMeterTests, ReanchorPreservesTotalsAndResetStartsANewClock) {
    MotuRxSphRateMeter meter;
    meter.Observe(SphFromTick(0),8,48000);
    meter.Observe(SphFromTick(4096),8,48000);
    meter.Reanchor();
    meter.Observe(SphFromTick(100000),8,48000);
    EXPECT_EQ(meter.CumulativeSnapshot().ticks,4096);
    meter.Observe(SphFromTick(104096),8,48000);
    EXPECT_EQ(meter.CumulativeSnapshot().ticks,8192);
    meter.Reset(); EXPECT_FALSE(meter.CumulativeSnapshot().valid);
}
TEST(MotuRxSphRateMeterTests, MalformedSphCannotBecomeAClockOrPhaseSample) {
    MotuRxSphRateMeter meter;
    meter.Observe(0x02000000,8,48000);
    meter.ObserveAbsolutePhase(0x02000000,0);
    EXPECT_FALSE(meter.CumulativeSnapshot().valid);
    EXPECT_FALSE(meter.AbsolutePhaseSnapshot().valid);
    EXPECT_EQ(meter.Rejected(),1U);
}
TEST(MotuRxSphRateMeterTests, AbsoluteMedianIsBoundedAndSurvivesSecondWrap) {
    MotuRxSphRateMeter meter;
    const auto cycle=ASFW::Timing::encodeCycleTimer(0,7999,0);
    for (int64_t phase : {5000,5100,10000,5050})
        meter.ObserveAbsolutePhase(SphFromTick(WrapTick(7999LL*kTicksPerCycle+phase)),cycle);
    const auto window=meter.AbsolutePhaseSnapshot();
    ASSERT_TRUE(window.valid); EXPECT_EQ(window.medianTicks,5100);
    EXPECT_EQ(window.minTicks,5000); EXPECT_EQ(window.maxTicks,10000);
    for (int i=0;i<100;++i) meter.ObserveAbsolutePhase(SphFromTick(1),0);
    EXPECT_EQ(meter.AbsolutePhaseSnapshot().stored,64U);
    meter.BeginWindow();EXPECT_FALSE(meter.AbsolutePhaseSnapshot().valid);
}
TEST(MotuRxSphClockSnapshotTests, RefusesWrongGenerationAndNonadvancingCounts) {
    MotuRxSphClockLatest bridge;bridge.Reset(7);
    EXPECT_FALSE(bridge.Publish({8,8,4096}));
    ASSERT_TRUE(bridge.Publish({7,8,4096}));
    EXPECT_FALSE(bridge.Publish({7,8,8192}));
    EXPECT_FALSE(bridge.Publish({7,16,4000}));
    ASSERT_TRUE(bridge.Publish({7,16,8192}));
    MotuRxSphClockSample sample;uint64_t updates=0;
    ASSERT_TRUE(bridge.ReadLatest(sample,updates));EXPECT_EQ(updates,2U);
    EXPECT_EQ(sample.rxFrames,16U);EXPECT_EQ(sample.rxTicks,8192);
    bridge.Reset(8);EXPECT_FALSE(bridge.ReadLatest(sample,updates));
}
