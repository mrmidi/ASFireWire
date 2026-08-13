// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioTxClockAdapter.hpp"
#include "ASFWDriver/Common/TimingUtils.hpp"

#include <cstdint>
#include <variant>

namespace {

using namespace ASFW::Audio::Families::BeBoB::MAudio;

constexpr StartEpoch kEpoch{17};
constexpr uint32_t kSampleRateHz = 48'000;
constexpr uint32_t kZtsPeriodFrames = 1'536;

struct TimebaseGuard {
    mach_timebase_info_data_t old{ASFW::Timing::gHostTimebaseInfo};

    TimebaseGuard(uint32_t numer, uint32_t denom) {
        ASFW::Timing::gHostTimebaseInfo.numer = numer;
        ASFW::Timing::gHostTimebaseInfo.denom = denom;
    }

    ~TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = old; }
};

[[nodiscard]] constexpr TxClockAnchor Anchor(uint32_t cycleTime,
                                              uint64_t hostTicks) {
    return TxClockAnchor{.cycleTime = cycleTime, .hostTicks = hostTicks};
}

TEST(MAudioTxClockAdapterTests,
     UsesThreeObservedTxGroupsBeforePublishingTheFirstPlaybackAnchor) {
    TimebaseGuard timebase(1, 1);
    TxClockAdapter adapter;
    ASSERT_TRUE(adapter.Arm(kEpoch, kSampleRateHz, kZtsPeriodFrames));
    EXPECT_TRUE(adapter.IsArmed());
    EXPECT_TRUE(std::holds_alternative<RunningTransport>(adapter.State()));

    const auto first = adapter.ObserveHardwareWake(1, Anchor(0x0010'0000, 1'000'000));
    EXPECT_FALSE(first.captureReferencePlanted);
    EXPECT_FALSE(first.playbackAnchorReady);

    const auto second = adapter.ObserveHardwareWake(2, Anchor(0x0010'1000, 1'750'000));
    EXPECT_TRUE(second.captureReferencePlanted);
    EXPECT_EQ(second.captureReference.cycleTime, 0x0010'1000U);
    EXPECT_EQ(second.captureReference.hostTicks, 1'750'000U);
    EXPECT_FALSE(second.playbackAnchorReady);

    const auto third = adapter.ObserveHardwareWake(3, Anchor(0x0010'2000, 2'500'000));
    EXPECT_FALSE(third.captureReferencePlanted);
    EXPECT_TRUE(third.playbackAnchorReady);
    EXPECT_EQ(third.sampleFrame, 0U);
    EXPECT_EQ(third.hostTicks, 2'500'000U);
    EXPECT_EQ(third.source.cycleTime, 0x0010'2000U);
    EXPECT_EQ(third.hostNanosPerSampleQ8,
              static_cast<uint32_t>((1'000'000'000ULL << 8) / kSampleRateHz));
    EXPECT_TRUE(IsPlaybackRunning(adapter.State()));
}

TEST(MAudioTxClockAdapterTests,
     ProjectsOnlyExactAudioDriverKitPeriodBoundariesFromTheTxOrigin) {
    TimebaseGuard timebase(1, 1);
    TxClockAdapter adapter;
    ASSERT_TRUE(adapter.Arm(kEpoch, kSampleRateHz, kZtsPeriodFrames));

    constexpr uint64_t kOriginHostTicks = 10'000'000;
    (void)adapter.ObserveHardwareWake(1, Anchor(0x0020'0000, 9'000'000));
    (void)adapter.ObserveHardwareWake(2, Anchor(0x0020'1000, 9'500'000));
    const auto seed = adapter.ObserveHardwareWake(3, Anchor(0x0020'2000, kOriginHostTicks));
    ASSERT_TRUE(seed.playbackAnchorReady);
    EXPECT_EQ(seed.sampleFrame, 0U);

    const auto beforeBoundary = adapter.ObserveHardwareWake(
        4, Anchor(0x0020'3000, kOriginHostTicks + 31'999'999));
    EXPECT_FALSE(beforeBoundary.playbackAnchorReady);

    const auto firstBoundary = adapter.ObserveHardwareWake(
        5, Anchor(0x0020'4000, kOriginHostTicks + 32'000'000));
    ASSERT_TRUE(firstBoundary.playbackAnchorReady);
    EXPECT_EQ(firstBoundary.sampleFrame, kZtsPeriodFrames);
    EXPECT_EQ(firstBoundary.hostTicks, kOriginHostTicks + 32'000'000);

    const auto secondBoundary = adapter.ObserveHardwareWake(
        6, Anchor(0x0020'5000, kOriginHostTicks + 64'000'000));
    ASSERT_TRUE(secondBoundary.playbackAnchorReady);
    EXPECT_EQ(secondBoundary.sampleFrame, kZtsPeriodFrames * 2ULL);
    EXPECT_EQ(secondBoundary.hostTicks, kOriginHostTicks + 64'000'000);
}

TEST(MAudioTxClockAdapterTests,
     IgnoresDuplicateOrInvalidHardwareWakesAndStopsCleanly) {
    TimebaseGuard timebase(1, 1);
    TxClockAdapter adapter;
    ASSERT_TRUE(adapter.Arm(kEpoch, kSampleRateHz, kZtsPeriodFrames));

    EXPECT_FALSE(adapter.ObserveHardwareWake(0, Anchor(0x0030'0000, 1)).playbackAnchorReady);
    EXPECT_FALSE(adapter.ObserveHardwareWake(1, Anchor(0x0030'0000, 0)).playbackAnchorReady);
    EXPECT_FALSE(adapter.ObserveHardwareWake(1, Anchor(0x0030'0000, 1'000)).playbackAnchorReady);
    EXPECT_TRUE(adapter.ObserveHardwareWake(2, Anchor(0x0030'1000, 2'000)).captureReferencePlanted);
    EXPECT_TRUE(adapter.ObserveHardwareWake(3, Anchor(0x0030'2000, 3'000)).playbackAnchorReady);

    const auto duplicate = adapter.ObserveHardwareWake(
        3, Anchor(0x0030'3000, 40'000'000));
    EXPECT_FALSE(duplicate.playbackAnchorReady);

    adapter.Disarm();
    EXPECT_FALSE(adapter.IsArmed());
    EXPECT_TRUE(std::holds_alternative<Stopped>(adapter.State()));
    EXPECT_FALSE(adapter.ObserveHardwareWake(
                     4, Anchor(0x0030'4000, 50'000'000)).playbackAnchorReady);
}

TEST(MAudioTxClockAdapterTests, RefusesAnInvalidAudioClockGeometry) {
    TimebaseGuard timebase(1, 1);
    TxClockAdapter adapter;
    EXPECT_FALSE(adapter.Arm(kEpoch, 0, kZtsPeriodFrames));
    EXPECT_FALSE(adapter.IsArmed());
    EXPECT_TRUE(std::holds_alternative<Failed>(adapter.State()));
    EXPECT_FALSE(adapter.Arm(kEpoch, kSampleRateHz, 0));
}

} // namespace
