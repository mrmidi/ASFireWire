#include "Audio/Runtime/HardwareSampleTimeline.hpp"
#include "Audio/Runtime/TxCompletionStampDrain.hpp"
#include "Audio/Wire/AMDTP/AmdtpCadence.hpp"
#include "Common/TimingUtils.hpp"

#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

namespace {
using namespace ASFW::Audio::Runtime;

// The ZTS period is geometry, not a literal. These scenarios straddle a
// boundary, so they are positioned against it -- writing 8192 here is what
// let the driver and the HAL disagree about the grid in the first place.
constexpr uint64_t kPeriod =
    HardwareSampleTimeline::kZeroTimestampPeriodFrames;

struct TimebaseGuard final {
    mach_timebase_info_data_t old{ASFW::Timing::gHostTimebaseInfo};
    TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = {1, 1}; }
    ~TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = old; }
};

[[nodiscard]] constexpr uint64_t BusDeltaAsHostTicks(uint64_t ticks) {
    return ticks * 1'000'000'000ULL / ASFW::Timing::kTicksPerSecond;
}

// V3: each epoch takes the ZTS period of its own rate tier, the same value the
// HAL is given (HalBufferProfileForRate), so a 48 -> 96 kHz change moves the
// anchor grid with the declared period.
TEST(HardwareSampleTimelineTests, EpochTakesTheZtsPeriodOfItsRate) {
    for (const uint32_t rate :
         {32'000U, 44'100U, 48'000U, 88'200U, 96'000U, 176'400U, 192'000U}) {
        HardwareSampleTimeline timeline{};
        ASSERT_NE(timeline.BeginEpoch(HardwareTimelineSource::Receive,
                                      HardwareTimelineDiscontinuity::StartIO, rate, 0),
                  0U)
            << rate;
        EXPECT_EQ(timeline.ZeroTimestampPeriodFrames(),
                  ASFW::IsochTransport::HalBufferProfileForRate(rate).zeroTimestampPeriodFrames)
            << rate;
    }
    EXPECT_EQ(HardwareSampleTimeline::ZeroTimestampPeriodForRate(48'000), 12'288U);
    EXPECT_EQ(HardwareSampleTimeline::ZeroTimestampPeriodForRate(96'000), 24'576U);
}

TEST(HardwareSampleTimelineTests, ProjectsBoundaryInsidePacketAtEveryV3Rate) {
    TimebaseGuard timebase{};
    for (const uint32_t rate : {44'100U, 48'000U, 96'000U, 192'000U}) {
        HardwareSampleTimeline timeline{};
        const uint64_t epoch = timeline.BeginEpoch(
            HardwareTimelineSource::Receive,
            HardwareTimelineDiscontinuity::StartIO, rate, 0);
        ASSERT_NE(epoch, 0U);
        const uint64_t ztsPeriod = timeline.ZeroTimestampPeriodFrames();
        constexpr uint64_t kPresentationBus = 1'000'000;
        constexpr uint64_t kHost = 20'000'000;
        HardwareZeroTimestamp boundary{};
        EXPECT_EQ(timeline.Observe({
                      .epoch = epoch,
                      .source = HardwareTimelineSource::Receive,
                      .sampleFrame = ztsPeriod - 8,
                      .frameCount = 16,
                      .presentationBusTicks = kPresentationBus,
                      .correlationBusTicks = kPresentationBus,
                      .correlationHostTicks = kHost,
                  }, &boundary),
                  HardwareObservationResult::BoundaryReady);
        EXPECT_EQ(boundary.sampleFrame, ztsPeriod);
        const uint64_t deltaTicks =
            HardwareSampleTimeline::AudioFramesToBusTicks(8, rate);
        EXPECT_EQ(boundary.hostTicks,
                  kHost + BusDeltaAsHostTicks(deltaTicks));
    }
}

TEST(HardwareSampleTimelineTests, Projects441TimelineExactRational) {
    HardwareSampleTimeline timeline{};
    const uint64_t epoch = timeline.BeginEpoch(
        HardwareTimelineSource::Receive,
        HardwareTimelineDiscontinuity::StartIO, 44'100U, 0);
    ASSERT_NE(epoch, 0U);

    EXPECT_EQ(HardwareSampleTimeline::AudioFramesToBusTicks(8, 44'100U), 4458ULL);
    EXPECT_EQ(HardwareSampleTimeline::BusTicksToAudioFrames(4458, 44'100U), 7ULL);
    EXPECT_EQ(HardwareSampleTimeline::BusTicksToAudioFrames(4459, 44'100U), 8ULL);

    constexpr uint64_t kObsBus = 1'000'000;
    constexpr uint64_t kObsFrame = 100;
    HardwareZeroTimestamp boundary{};
    EXPECT_EQ(timeline.Observe({
                  .epoch = epoch,
                  .source = HardwareTimelineSource::Receive,
                  .sampleFrame = kObsFrame,
                  .frameCount = 8,
                  .presentationBusTicks = kObsBus,
                  .correlationBusTicks = kObsBus,
                  .correlationHostTicks = 10'000,
              }, &boundary),
              HardwareObservationResult::Accepted);

    uint64_t projected = 0;
    EXPECT_TRUE(timeline.ProjectFirstFrameFromObservation(kObsBus + 4459, projected));
    EXPECT_EQ(projected, kObsFrame + 8);
}

TEST(HardwareSampleTimelineTests, BoundaryInputOverflowDoesNotCrashOrWrap) {
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(HardwareSampleTimeline::BusTicksToAudioFrames(kMax, 192'000U), kMax / 128ULL);
    EXPECT_EQ(HardwareSampleTimeline::AudioFramesToBusTicks(kMax, 44'100U), kMax);

    HardwareSampleTimeline timeline{};
    const uint64_t epoch = timeline.BeginEpoch(
        HardwareTimelineSource::Receive,
        HardwareTimelineDiscontinuity::StartIO, 48'000U, 0);
    ASSERT_NE(epoch, 0U);

    const uint32_t ztsPeriod = timeline.ZeroTimestampPeriodFrames();
    const uint64_t safeNearMaxFrame = kMax - ztsPeriod;

    HardwareZeroTimestamp boundary{};
    EXPECT_EQ(timeline.Observe({
                  .epoch = epoch,
                  .source = HardwareTimelineSource::Receive,
                  .sampleFrame = safeNearMaxFrame,
                  .frameCount = 8,
                  .presentationBusTicks = 1'000,
                  .correlationBusTicks = 1'000,
                  .correlationHostTicks = 10'000,
              }, &boundary),
              HardwareObservationResult::Accepted);

    uint64_t projected = 0;
    EXPECT_FALSE(timeline.ProjectFirstFrameFromObservation(1'000 + 24'576'000ULL, projected));

    EXPECT_EQ(timeline.Observe({
                  .epoch = epoch,
                  .source = HardwareTimelineSource::Receive,
                  .sampleFrame = kMax - 10,
                  .frameCount = 8,
                  .presentationBusTicks = 2'000,
                  .correlationBusTicks = 2'000,
                  .correlationHostTicks = 20'000,
              }, &boundary),
              HardwareObservationResult::Invalid);
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
                  .sampleFrame = kPeriod - 8,
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
                  .sampleFrame = 2 * kPeriod - 8,
                  .frameCount = 16,
                  .presentationBusTicks = 9'195'000,
                  .correlationBusTicks = 9'195'000,
                  .correlationHostTicks = 220'123'456,
              }, &second), HardwareObservationResult::BoundaryReady);
    EXPECT_EQ(second.sampleFrame, 2 * kPeriod);
    EXPECT_EQ(second.hostTicks,
              220'123'456U + BusDeltaAsHostTicks(8ULL * 512));
    EXPECT_NE(second.hostTicks - first.hostTicks,
              BusDeltaAsHostTicks(kPeriod * 512));
}

TEST(HardwareSampleTimelineTests, SuppressesDuplicateBoundaryWithinEpoch) {
    TimebaseGuard timebase{};
    HardwareSampleTimeline timeline{};
    const uint64_t epoch = timeline.BeginEpoch(
        HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::StartIO, 96'000, 0);
    const uint64_t ztsPeriod = timeline.ZeroTimestampPeriodFrames();
    const HardwarePresentationObservation observation{
        .epoch = epoch,
        .source = HardwareTimelineSource::Transmit,
        .sampleFrame = ztsPeriod - 2,
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
        HardwareTimelineDiscontinuity::BusGeneration, 48'000, kPeriod);
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
    EXPECT_EQ(range.firstAudioFrame, kPeriod);
}


// --- Completion-stamp drain ---------------------------------------------------
//
// The timeline publishes a zero-timestamp boundary only when that boundary
// falls inside the packet it was handed. An observer that submits one packet in
// N therefore publishes roughly one boundary in N, and the ones it misses are
// never recovered. These tests pin both halves: the cursor arithmetic that
// decides which stamps are still owed, and the coverage property that makes
// draining them matter.

TEST(TxCompletionStampDrainTests, DrainsEveryStampPushedSinceTheLastPass) {
    const auto first = PlanTxCompletionStampDrain(0, 6, 32);
    EXPECT_EQ(first.first, 0U);
    EXPECT_EQ(first.last, 6U);
    EXPECT_EQ(first.Count(), 6U);
    EXPECT_EQ(first.missed, 0U);

    const auto second = PlanTxCompletionStampDrain(first.last, 12, 32);
    EXPECT_EQ(second.first, 6U);
    EXPECT_EQ(second.Count(), 6U);
    EXPECT_EQ(second.missed, 0U);
}

TEST(TxCompletionStampDrainTests, QuietWakeDrainsNothingAndLosesNothing) {
    const auto drain = PlanTxCompletionStampDrain(9, 9, 32);
    EXPECT_TRUE(drain.Empty());
    EXPECT_EQ(drain.Count(), 0U);
    EXPECT_EQ(drain.missed, 0U);
    EXPECT_FALSE(drain.queueRestarted);
}

TEST(TxCompletionStampDrainTests, StampsOverwrittenBeforeReadingAreReportedNotRead) {
    // 100 pushed, 32 slots: everything below 68 has been overwritten. Reading
    // one of those would feed the clock a plausible wrong number.
    const auto drain = PlanTxCompletionStampDrain(10, 100, 32);
    EXPECT_EQ(drain.first, 68U);
    EXPECT_EQ(drain.last, 100U);
    EXPECT_EQ(drain.missed, 58U);
    EXPECT_FALSE(drain.queueRestarted);
}

TEST(TxCompletionStampDrainTests, ARestartedQueueRewindsTheCursorInsteadOfSkippingTheStream) {
    // The queue re-arms and its count restarts at zero. A cursor left above the
    // new count would read as "already drained" for the next 400 packets.
    const auto drain = PlanTxCompletionStampDrain(400, 3, 32);
    EXPECT_TRUE(drain.queueRestarted);
    EXPECT_EQ(drain.first, 0U);
    EXPECT_EQ(drain.last, 3U);
    EXPECT_EQ(drain.missed, 0U);
}

TEST(TxCompletionStampDrainTests, AnEmptyQueueLeavesNothingToDrain) {
    const auto drain = PlanTxCompletionStampDrain(400, 0, 32);
    EXPECT_TRUE(drain.Empty());
    EXPECT_TRUE(drain.queueRestarted);
}

namespace {

/// Run the production 48 kHz blocking cadence for two seconds, submitting the
/// newest DATA packet of every `wakeGroup` packets, and report which boundaries
/// the timeline published. `wakeGroup == 1` is the drained observer.
[[nodiscard]] std::vector<uint64_t> BoundariesObservedEvery(unsigned wakeGroup) {
    HardwareSampleTimeline timeline;
    const uint64_t epoch = timeline.BeginEpoch(
        HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::StartIO, 48'000, 0);
    ASFW::Protocols::Audio::AMDTP::BlockingCadence cadence;
    std::vector<uint64_t> boundaries;
    uint64_t frame = 0;
    for (uint64_t packet = 0; packet < 16'000; ++packet) {
        const unsigned frames = cadence.CurrentCycleDataFrames();
        if (frames != 0 && (packet + 1) % wakeGroup == 0) {
            HardwareZeroTimestamp boundary{};
            const auto result = timeline.Observe(
                {epoch, HardwareTimelineSource::Transmit, frame, frames,
                 1'000'000 + packet * 3072, 1'000'000 + packet * 3072,
                 1'000'000'000 + packet * 125'000},
                &boundary);
            if (result == HardwareObservationResult::BoundaryReady) {
                boundaries.push_back(boundary.sampleFrame);
            }
        }
        frame += frames;
        cadence.AdvanceCycle();
    }
    return boundaries;
}

} // namespace

TEST(HardwareSampleTimelineTests, ObservingEveryDataPacketPublishesEveryBoundary) {
    TimebaseGuard timebase{};
    const auto drained = BoundariesObservedEvery(1);

    // Two seconds of 48 kHz is 96000 frames, so every period boundary from 0
    // must appear, in order and without gaps. Derived, so the count follows
    // the geometry rather than pinning one era of it.
    ASSERT_EQ(drained.size(), 96'000U / kPeriod + 1U);
    for (size_t i = 0; i < drained.size(); ++i) {
        EXPECT_EQ(drained[i], static_cast<uint64_t>(i) * kPeriod);
    }
}

TEST(HardwareSampleTimelineTests, ObservingOnlyTheNewestPacketOfAWakeLosesBoundaries) {
    TimebaseGuard timebase{};
    const auto drained = BoundariesObservedEvery(1);

    // This is the defect the cursor exists to prevent, kept as an executable
    // statement of it: a boundary lands inside one packet, so an observer that
    // skips packets skips boundaries, and the first anchor arrives hundreds of
    // milliseconds late. One completion group is eight packets.
    for (const unsigned wakeGroup :
         {ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup,
          2U * ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup}) {
        const auto sparse = BoundariesObservedEvery(wakeGroup);
        EXPECT_LT(sparse.size(), drained.size()) << "wakeGroup=" << wakeGroup;
        // Losing EVERY boundary in the window is the same defect, more
        // severe: one packet in 1536 carries a boundary at a 12288-frame
        // period, so a skipping observer can miss the whole two seconds.
        // The strict claim above still holds; only the ordering check needs
        // a survivor to talk about.
        if (!sparse.empty()) {
            EXPECT_GT(sparse.front(), drained.front())
                << "wakeGroup=" << wakeGroup;
        }
    }
}

// Nominal ticks per frame exist only where they are an integer: 32 kHz and
// the 48 kHz family. The 44.1 kHz family has none, and says so with 0 rather
// than a rounded value; its conversions are exact rational instead.
TEST(HardwareSampleTimelineTests, NominalTicksOnlyWhereIntegral) {
    EXPECT_EQ(HardwareSampleTimeline::NominalBusTicksPerFrame(32'000), 768U);
    EXPECT_EQ(HardwareSampleTimeline::NominalBusTicksPerFrame(48'000), 512U);
    EXPECT_EQ(HardwareSampleTimeline::NominalBusTicksPerFrame(96'000), 256U);
    EXPECT_EQ(HardwareSampleTimeline::NominalBusTicksPerFrame(44'100), 0U);
    EXPECT_EQ(HardwareSampleTimeline::NominalBusTicksPerFrame(88'200), 0U);
    // 441 frames at 44.1 kHz are exactly 245,760 ticks (10 ms).
    EXPECT_EQ(HardwareSampleTimeline::AudioFramesToBusTicks(441, 44'100), 245'760U);
    EXPECT_EQ(HardwareSampleTimeline::BusTicksToAudioFrames(245'760, 44'100), 441U);
    EXPECT_FALSE(HardwareSampleTimeline::IsSupportedSampleRate(22'050));
}

} // namespace
