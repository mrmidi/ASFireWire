// SPDX-License-Identifier: Apache-2.0

#include "ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioPresentationObserver.hpp"
#include "ASFWDriver/Common/TimingUtils.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {
using namespace ASFW::Audio::Families::BeBoB::MAudio;
using ASFW::Audio::Runtime::HardwareTimelineSource;

constexpr StartEpoch kStartEpoch{17};
constexpr uint64_t kTimelineEpoch = 9;
constexpr uint32_t kRate = 48'000;
constexpr uint32_t kPresentationOffset = 0x2e00;

struct TimebaseGuard final {
    mach_timebase_info_data_t old{ASFW::Timing::gHostTimebaseInfo};
    TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = {1, 1}; }
    ~TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = old; }
};

[[nodiscard]] PresentationObservationResult Observe(
    PresentationObserver& observer,
    uint64_t generation,
    uint64_t completionBusTicks,
    uint64_t correlationBusTicks,
    uint64_t hostTicks,
    uint64_t sampleFrame,
    uint32_t frameCount) {
    return observer.ObserveHardwareWake(
        generation, completionBusTicks, correlationBusTicks,
        {.cycleTime = static_cast<uint32_t>(correlationBusTicks),
         .hostTicks = hostTicks},
        sampleFrame, frameCount);
}

TEST(MAudioPresentationObserverTests,
     QualifiesThirdGroupAndPreservesSuppliedAbsoluteFrame) {
    TimebaseGuard timebase{};
    PresentationObserver observer{};
    ASSERT_TRUE(observer.Arm(kStartEpoch, kTimelineEpoch, kRate,
                             kPresentationOffset));

    const auto first = Observe(observer, 1, 10'000, 9'500, 1'000'000, 700, 8);
    EXPECT_FALSE(first.captureReferencePlanted);
    EXPECT_FALSE(first.observationReady);

    const auto second = Observe(observer, 2, 20'000, 19'500, 2'000'000, 708, 8);
    EXPECT_TRUE(second.captureReferencePlanted);
    EXPECT_FALSE(second.observationReady);

    const auto third = Observe(observer, 3, 30'000, 29'500, 3'000'000, 716, 8);
    ASSERT_TRUE(third.observationReady);
    EXPECT_EQ(third.groupCount, 3U);
    EXPECT_EQ(third.observation.epoch, kTimelineEpoch);
    EXPECT_EQ(third.observation.source, HardwareTimelineSource::Transmit);
    EXPECT_EQ(third.observation.sampleFrame, 716U);
    EXPECT_EQ(third.observation.frameCount, 8U);
    EXPECT_EQ(third.observation.presentationBusTicks,
              30'000U + kPresentationOffset);
    EXPECT_EQ(third.observation.correlationBusTicks, 29'500U);
    EXPECT_EQ(third.observation.correlationHostTicks, 3'000'000U);
}

TEST(MAudioPresentationObserverTests,
     NeverProjectsFramesFromNominalElapsedHostTime) {
    TimebaseGuard timebase{};
    PresentationObserver observer{};
    ASSERT_TRUE(observer.Arm(kStartEpoch, kTimelineEpoch, kRate,
                             kPresentationOffset));
    (void)Observe(observer, 1, 10'000, 9'000, 100, 1'000, 8);
    (void)Observe(observer, 2, 20'000, 19'000, 200, 1'008, 8);
    ASSERT_TRUE(Observe(observer, 3, 30'000, 29'000, 300, 1'016, 8)
                    .observationReady);

    const auto later = Observe(observer, 4, 40'000, 39'000,
                               9'000'000'000ULL, 55'000, 8);
    ASSERT_TRUE(later.observationReady);
    EXPECT_EQ(later.observation.sampleFrame, 55'000U);
    EXPECT_EQ(later.observation.frameCount, 8U);
}

TEST(MAudioPresentationObserverTests,
     RejectsInvalidGeometryDuplicatesAndDisarmedCallbacks) {
    TimebaseGuard timebase{};
    PresentationObserver observer{};
    EXPECT_FALSE(observer.Arm(kStartEpoch, kTimelineEpoch, 44'100,
                              kPresentationOffset));
    EXPECT_FALSE(observer.Arm(kStartEpoch, 0, kRate, kPresentationOffset));
    ASSERT_TRUE(observer.Arm(kStartEpoch, kTimelineEpoch, kRate,
                             kPresentationOffset));
    EXPECT_FALSE(Observe(observer, 0, 1, 1, 1, 0, 8).observationReady);
    (void)Observe(observer, 1, 10, 9, 10, 0, 8);
    EXPECT_FALSE(Observe(observer, 1, 11, 10, 11, 8, 8).observationReady);
    observer.Disarm();
    EXPECT_TRUE(std::holds_alternative<Stopped>(observer.State()));
    EXPECT_FALSE(Observe(observer, 2, 20, 19, 20, 8, 8).observationReady);
}

} // namespace
