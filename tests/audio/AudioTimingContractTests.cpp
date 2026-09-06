#include "AudioTimingContract.hpp"
#include "Isoch/Transmit/IsochTxLayout.hpp"
#include "Isoch/Transmit/TxPacketIndexLift.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <vector>

namespace {
using namespace ASFW::Testing::AudioTiming;
using namespace ASFW::Audio::Runtime;

constexpr EpochAgreement Epoch(uint64_t value) { return {value, value, value}; }
constexpr uint32_t kRing = 48;
constexpr uint64_t kBusOrigin = 10'000'000;

// The lap arithmetic below is only meaningful against the real IT ring. Without
// this the suite would keep testing 48 after the geometry moved.
static_assert(kRing == ASFW::Isoch::Tx::Layout::kNumPackets,
              "fixture ring size must track the OHCI IT descriptor ring");

template <typename T>
void ExpectRejected(const Decision<T>& result, Rejection expected) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), expected);
}

// An independent physical event generator. The executed packet count only
// advances on execution, while bus cycles also advance through self-linked
// skips. The frame count comes from a fixed DATA/DATA/DATA/NO-DATA script,
// not from a production cursor, preview, completion count, or index lift.
struct PhysicalTrace {
    uint64_t cycles{0};
    uint64_t executed{0};
    uint64_t frames{0};
    uint32_t dataFrames{8};
    uint64_t nominalTicks{512};

    void Cycle(bool skip = false) {
        ++cycles;
        if (skip) return;
        if (executed % 4 != 3) frames += dataFrames;
        ++executed;
    }

    // Fresh local evidence for each range, including a synthetic wandering
    // clock phase. No use of HardwareSampleTimeline's nominal-rate helper.
    PresentationEvidence NextPresentation(uint64_t epoch) const {
        const uint64_t drift = frames / 1'000;
        const uint64_t first = kBusOrigin + frames * nominalTicks + drift;
        return {epoch, frames, dataFrames, first - 2, first + 2};
    }
};

TEST(AudioTimingProgressContract, MatchesEnumeratedCandidatesIncludingBothEndpoints) {
    // Independent oracle: enumerate every integer in the admissible interval.
    // This checks odd rings, singleton intervals, ties, and intervals > a lap.
    for (uint32_t ring : {1U, 3U, 7U, 48U}) {
        for (uint64_t previous : {0ULL, 5ULL, 95ULL}) {
            for (uint64_t low = 0; low <= ring + 1; ++low) {
                for (uint64_t width = 0; width <= 2 * ring; ++width) {
                    for (uint32_t slot = 0; slot < ring; ++slot) {
                        std::vector<uint64_t> candidates;
                        for (uint64_t advance = low; advance <= low + width; ++advance) {
                            if ((previous + advance) % ring == slot) {
                                candidates.push_back(previous + advance);
                            }
                        }
                        const auto result = ResolveProgress(previous, slot, ring,
                                                           AdvanceBounds{low, low + width});
                        if (candidates.size() == 1) {
                            ASSERT_TRUE(result.has_value());
                            ASSERT_EQ(*result, candidates.front());
                        } else {
                            ASSERT_FALSE(result.has_value());
                            ASSERT_EQ(result.error(), candidates.empty()
                                ? Rejection::InconsistentProgress : Rejection::AmbiguousProgress);
                        }
                    }
                }
            }
        }
    }
}

TEST(AudioTimingProgressContract, AFullLapWithTheSameSlotIsAmbiguous) {
    ExpectRejected(ResolveProgress(5, 5, kRing, AdvanceBounds{0, 48}),
                   Rejection::AmbiguousProgress);
    // Stronger independent execution evidence can make the SAME pointer useful.
    const auto exact = ResolveProgress(5, 5, kRing, AdvanceBounds{48, 48});
    ASSERT_TRUE(exact);
    EXPECT_EQ(*exact, 53U);
    ExpectRejected(ResolveProgress(5, 5, kRing, std::nullopt),
                   Rejection::MissingEvidence);
}

TEST(AudioTimingProgressContract, SkippedCyclesDoNotInventExecutedLaps) {
    PhysicalTrace trace;
    for (unsigned cycle = 0; cycle < 101; ++cycle) trace.Cycle(cycle < 48);
    ASSERT_EQ(trace.cycles, 101U);
    ASSERT_EQ(trace.executed, 53U);
    const auto slot = static_cast<uint32_t>(trace.executed % kRing);
    // Demonstrates why the existing nearest-lap helper is not authoritative.
    EXPECT_EQ(ASFW::Isoch::Tx::LiftRingSlotToAbsolute(slot, trace.cycles, kRing), 101U);
    ExpectRejected(ResolveProgress(0, slot, kRing, AdvanceBounds{0, trace.cycles}),
                   Rejection::AmbiguousProgress);
    const auto result = ResolveProgress(0, slot, kRing, AdvanceBounds{53, 53});
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, trace.executed);
}

