// Absolute presentation time reconstructed from a replayed SYT.
//
// These cases were first written as a standalone characterisation of the
// defect, against a retained Apogee Duet anchor read over the MCP control
// plane (documentation/reports/duet-syt-lift-2026-09-08). They are kept as
// tests because the wire encoding is invariant to the bug: every assertion
// here that fails without ComputePresentationLeadTicks passes with
// ComputeReplaySyt alone, so no round-trip test can stand in for them.

#include "Audio/Runtime/HardwareSampleTimeline.hpp"
#include "Audio/Wire/AMDTP/RxSequenceReplay.hpp"

#include <gtest/gtest.h>

namespace {

using namespace ASFW::Audio::Runtime;

constexpr uint32_t kTicksPerCycle = ASFW::Timing::kTicksPerCycle;
constexpr uint32_t kWindow = kSytCycleWindowTicks;   // 16 cycles, 49'152 ticks
constexpr uint32_t kDelay = 12'800;                  // IEC 61883-6 transfer delay

// Forward distance from a receive cycle to the cycle a SYT names, with no
// modulo lift and no transfer-delay threshold. Independent oracle: FFADO's
// receive lift (libffado cycletimer.h:390-440) reconstructs against the
// receive cycle the same way.
[[nodiscard]] constexpr uint32_t DirectLead(uint16_t syt,
                                            uint32_t cycle) noexcept {
    const uint32_t forwardCycles = ((syt >> 12) + 16u - (cycle % 16u)) % 16u;
    return forwardCycles * kTicksPerCycle + (syt & 0x0FFFu);
}

[[nodiscard]] constexpr uint16_t EncodeLead(uint32_t lead) noexcept {
    return static_cast<uint16_t>(((lead / kTicksPerCycle) << 12) |
                                 (lead % kTicksPerCycle));
}

// The retained Duet seed: receive cycle 2719, SYT 0x1159, direct lead 6489.
constexpr uint32_t kSeedCycleTimer = 0x8ca9f000;
constexpr uint16_t kSeedSyt = 0x1159;

TEST(RxSequencePresentationLeadTests, RecoversTheDirectLeadThroughTheLift) {
    const uint32_t cycle =
        ASFW::Timing::decodeCycleTimer(kSeedCycleTimer).cycle;
    const uint32_t direct = DirectLead(kSeedSyt, cycle);
    ASSERT_EQ(direct, 6489u);

    const uint32_t phase =
        ComputeReplaySytOffset(kSeedSyt, kSeedCycleTimer, kDelay);
    // The lift fired: the phase is a whole window above the honest remainder.
    EXPECT_EQ(phase, 42841u);
    EXPECT_EQ(phase + kDelay, direct + kWindow);

    EXPECT_EQ(ComputePresentationLeadTicks(phase, kDelay), direct);
}

TEST(RxSequencePresentationLeadTests, LeavesAnUnliftedPhaseAlone) {
    // A lead at or above the transfer delay takes the other branch, so the
    // reduction must be a no-op there rather than a second correction.
    const uint32_t sourceCycleTimer = ASFW::Timing::encodeCycleTimer(0, 0, 0);
    const uint32_t direct = kDelay + 1000;
    const uint32_t phase = ComputeReplaySytOffset(
        EncodeLead(direct), sourceCycleTimer, kDelay);
    EXPECT_EQ(phase + kDelay, direct);
    EXPECT_EQ(ComputePresentationLeadTicks(phase, kDelay), direct);
}

TEST(RxSequencePresentationLeadTests, IsContinuousAcrossTheDelayThreshold) {
    // One tick of input either side of the branch used to move absolute
    // presentation time by a whole window in the opposite direction.
    const uint32_t sourceCycleTimer = ASFW::Timing::encodeCycleTimer(0, 0, 0);
    const auto lead = [&](uint32_t direct) {
        return ComputePresentationLeadTicks(
            ComputeReplaySytOffset(EncodeLead(direct), sourceCycleTimer,
                                   kDelay),
            kDelay);
    };
    EXPECT_EQ(lead(kDelay - 1), kDelay - 1);
    EXPECT_EQ(lead(kDelay), kDelay);
    EXPECT_EQ(static_cast<int64_t>(lead(kDelay)) - lead(kDelay - 1), 1);
}

TEST(RxSequencePresentationLeadTests, DoesNotDisturbTheWireEncoding) {
    // Why the defect survived: the reduction removes a whole window, and
    // ComputeReplaySyt masks the cycle to four bits, so the wire is identical.
    const uint32_t phase =
        ComputeReplaySytOffset(kSeedSyt, kSeedCycleTimer, kDelay);
    EXPECT_EQ(ComputeReplaySyt(phase, kSeedCycleTimer, kDelay), kSeedSyt);
    EXPECT_EQ(ComputePresentationLeadTicks(phase, kDelay) % kTicksPerCycle,
              (phase + kDelay) % kTicksPerCycle);
}

TEST(RxSequencePresentationLeadTests, ConfiguredDelayNoLongerMovesAbsoluteTime) {
    // The same received packet reconstructed under two transfer delays. The
    // wire SYT is invariant either way; before the reduction the absolute
    // coordinate moved 2 ms, which is how a configuration constant reached
    // the HAL clock.
    for (const uint32_t delay : {6'000u, kDelay, 20'000u}) {
        const uint32_t phase =
            ComputeReplaySytOffset(kSeedSyt, kSeedCycleTimer, delay);
        EXPECT_EQ(ComputeReplaySyt(phase, kSeedCycleTimer, delay), kSeedSyt);
        EXPECT_EQ(ComputePresentationLeadTicks(phase, delay), 6489u)
            << "transfer delay " << delay;
    }
}

TEST(RxSequencePresentationLeadTests, PassesTheNoInfoSentinelThrough) {
    EXPECT_EQ(ComputePresentationLeadTicks(
                  RxSequenceReplayState::kNoInfo, kDelay),
              RxSequenceReplayState::kNoInfo);
}

TEST(RxSequencePresentationLeadTests, KeepsTheTxContentSeedWhereItWas) {
    // The reason RX could not be corrected on its own: PreviewTxRange places
    // the content cursor from the gap between the two presentation
    // coordinates, and the artefact used to cancel inside that difference.
    // Applying the same reduction to both keeps the seed fixed.
    ASFW::Timing::gHostTimebaseInfo = {125, 3};
    const auto fields = ASFW::Timing::decodeCycleTimer(kSeedCycleTimer);
    const uint64_t packetBus = ASFW::Timing::tstampToOffsets(fields);
    const uint32_t phase =
        ComputeReplaySytOffset(kSeedSyt, kSeedCycleTimer, kDelay);
    const uint32_t liftedLead = phase + kDelay;             // old behaviour
    const uint32_t reducedLead =
        ComputePresentationLeadTicks(phase, kDelay);        // new behaviour
    ASSERT_EQ(liftedLead - reducedLead, kWindow);

    constexpr uint64_t kPlanAhead = 100ull * kTicksPerCycle;
    const auto seed = [&](uint32_t rxLead, uint32_t txLead) {
        HardwareSampleTimeline timeline{};
        const auto epoch = timeline.BeginEpoch(
            HardwareTimelineSource::Receive,
            HardwareTimelineDiscontinuity::StartIO, 48'000, 0);
        HardwareZeroTimestamp out{};
        EXPECT_EQ(timeline.Observe({
            .epoch = epoch,
            .source = HardwareTimelineSource::Receive,
            .sampleFrame = 12'288,
            .frameCount = 8,
            .presentationBusTicks = packetBus + rxLead,
            .correlationBusTicks = packetBus,
            .correlationHostTicks = 8'897'650'536'889ull,
        }, &out), HardwareObservationResult::BoundaryReady);
        TxPresentationRange range{};
        EXPECT_TRUE(timeline.PreviewTxRange(
            timeline.Epoch(), packetBus + kPlanAhead + txLead, 8, range));
        return range.firstAudioFrame;
    };

    EXPECT_EQ(seed(reducedLead, reducedLead), seed(liftedLead, liftedLead));
    // And the failure mode the report identified for a one-sided fix.
    EXPECT_EQ(seed(reducedLead, liftedLead), seed(liftedLead, liftedLead) + 96);
}

}  // namespace
