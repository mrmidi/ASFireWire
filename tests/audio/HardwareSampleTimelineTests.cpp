#include "Audio/Runtime/HardwareSampleTimeline.hpp"
#include "Common/TimingUtils.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>

namespace {
using namespace ASFW::Audio::Runtime;

struct TimebaseGuard final {
    mach_timebase_info_data_t old{ASFW::Timing::gHostTimebaseInfo};
    TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = {1, 1}; }
    ~TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = old; }
};

[[nodiscard]] constexpr uint64_t BusDeltaAsHostTicks(uint64_t ticks) {
    return ticks * 1'000'000'000ULL / ASFW::Timing::kTicksPerSecond;
}

TEST(HardwareSampleTimelineTests, ProjectsBoundaryInsidePacketAtEveryV3Rate) {
    TimebaseGuard timebase{};
    for (const auto [rate, nominal] :
         std::array{std::pair{48'000U, 512U}, std::pair{96'000U, 256U},
                    std::pair{192'000U, 128U}}) {
        HardwareSampleTimeline timeline{};
        const uint64_t epoch = timeline.BeginEpoch(
            HardwareTimelineSource::Receive,
            HardwareTimelineDiscontinuity::StartIO, rate, 0);
        ASSERT_NE(epoch, 0U);
        constexpr uint64_t kPresentationBus = 1'000'000;
        constexpr uint64_t kHost = 20'000'000;
        HardwareZeroTimestamp boundary{};
        EXPECT_EQ(timeline.Observe({
                      .epoch = epoch,
                      .source = HardwareTimelineSource::Receive,
                      .sampleFrame = 8'184,
                      .frameCount = 16,
                      .presentationBusTicks = kPresentationBus,
                      .correlationBusTicks = kPresentationBus,
                      .correlationHostTicks = kHost,
                  }, &boundary),
                  HardwareObservationResult::BoundaryReady);
        EXPECT_EQ(boundary.sampleFrame, 8'192U);
        EXPECT_EQ(boundary.hostTicks,
                  kHost + BusDeltaAsHostTicks(8ULL * nominal));
    }
}

TEST(HardwareSampleTimelineTests,
     SuccessiveAnchorsFollowCurrentHardwareObservationNotStartupSlope) {
    TimebaseGuard timebase{};
    HardwareSampleTimeline timeline{};
    const uint64_t epoch = timeline.BeginEpoch(
        HardwareTimelineSource::Receive,
        HardwareTimelineDiscontinuity::StartIO, 48'000, 0);

    HardwareZeroTimestamp first{};
    ASSERT_EQ(timeline.Observe({
                  .epoch = epoch,
                  .source = HardwareTimelineSource::Receive,
                  .sampleFrame = 8'184,
                  .frameCount = 16,
                  .presentationBusTicks = 5'000'000,
                  .correlationBusTicks = 5'000'000,
                  .correlationHostTicks = 50'000'000,
              }, &first), HardwareObservationResult::BoundaryReady);

    // The second observation includes a synthetic media-clock phase drift.
    // Its ZTS follows this fresh hardware correlation; it is not extrapolated
    // from the first anchor at a fixed 48-kHz slope.
    HardwareZeroTimestamp second{};
    ASSERT_EQ(timeline.Observe({
                  .epoch = epoch,
                  .source = HardwareTimelineSource::Receive,
                  .sampleFrame = 16'376,
                  .frameCount = 16,
                  .presentationBusTicks = 9'195'000,
                  .correlationBusTicks = 9'195'000,
                  .correlationHostTicks = 220'123'456,
              }, &second), HardwareObservationResult::BoundaryReady);
    EXPECT_EQ(second.sampleFrame, 16'384U);
    EXPECT_EQ(second.hostTicks,
              220'123'456U + BusDeltaAsHostTicks(8ULL * 512));
    EXPECT_NE(second.hostTicks - first.hostTicks,
              BusDeltaAsHostTicks(8'192ULL * 512));
}

TEST(HardwareSampleTimelineTests, SuppressesDuplicateBoundaryWithinEpoch) {
    TimebaseGuard timebase{};
    HardwareSampleTimeline timeline{};
    const uint64_t epoch = timeline.BeginEpoch(
        HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::StartIO, 96'000, 0);
    const HardwarePresentationObservation observation{
        .epoch = epoch,
        .source = HardwareTimelineSource::Transmit,
        .sampleFrame = 8'190,
        .frameCount = 4,
        .presentationBusTicks = 100'000,
        .correlationBusTicks = 100'000,
        .correlationHostTicks = 1'000'000,
    };
    EXPECT_EQ(timeline.Observe(observation),
              HardwareObservationResult::BoundaryReady);
    EXPECT_EQ(timeline.Observe(observation),
              HardwareObservationResult::DuplicateBoundary);
    EXPECT_EQ(timeline.duplicateBoundaries_.load(), 1U);
}

TEST(HardwareSampleTimelineTests, RejectsGapsAndOverlapsInCommittedTxRanges) {
    HardwareSampleTimeline timeline{};
    const uint64_t epoch = timeline.BeginEpoch(
        HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::StartIO, 192'000, 0);
    TxPresentationRange range{};
    ASSERT_TRUE(timeline.PreviewTxRange(epoch, 10'000, 24, range));
    EXPECT_EQ(range.firstAudioFrame, 0U);
    ASSERT_TRUE(timeline.CommitTxRange(range));
    EXPECT_EQ(timeline.NextTxFrame(), 24U);

    auto overlap = range;
    EXPECT_FALSE(timeline.CommitTxRange(overlap));
    auto gap = range;
    gap.firstAudioFrame = 48;
    EXPECT_FALSE(timeline.CommitTxRange(gap));
    ASSERT_TRUE(timeline.PreviewTxRange(epoch, 20'000, 24, range));
    EXPECT_EQ(range.firstAudioFrame, 24U);
    EXPECT_TRUE(timeline.CommitTxRange(range));
    EXPECT_EQ(timeline.NextTxFrame(), 48U);
}

TEST(HardwareSampleTimelineTests, EpochChangeRejectsOldObservationsAndRanges) {
    HardwareSampleTimeline timeline{};
    const uint64_t oldEpoch = timeline.BeginEpoch(
        HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::StartIO, 48'000, 0);
    const uint64_t newEpoch = timeline.BeginEpoch(
        HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::BusGeneration, 48'000, 8'192);
    EXPECT_GT(newEpoch, oldEpoch);
    EXPECT_EQ(timeline.Observe({
                  .epoch = oldEpoch,
                  .source = HardwareTimelineSource::Transmit,
                  .sampleFrame = 0,
                  .frameCount = 8,
                  .presentationBusTicks = 1,
                  .correlationBusTicks = 1,
                  .correlationHostTicks = 1,
              }), HardwareObservationResult::StaleEpoch);
    TxPresentationRange range{};
    EXPECT_FALSE(timeline.PreviewTxRange(oldEpoch, 100, 8, range));
    ASSERT_TRUE(timeline.PreviewTxRange(newEpoch, 100, 8, range));
    EXPECT_EQ(range.firstAudioFrame, 8'192U);
}

} // namespace