TEST(AudioTimingProgressContract, RejectsBadGeometryAndOverflowWithoutWrapping) {
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    ExpectRejected(ResolveProgress(0, 0, 0, AdvanceBounds{0, 0}), Rejection::InvalidEvidence);
    ExpectRejected(ResolveProgress(0, 48, 48, AdvanceBounds{0, 48}), Rejection::InvalidEvidence);
    ExpectRejected(ResolveProgress(0, 0, 48, AdvanceBounds{2, 1}), Rejection::InvalidEvidence);
    ExpectRejected(ResolveProgress(max, 0, 48, AdvanceBounds{0, 1}), Rejection::InvalidEvidence);
    const auto result = ResolveProgress(max - 1, static_cast<uint32_t>(max % 48),
                                        48, AdvanceBounds{1, 1});
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, max);
}

class AudioTimingContractTest : public testing::Test {
protected:
    Contract contract;
    TxPresentationRange range{1, 0, 8, kBusOrigin};
    PresentationEvidence evidence{1, 0, 8, kBusOrigin - 2, kBusOrigin + 2};

    void SetUp() override {
        ASSERT_TRUE(contract.BeginEpoch(Epoch(1), 0, 0));
        ASSERT_TRUE(contract.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 0}));
    }
};

TEST_F(AudioTimingContractTest, PresentationWindowIsInclusiveAndCannotSilentlyWiden) {
    for (const int offset : {-3, -2, 0, 2, 3}) {
        Contract local;
        ASSERT_TRUE(local.BeginEpoch(Epoch(1), 0, 0));
        ASSERT_TRUE(local.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 0}));
        auto candidate = range;
        candidate.presentationBusTicks = kBusOrigin + offset;
        const auto decision = local.Admit(Epoch(1), candidate, evidence);
        if (offset == -3 || offset == 3) {
            ExpectRejected(decision, Rejection::PresentationMismatch);
            EXPECT_EQ(local.NextFrame(), 0U);
        } else {
            ASSERT_TRUE(decision);
            EXPECT_EQ(local.NextFrame(), 8U);
        }
    }
}

TEST_F(AudioTimingContractTest, MissingEvidenceRejectsAndBlocksSubsequentSalvage) {
    ExpectRejected(contract.Admit(Epoch(1), range, std::nullopt), Rejection::MissingEvidence);
    EXPECT_EQ(contract.NextFrame(), 0U);
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::RecoveryRequired);
}

TEST_F(AudioTimingContractTest, MissingProgressEvidenceCannotReachAdmission) {
    Contract local;
    ASSERT_TRUE(local.BeginEpoch(Epoch(1), 0, 0));
    ExpectRejected(local.Admit(Epoch(1), range, evidence), Rejection::MissingEvidence);
}

TEST_F(AudioTimingContractTest, NoEpochAndInconsistentProgressCannotReachAdmission) {
    Contract local;
    ExpectRejected(local.Admit(Epoch(1), range, evidence), Rejection::NoEpoch);
    ExpectRejected(local.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 0}),
                   Rejection::NoEpoch);
    ExpectRejected(contract.ObserveProgress(Epoch(1), 2, kRing, AdvanceBounds{0, 1}),
                   Rejection::InconsistentProgress);
    EXPECT_EQ(contract.PacketIndex(), 0U);
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::RecoveryRequired);
}

TEST_F(AudioTimingContractTest, RingGeometryCannotChangeWithinAnEpoch) {
    ExpectRejected(contract.ObserveProgress(Epoch(1), 0, 24, AdvanceBounds{0, 0}),
                   Rejection::InvalidEvidence);
    ASSERT_TRUE(contract.BeginEpoch(Epoch(2), 0, 0));
    ASSERT_TRUE(contract.ObserveProgress(Epoch(2), 0, 24, AdvanceBounds{0, 0}));
}

TEST_F(AudioTimingContractTest, PresentationFailureAlsoBlocksLaterApparentlyGoodRanges) {
    auto slipped = range;
    slipped.presentationBusTicks += 48 * 3072;
    ExpectRejected(contract.Admit(Epoch(1), slipped, evidence), Rejection::PresentationMismatch);
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::RecoveryRequired);
}

TEST_F(AudioTimingContractTest, AmbiguityCannotBeSalvagedByALaterPlausibleObservation) {
    ExpectRejected(contract.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 96}),
                   Rejection::AmbiguousProgress);
    EXPECT_EQ(contract.PacketIndex(), 0U);
    ExpectRejected(contract.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{96, 96}),
                   Rejection::RecoveryRequired);
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::RecoveryRequired);
    ExpectRejected(contract.BeginEpoch(Epoch(1), 0, 0), Rejection::EpochMismatch);
    ASSERT_TRUE(contract.BeginEpoch(Epoch(2), 8'192, 0));
    ASSERT_TRUE(contract.ObserveProgress(Epoch(2), 0, kRing, AdvanceBounds{0, 0}));
    ASSERT_TRUE(contract.Admit(Epoch(2), {2, 8'192, 8, kBusOrigin},
                              PresentationEvidence{2, 8'192, 8, kBusOrigin, kBusOrigin}));
}

TEST_F(AudioTimingContractTest, GapOverlapAndWrongContentCountNeverAdvanceTheCursor) {
    ASSERT_TRUE(contract.Admit(Epoch(1), range, evidence));
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::FrameDiscontinuity);
    EXPECT_EQ(contract.NextFrame(), 8U);
    Contract gap;
    ASSERT_TRUE(gap.BeginEpoch(Epoch(1), 0, 0));
    ASSERT_TRUE(gap.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 0}));
    auto candidate = range;
    candidate.firstAudioFrame = 8;
    ExpectRejected(gap.Admit(Epoch(1), candidate, evidence), Rejection::FrameDiscontinuity);
    EXPECT_EQ(gap.NextFrame(), 0U);
    Contract count;
    ASSERT_TRUE(count.BeginEpoch(Epoch(1), 0, 0));
    ASSERT_TRUE(count.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 0}));
    candidate = range;
    candidate.frameCount = 16;
    ExpectRejected(count.Admit(Epoch(1), candidate, evidence), Rejection::ContentMismatch);
    EXPECT_EQ(count.NextFrame(), 0U);
}

TEST_F(AudioTimingContractTest, MixedEpochsFailAtTransitionAndAtAdmission) {
    for (const auto mixed : {EpochAgreement{2, 1, 1}, EpochAgreement{2, 2, 1},
                              EpochAgreement{1, 2, 2}, EpochAgreement{0, 0, 0}}) {
        ExpectRejected(contract.BeginEpoch(mixed, 0, 0), Rejection::EpochMismatch);
    }
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::RecoveryRequired);
    ASSERT_TRUE(contract.BeginEpoch(Epoch(2), 0, 0));
    ASSERT_TRUE(contract.ObserveProgress(Epoch(2), 0, kRing, AdvanceBounds{0, 0}));
    ExpectRejected(contract.Admit({2, 2, 1}, range, evidence), Rejection::EpochMismatch);
}

TEST_F(AudioTimingContractTest, OldRecordsAreRejectedWithoutPoisoningACoherentNewEpoch) {
    ASSERT_TRUE(contract.BeginEpoch(Epoch(2), 0, 0));
    ExpectRejected(contract.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 0}),
                   Rejection::StaleEpoch);
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::StaleEpoch);
    ASSERT_TRUE(contract.ObserveProgress(Epoch(2), 0, kRing, AdvanceBounds{0, 0}));
    range.epoch = evidence.epoch = 2;
    ASSERT_TRUE(contract.Admit(Epoch(2), range, evidence));
}

TEST_F(AudioTimingContractTest, StaleSnapshotCannotWaveThroughACurrentRecord) {
    // Mirror of CurrentSnapshotCannotBlessOldRangeOrOldEvidence. Only an
    // *entirely* old record may be dismissed as merely late; a stale snapshot
    // carrying a current-epoch record is the same broken handoff and must
    // demand recovery rather than leave the contract usable.
    ASSERT_TRUE(contract.BeginEpoch(Epoch(2), 0, 0));
    ASSERT_TRUE(contract.ObserveProgress(Epoch(2), 0, kRing, AdvanceBounds{0, 0}));
    range.epoch = evidence.epoch = 2;
    ExpectRejected(contract.Admit(Epoch(1), range, evidence), Rejection::EpochMismatch);
    ExpectRejected(contract.Admit(Epoch(2), range, evidence), Rejection::RecoveryRequired);
    EXPECT_EQ(contract.NextFrame(), 0U);

    // A record disagreeing with itself is a broken handoff under any snapshot.
    Contract split;
    ASSERT_TRUE(split.BeginEpoch(Epoch(2), 0, 0));
    ASSERT_TRUE(split.ObserveProgress(Epoch(2), 0, kRing, AdvanceBounds{0, 0}));
    auto stale = evidence;
    stale.epoch = 1;
    ExpectRejected(split.Admit(Epoch(1), range, stale), Rejection::EpochMismatch);
    ExpectRejected(split.Admit(Epoch(2), range, evidence), Rejection::RecoveryRequired);

    // The entirely-old record is still harmless.
    Contract old;
    ASSERT_TRUE(old.BeginEpoch(Epoch(2), 0, 0));
    ASSERT_TRUE(old.ObserveProgress(Epoch(2), 0, kRing, AdvanceBounds{0, 0}));
    auto oldRange = range;
    oldRange.epoch = stale.epoch = 1;
    ExpectRejected(old.Admit(Epoch(1), oldRange, stale), Rejection::StaleEpoch);
    ASSERT_TRUE(old.Admit(Epoch(2), range, evidence));
}

TEST_F(AudioTimingContractTest, CurrentSnapshotCannotBlessOldRangeOrOldEvidence) {
    ASSERT_TRUE(contract.BeginEpoch(Epoch(2), 0, 0));
    ASSERT_TRUE(contract.ObserveProgress(Epoch(2), 0, kRing, AdvanceBounds{0, 0}));
    ExpectRejected(contract.Admit(Epoch(2), range, evidence), Rejection::EpochMismatch);
    ASSERT_TRUE(contract.BeginEpoch(Epoch(3), 0, 0));
    ASSERT_TRUE(contract.ObserveProgress(Epoch(3), 0, kRing, AdvanceBounds{0, 0}));
    range.epoch = 3;
    ExpectRejected(contract.Admit(Epoch(3), range, evidence), Rejection::EpochMismatch);
}

TEST_F(AudioTimingContractTest, InvalidRangesAndEvidenceCannotWrapIntoValidity) {
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    for (const auto start : {0ULL, max - 3}) {
        for (unsigned fault = 0; fault < 4; ++fault) {
            Contract local;
            ASSERT_TRUE(local.BeginEpoch(Epoch(1), start, 0));
            ASSERT_TRUE(local.ObserveProgress(Epoch(1), 0, kRing, AdvanceBounds{0, 0}));
            auto candidate = range;
            auto expected = evidence;
            candidate.firstAudioFrame = expected.firstAudioFrame = start;
            if (fault == 0) candidate.frameCount = 0;
            if (fault == 1) expected.frameCount = 0;
            if (fault == 2) expected.earliestBusTicks = expected.latestBusTicks + 1;
            if (fault == 3 && start == 0) candidate.firstAudioFrame = max - 3;
            ExpectRejected(local.Admit(Epoch(1), candidate, expected), Rejection::InvalidEvidence);
            EXPECT_EQ(local.NextFrame(), start);
        }
    }
}

TEST(AudioTimingContractIntegration, RealTimelineAcceptsNormalRangesWithFreshDriftingEvidence) {
    for (const uint32_t rate : {48'000U, 96'000U, 192'000U}) {
        HardwareSampleTimeline timeline;
        const auto epoch = timeline.BeginEpoch(HardwareTimelineSource::Transmit,
            HardwareTimelineDiscontinuity::StartIO, rate, 0);
        Contract contract;
        ASSERT_TRUE(contract.BeginEpoch(Epoch(epoch), 0, 0));
        PhysicalTrace physical;
        physical.dataFrames = 8 * (rate / 48'000);
        physical.nominalTicks = 24'576'000 / rate;
        // Consume every execution event here; delayed observations are tested
        // separately. These are transport events, not client callback sizes.
        for (unsigned cycle = 0; cycle < 2'000; ++cycle) {
            ASSERT_TRUE(contract.ObserveProgress(Epoch(epoch),
                static_cast<uint32_t>(physical.executed % kRing), kRing,
                AdvanceBounds{cycle == 0 ? 0U : 1U, cycle == 0 ? 0U : 1U}));
            if (physical.executed % 4 != 3) {
                const auto expected = physical.NextPresentation(epoch);
                TxPresentationRange range;
                ASSERT_TRUE(timeline.PreviewTxRange(epoch, expected.earliestBusTicks + 2,
                                                   physical.dataFrames, range));
                ASSERT_TRUE(contract.Admit(Epoch(epoch), range, expected));
                ASSERT_TRUE(timeline.CommitTxRange(range));
            }
            physical.Cycle();
        }
        EXPECT_EQ(contract.NextFrame(), physical.frames);
    }
}

TEST(AudioTimingContractIntegration, DelayedProgressRequiresEvidenceThatDistinguishesLaps) {
    for (const uint64_t delay : {6U, 47U, 48U, 96U, 624U}) {
        PhysicalTrace trace;
        for (uint64_t cycle = 0; cycle < delay; ++cycle) trace.Cycle();
        Contract contract;
        ASSERT_TRUE(contract.BeginEpoch(Epoch(1), 0, 0));
        const auto slot = static_cast<uint32_t>(trace.executed % kRing);
        const auto weak = contract.ObserveProgress(Epoch(1), slot, kRing,
                                                   AdvanceBounds{0, trace.cycles});
        if (delay < kRing) {
            ASSERT_TRUE(weak);
            EXPECT_EQ(*weak, trace.executed);
        } else {
            ExpectRejected(weak, Rejection::AmbiguousProgress);
        }
        // A separate run starts with retained, unambiguous execution evidence.
        Contract retained;
        ASSERT_TRUE(retained.BeginEpoch(Epoch(1), 0, 0));
        const auto recovered = retained.ObserveProgress(Epoch(1), slot, kRing,
            AdvanceBounds{trace.executed, trace.executed});
        ASSERT_TRUE(recovered);
        EXPECT_EQ(*recovered, trace.executed);
    }
}

TEST(AudioTimingContractIntegration, DetectsRealPostSeedPresentationSlipOfEitherSign) {
    // Audit of the current production class: monotonic content allocation is
    // insufficient. A passing test means the contract DETECTS the violation,
    // not that HardwareSampleTimeline has been repaired.
    for (const int64_t laps : {-13, -1, 1, 13}) {
        HardwareSampleTimeline timeline;
        const auto epoch = timeline.BeginEpoch(HardwareTimelineSource::Transmit,
            HardwareTimelineDiscontinuity::StartIO, 48'000, 0);
        Contract contract;
        ASSERT_TRUE(contract.BeginEpoch(Epoch(epoch), 0, 0));
        ASSERT_TRUE(contract.ObserveProgress(Epoch(epoch), 0, kRing, AdvanceBounds{0, 0}));
        PhysicalTrace physical;
        auto expected = physical.NextPresentation(epoch);
        TxPresentationRange range;
        ASSERT_TRUE(timeline.PreviewTxRange(epoch, expected.earliestBusTicks + 2, 8, range));
        ASSERT_TRUE(contract.Admit(Epoch(epoch), range, expected));
        ASSERT_TRUE(timeline.CommitTxRange(range));
        physical.Cycle();
        expected = physical.NextPresentation(epoch);
        const auto displacedBus = static_cast<uint64_t>(
            static_cast<int64_t>(expected.earliestBusTicks + 2) + laps * 48 * 3072);
        ASSERT_TRUE(timeline.PreviewTxRange(epoch, displacedBus, 8, range));
        EXPECT_EQ(range.firstAudioFrame, physical.frames);
        ExpectRejected(contract.Admit(Epoch(epoch), range, expected), Rejection::PresentationMismatch);
        EXPECT_EQ(contract.NextFrame(), 8U);
    }
}

TEST(AudioTimingContractIntegration, IndependentContentIdentityCatchesABadReceiveDerivedSeed) {
    for (const uint64_t laps : {1U, 13U}) {
        HardwareSampleTimeline timeline;
        const auto epoch = timeline.BeginEpoch(HardwareTimelineSource::Receive,
            HardwareTimelineDiscontinuity::StartIO, 48'000, 0);
        ASSERT_EQ(timeline.Observe({epoch, HardwareTimelineSource::Receive,
            1'000, 8, 1'000'000, 1'000'000, 100'000'000}), HardwareObservationResult::Accepted);
        constexpr uint64_t intendedBus = 1'102'400;
        TxPresentationRange range;
        ASSERT_TRUE(timeline.PreviewTxRange(epoch, intendedBus + laps * 48 * 3072, 8, range));
        EXPECT_EQ(range.firstAudioFrame, 1'200 + laps * 288);
        // Even an equally shifted internal cursor cannot establish identity.
        // The expected content is fixed before reading the candidate's labels.
        Contract contract;
        ASSERT_TRUE(contract.BeginEpoch(Epoch(epoch), range.firstAudioFrame, 0));
        ASSERT_TRUE(contract.ObserveProgress(Epoch(epoch), 0, kRing, AdvanceBounds{0, 0}));
        ExpectRejected(contract.Admit(Epoch(epoch), range,
            PresentationEvidence{epoch, 1'200, 8, intendedBus, intendedBus}),
            Rejection::ContentMismatch);
    }
}

} // namespace
