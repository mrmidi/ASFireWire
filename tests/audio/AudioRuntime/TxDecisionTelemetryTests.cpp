// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Decision-lane telemetry: the neutral shared storage that lets a completed
// packet be joined back to the producer and transport decisions taken about it.
//
// These tests exercise the storage contract directly rather than through a live
// stream, because the properties that matter are exactly the ones a live stream
// cannot be made to reproduce on demand: a slot recycled under a reader, a stop
// that arrives after a rearm, a writer interrupted mid-record.

#include "Isoch/Core/IsochTxQueue.hpp"
#include "Audio/Runtime/TxLatencySession.hpp"
#include "Audio/Shared/AudioTimingGeometry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace ASFW::Isoch;
using ASFW::Audio::Shared::AudioTimingGeometry;

namespace {

constexpr uint32_t kSlots = AudioTimingGeometry::kTxSharedSlotPackets;  // 1512

std::unique_ptr<IsochTxQueueControl> MakeControl(uint32_t numSlots = kSlots) {
    auto ctrl = std::make_unique<IsochTxQueueControl>();
    ctrl->abiVersion = kTxQueueAbiVersion;
    ctrl->numSlots = numSlots;
    return ctrl;
}

constexpr uint64_t kToken = MakeTxCaptureToken(/*generation=*/7, /*epoch=*/3);

uint64_t SlotGen(uint64_t packetIndex, uint32_t numSlots = kSlots) {
    return ExpectedTxCommitGeneration(packetIndex, numSlots);
}

}  // namespace

// --- Retention -----------------------------------------------------------
//
// The reason the ring is sized to the queue's own slot count.

TEST(TxDecisionTelemetryTests, RingRetainsOneRecordPerQueueSlot) {
    // A smaller private modulus would recycle a record while the packet it
    // describes was still in flight. Pinning this stops the array being
    // "tidied" back to a round number that silently loses the evidence.
    EXPECT_GE(kTxDecisionRingSlots, kSlots);
    EXPECT_GE(kTxDecisionRingSlots,
              AudioTimingGeometry::kTxPreparationLeadPackets);
}

TEST(TxDecisionTelemetryTests, RecordSurvivesTheFullPreparationLead) {
    // The producer records a decision up to a full preparation lead ahead of
    // the wire. The record must still be joinable when that packet finally
    // completes -- with a 512-slot ring it had been overwritten roughly three
    // times over by then, so every join failed and the lane read as silent.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);

    constexpr uint64_t kMeasured = 4000;
    ctrl->RecordProducerAcquire(kToken, kMeasured, SlotGen(kMeasured),
                                LatePayloadAcquireResult::Success, false);
    ctrl->RecordProducerEncode(kToken, kMeasured, 111);

    // Everything the producer prepares between that packet and the wire
    // catching up with it.
    for (uint64_t p = kMeasured + 1;
         p < kMeasured + AudioTimingGeometry::kTxPreparationLeadPackets; ++p) {
        ctrl->RecordProducerAcquire(kToken, p, SlotGen(p),
                                    LatePayloadAcquireResult::Success, false);
    }

    TxProducerDecisionSnapshot snap{};
    EXPECT_EQ(ctrl->ReadProducerDecision(kMeasured, kToken, SlotGen(kMeasured),
                                         snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.encodeHostTicks, 111U);
}

TEST(TxDecisionTelemetryTests, SlotReuseIsReportedNotSilentlyMisattributed) {
    // Once a later packet legitimately takes the slot, the honest answer is
    // "overwritten", never the later packet's numbers wearing this packet's
    // index.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);

    constexpr uint64_t kOld = 10;
    const uint64_t kNew = kOld + kSlots;  // same slot, next lap
    ctrl->RecordProducerAcquire(kToken, kOld, SlotGen(kOld),
                                LatePayloadAcquireResult::Success, false);
    ctrl->RecordProducerAcquire(kToken, kNew, SlotGen(kNew),
                                LatePayloadAcquireResult::Success, false);

    TxProducerDecisionSnapshot snap{};
    EXPECT_EQ(ctrl->ReadProducerDecision(kOld, kToken, SlotGen(kOld), snap),
              TxDecisionJoinResult::SlotReused);
    EXPECT_EQ(ctrl->ReadProducerDecision(kNew, kToken, SlotGen(kNew), snap),
              TxDecisionJoinResult::Valid);
}

// --- Distinguishing missing evidence from overwritten evidence -----------

TEST(TxDecisionTelemetryTests, NeverWrittenIsNotOverwritten) {
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    TxProducerDecisionSnapshot snap{};
    EXPECT_EQ(ctrl->ReadProducerDecision(42, kToken, SlotGen(42), snap),
              TxDecisionJoinResult::NeverWritten);
}

TEST(TxDecisionTelemetryTests, CaptureStartedLaterIsDistinctFromLostEvidence) {
    // A packet prepared before the capture armed has no record and never could
    // have had one. Reporting that as overwritten would condemn a perfectly
    // good run.
    auto ctrl = MakeControl();
    const uint64_t oldToken = MakeTxCaptureToken(6, 1);
    ctrl->BeginCapture(6, 1);
    ctrl->RecordProducerAcquire(oldToken, 100, SlotGen(100),
                                LatePayloadAcquireResult::Success, false);

    ctrl->BeginCapture(7, 3);
    TxProducerDecisionSnapshot snap{};
    EXPECT_EQ(ctrl->ReadProducerDecision(100, kToken, SlotGen(100), snap),
              TxDecisionJoinResult::CaptureStartedLater);
}

// --- Producer lane -------------------------------------------------------

TEST(TxDecisionTelemetryTests, AcquireRejectedAsFinalizedIsRecorded) {
    // Scenario: PCM was published, but by the time the producer went to place
    // it the packet's payload choice was already final.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    ctrl->RecordProducerAcquire(kToken, 5, SlotGen(5),
                                LatePayloadAcquireResult::RejectedFinalized,
                                false);

    TxProducerDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadProducerDecision(5, kToken, SlotGen(5), snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.acquireResult,
              static_cast<uint8_t>(LatePayloadAcquireResult::RejectedFinalized));
    EXPECT_EQ(snap.flags & kTxProducerFlagAcquireRecorded,
              kTxProducerFlagAcquireRecorded);
    EXPECT_EQ(snap.flags & kTxProducerFlagOfferRecorded, 0U);
}

TEST(TxDecisionTelemetryTests, StaleSlotIsObservedWithoutBecomingARejection) {
    // The observation is recorded; it must not turn into a new refusal, or the
    // telemetry patch changes the behaviour it exists to explain.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    ctrl->RecordProducerAcquire(kToken, 5, SlotGen(5),
                                LatePayloadAcquireResult::Success,
                                /*staleSlotSeen=*/true);

    TxProducerDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadProducerDecision(5, kToken, SlotGen(5), snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.acquireResult,
              static_cast<uint8_t>(LatePayloadAcquireResult::Success));
    EXPECT_EQ(snap.flags & kTxProducerFlagStaleSlotSeen,
              kTxProducerFlagStaleSlotSeen);
}

TEST(TxDecisionTelemetryTests, OfferBracketAndObservedPhaseAreRecorded) {
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    ctrl->RecordProducerAcquire(kToken, 9, SlotGen(9),
                                LatePayloadAcquireResult::Success, false);
    ctrl->RecordProducerEncode(kToken, 9, 500);
    ctrl->RecordProducerOffer(kToken, 9, SlotGen(9), /*startTicks=*/600,
                              /*endTicks=*/650, /*won=*/false,
                              static_cast<uint8_t>(
                                  TxPayloadArbitration::kFinalOnArmedImage));

    TxProducerDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadProducerDecision(9, kToken, SlotGen(9), snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.encodeHostTicks, 500U);
    EXPECT_EQ(snap.offerStartHostTicks, 600U);
    EXPECT_EQ(snap.offerEndHostTicks, 650U);
    EXPECT_EQ(snap.offerResult, 2U);  // lost
    EXPECT_EQ(snap.observedArbitrationPhase,
              static_cast<uint8_t>(TxPayloadArbitration::kFinalOnArmedImage));
    // Acquire and encode survive the offer write rather than being reset.
    EXPECT_EQ(snap.acquireResult,
              static_cast<uint8_t>(LatePayloadAcquireResult::Success));
}

// --- Arbitration: both orders are truthfully recorded --------------------

TEST(TxDecisionTelemetryTests, OfferWinsThenSealDiscardsAlternative) {
    IsochTxPacketMeta meta{};
    meta.packetIndex = 4;
    const uint64_t gen = 1;
    meta.payloadArbitration.store(
        MakeTxPayloadArbitration(gen, TxPayloadArbitration::kNoAlternative));

    uint64_t observed = 0;
    EXPECT_TRUE(OfferLateTxPayload(meta, gen, observed));

    uint64_t previous = 0;
    EXPECT_EQ(FinalizeTxPayloadOnArmedImage(meta, gen, previous),
              SealResult::SealedDiscardingAlternative);
}

TEST(TxDecisionTelemetryTests, SealWinsThenOfferIsRefused) {
    IsochTxPacketMeta meta{};
    meta.packetIndex = 4;
    const uint64_t gen = 1;
    meta.payloadArbitration.store(
        MakeTxPayloadArbitration(gen, TxPayloadArbitration::kNoAlternative));

    uint64_t previous = 0;
    EXPECT_EQ(FinalizeTxPayloadOnArmedImage(meta, gen, previous),
              SealResult::SealedWithoutAlternative);

    uint64_t observed = 0;
    EXPECT_FALSE(OfferLateTxPayload(meta, gen, observed));
    // The producer is told exactly what it lost to, not merely that it lost.
    EXPECT_EQ(observed & ((1ULL << kTxPayloadArbitrationPhaseBits) - 1),
              static_cast<uint64_t>(TxPayloadArbitration::kFinalOnArmedImage));
}

TEST(TxDecisionTelemetryTests, SealOfAlreadyTerminalPacketIsNotDoubleCounted) {
    IsochTxPacketMeta meta{};
    meta.packetIndex = 4;
    const uint64_t gen = 1;
    meta.payloadArbitration.store(
        MakeTxPayloadArbitration(gen, TxPayloadArbitration::kNoAlternative));

    uint64_t previous = 0;
    EXPECT_EQ(FinalizeTxPayloadOnArmedImage(meta, gen, previous),
              SealResult::SealedWithoutAlternative);
    EXPECT_EQ(FinalizeTxPayloadOnArmedImage(meta, gen, previous),
              SealResult::AlreadyTerminal);
}

TEST(TxDecisionTelemetryTests, SealAgainstWrongGenerationIsReportedAsMismatch) {
    IsochTxPacketMeta meta{};
    meta.packetIndex = 4;
    meta.payloadArbitration.store(
        MakeTxPayloadArbitration(2, TxPayloadArbitration::kNoAlternative));

    uint64_t previous = 0;
    EXPECT_EQ(FinalizeTxPayloadOnArmedImage(meta, /*generation=*/1, previous),
              SealResult::GenerationMismatch);
}

// --- Transport lane: examination history ---------------------------------

TEST(TxDecisionTelemetryTests, RepeatedExaminationsPreserveTerminalEvidence) {
    // The heart of the missed-service measurement. A packet is looked at on
    // every pass that can reach it; a single mutable "last result" reports
    // where transport ended up and destroys the evidence of the passes that
    // walked past a ready image.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 77;
    const uint64_t gen = SlotGen(kPkt);

    // Two passes before the producer offered anything.
    ctrl->RecordTransportExamination(kToken, kPkt, gen, /*passId=*/1,
                                     /*hostTicks=*/1000, 0,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, /*offerVisible=*/false,
                                     /*terminal=*/false);
    ctrl->RecordTransportExamination(kToken, kPkt, gen, /*passId=*/2,
                                     /*hostTicks=*/2000, 0,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, /*offerVisible=*/false,
                                     /*terminal=*/false);
    // The offer becomes visible; two passes see it.
    ctrl->RecordTransportExamination(kToken, kPkt, gen, /*passId=*/3,
                                     /*hostTicks=*/3000, 1,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, /*offerVisible=*/true,
                                     /*terminal=*/false);
    ctrl->RecordTransportExamination(kToken, kPkt, gen, /*passId=*/4,
                                     /*hostTicks=*/4000, 1,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, /*offerVisible=*/true,
                                     /*terminal=*/false);
    // Terminal decision on pass 5.
    ctrl->RecordTransportExamination(kToken, kPkt, gen, /*passId=*/5,
                                     /*hostTicks=*/5000, 1,
                                     LatePayloadBindResult::Bound, 900, true,
                                     /*offerVisible=*/true, /*terminal=*/true);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);

    // Last look before the offer: the most recent one, not the first.
    EXPECT_EQ(snap.lastBeforeOffer.passId, 2U);
    EXPECT_EQ(snap.lastBeforeOffer.hostTicks, 2000U);
    // First look after the offer: the first one, not the last.
    EXPECT_EQ(snap.firstAfterOffer.passId, 3U);
    EXPECT_EQ(snap.firstAfterOffer.hostTicks, 3000U);
    // Terminal decision, kept separately from both.
    EXPECT_EQ(snap.terminal.passId, 5U);
    EXPECT_EQ(snap.terminal.hostTicks, 5000U);
    EXPECT_EQ(snap.terminal.bindResult,
              static_cast<uint8_t>(LatePayloadBindResult::Bound));
    EXPECT_EQ(snap.examinationCount, 5U);
}

TEST(TxDecisionTelemetryTests, FirstTerminalDecisionIsNotOverwritten) {
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 78;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 1, 1000, 1,
                                     LatePayloadBindResult::RejectedInsideGuard,
                                     500, true, true, /*terminal=*/true);
    ctrl->RecordTransportExamination(kToken, kPkt, gen, 2, 2000, 1,
                                     LatePayloadBindResult::Bound, 600, true,
                                     true, /*terminal=*/true);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.terminal.passId, 1U);
    EXPECT_EQ(snap.terminal.bindResult,
              static_cast<uint8_t>(LatePayloadBindResult::RejectedInsideGuard));
}

TEST(TxDecisionTelemetryTests, SealWithoutAlternativeIsDistinguishableFromUnset) {
    // SealResult::SealedWithoutAlternative is 0, so a zeroed field cannot be
    // used as "nothing recorded". An explicit recorded bit carries that.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 79;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 1, 1000, 0,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, false, /*terminal=*/true);

    TxTransportDecisionSnapshot before{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, before),
              TxDecisionJoinResult::Valid);

    ctrl->RecordTransportSeal(kToken, kPkt, SealResult::SealedWithoutAlternative,
                              SealReason::FinalityFrontier, 1100, 1150, false);

    TxTransportDecisionSnapshot after{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, after),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(after.sealResult,
              static_cast<uint8_t>(SealResult::SealedWithoutAlternative));
    EXPECT_EQ(after.sealReason,
              static_cast<uint8_t>(SealReason::FinalityFrontier));
    EXPECT_EQ(after.sealStartHostTicks, 1100U);

    // A second seal of the same packet does not replace the first.
    ctrl->RecordTransportSeal(kToken, kPkt,
                              SealResult::SealedDiscardingAlternative,
                              SealReason::LiveGuardRejection, 9000, 9100, false);
    TxTransportDecisionSnapshot again{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, again),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(again.sealStartHostTicks, 1100U);
}

TEST(TxDecisionTelemetryTests, DescriptorUpdateIsRecordedAfterTheClaim) {
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 80;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 1, 1000, 1,
                                     LatePayloadBindResult::Bound, 500, true,
                                     true, /*terminal=*/true);
    ctrl->RecordTransportClaim(kToken, kPkt, /*claimStart=*/1100,
                               /*claimEnd=*/1150, /*descUpdate=*/1200,
                               /*descriptorWritten=*/true);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.claimStartHostTicks, 1100U);
    EXPECT_EQ(snap.claimEndHostTicks, 1150U);
    EXPECT_EQ(snap.descriptorUpdateHostTicks, 1200U);
    EXPECT_GT(snap.descriptorUpdateHostTicks, snap.claimEndHostTicks);
    EXPECT_EQ(snap.flags & kTxTransportFlagDescriptorWritten,
              kTxTransportFlagDescriptorWritten);
}

TEST(TxDecisionTelemetryTests, SnapshotPositionIsFlaggedRatherThanPresentedAsRead) {
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 81;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 1, 1000, 0,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, false, /*terminal=*/true);
    ctrl->RecordTransportSeal(kToken, kPkt, SealResult::SealedWithoutAlternative,
                              SealReason::FinalityFrontier, 1100, 1150,
                              /*positionIsSnapshot=*/true);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.flags & kTxTransportFlagTerminalPosIsSnapshot,
              kTxTransportFlagTerminalPosIsSnapshot);
}

// --- Capture identity and lifecycle --------------------------------------

TEST(TxDecisionTelemetryTests, CaptureTokenIsOneCoherentWord) {
    auto ctrl = MakeControl();
    ctrl->BeginCapture(11, 22);
    const uint64_t token = ctrl->ActiveCaptureToken();
    EXPECT_EQ(TxCaptureTokenGeneration(token), 11U);
    EXPECT_EQ(TxCaptureTokenEpoch(token), 22U);
}

TEST(TxDecisionTelemetryTests, StaleStopDoesNotSilenceARearmedCapture) {
    // The race the review named: a stop queued for a finished session arriving
    // after the next one armed. An unconditional clear silenced the new
    // capture, and the run recorded nothing at all -- indistinguishable from a
    // driver that never offered any content.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(/*generation=*/5, 1);
    ctrl->BeginCapture(/*generation=*/6, 2);  // rearm

    EXPECT_FALSE(ctrl->EndCapture(/*generation=*/5));  // the late stop
    EXPECT_NE(ctrl->ActiveCaptureToken(), 0U);
    EXPECT_EQ(TxCaptureTokenGeneration(ctrl->ActiveCaptureToken()), 6U);

    EXPECT_TRUE(ctrl->EndCapture(/*generation=*/6));
    EXPECT_EQ(ctrl->ActiveCaptureToken(), 0U);
}

TEST(TxDecisionTelemetryTests, WritersAreNoOpsWhenNoCaptureIsActive) {
    auto ctrl = MakeControl();
    // No BeginCapture. A zero token must record nothing at all, which is what
    // lets the callers gate their clock reads on the same value.
    ctrl->RecordProducerAcquire(ctrl->ActiveCaptureToken(), 3, SlotGen(3),
                                LatePayloadAcquireResult::Success, false);
    ctrl->RecordTransportExamination(ctrl->ActiveCaptureToken(), 3, SlotGen(3),
                                     1, 1000, 0,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, false, true);

    TxProducerDecisionSnapshot prod{};
    EXPECT_EQ(ctrl->ReadProducerDecision(3, kToken, SlotGen(3), prod),
              TxDecisionJoinResult::NeverWritten);
    TxTransportDecisionSnapshot trans{};
    EXPECT_EQ(ctrl->ReadTransportDecision(3, kToken, SlotGen(3), trans),
              TxDecisionJoinResult::NeverWritten);
}

TEST(TxDecisionTelemetryTests, TransportArmDoesNotDisarmAnActiveCapture) {
    // ResetConsumerForArm runs on the transport arm path, which cannot know
    // whether a capture is in progress. It used to clear the capture fields,
    // so a capture armed before the stream started recorded nothing.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    ctrl->ResetConsumerForArm();
    EXPECT_EQ(ctrl->ActiveCaptureToken(), kToken);
}

TEST(TxDecisionTelemetryTests, RecordsFromAPriorCaptureAreNotJoinedToThisOne) {
    auto ctrl = MakeControl();
    const uint64_t first = MakeTxCaptureToken(1, 1);
    ctrl->BeginCapture(1, 1);
    ctrl->RecordProducerAcquire(first, 50, SlotGen(50),
                                LatePayloadAcquireResult::Success, false);

    const uint64_t second = MakeTxCaptureToken(2, 9);
    ctrl->BeginCapture(2, 9);
    TxProducerDecisionSnapshot snap{};
    const auto join = ctrl->ReadProducerDecision(50, second, SlotGen(50), snap);
    EXPECT_NE(join, TxDecisionJoinResult::Valid);
    EXPECT_EQ(join, TxDecisionJoinResult::CaptureStartedLater);
}

// --- Concurrency ---------------------------------------------------------

TEST(TxDecisionTelemetryTests, ConcurrentWriterAndReaderNeverTearARecord) {
    // Under TSan this is the test that would catch a seqlock over plain
    // fields. Without it, it still catches a reader assembling halves of two
    // different writes.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 123;
    const uint64_t gen = SlotGen(kPkt);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> torn{0};
    std::atomic<uint64_t> valid{0};

    std::thread writer([&] {
        for (uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
            // Every field of a given write carries the same magnitude, so any
            // blend of two writes is detectable.
            ctrl->RecordProducerAcquire(kToken, kPkt, gen,
                                        LatePayloadAcquireResult::Success,
                                        false);
            ctrl->RecordProducerEncode(kToken, kPkt, i);
            ctrl->RecordProducerOffer(kToken, kPkt, gen, i, i, true, 1);
        }
    });

    for (int i = 0; i < 200000; ++i) {
        TxProducerDecisionSnapshot snap{};
        const auto join =
            ctrl->ReadProducerDecision(kPkt, kToken, gen, snap);
        if (join != TxDecisionJoinResult::Valid) continue;
        valid.fetch_add(1, std::memory_order_relaxed);
        if ((snap.flags & kTxProducerFlagOfferRecorded) != 0 &&
            snap.offerStartHostTicks != snap.offerEndHostTicks) {
            torn.fetch_add(1, std::memory_order_relaxed);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    EXPECT_EQ(torn.load(), 0U);
    EXPECT_GT(valid.load(), 0U);
}

TEST(TxDecisionTelemetryTests, InFlightWriterDuringCaptureEndIsNotJoined) {
    // A writer that began under a token must finish under it; the reader then
    // rejects the record because the capture it belongs to has ended. Nothing
    // is torn, and nothing from the ended capture leaks into the next one.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 55;
    const uint64_t gen = SlotGen(kPkt);

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            // Deliberately keeps using the token it captured at entry, exactly
            // as the real writers do.
            ctrl->RecordProducerAcquire(kToken, kPkt, gen,
                                        LatePayloadAcquireResult::Success,
                                        false);
        }
    });

    ctrl->EndCapture(7);
    ctrl->BeginCapture(8, 4);
    const uint64_t newToken = MakeTxCaptureToken(8, 4);

    for (int i = 0; i < 20000; ++i) {
        TxProducerDecisionSnapshot snap{};
        const auto join =
            ctrl->ReadProducerDecision(kPkt, newToken, gen, snap);
        // The in-flight writer's records belong to the old capture and must
        // never be accepted as this one's evidence.
        EXPECT_NE(join, TxDecisionJoinResult::Valid);
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
}

// --- Regressions: three ways the join produced confident nonsense ---------

TEST(TxDecisionTelemetryTests, SuccessfulBindIsNotReportedAsASeal) {
    // A successful bind writes a terminal event and never seals. Inferring
    // "sealed" from the terminal event marked every bound packet as sealed,
    // and the offer-to-seal interval was then computed against zeroed seal
    // timestamps -- so the packets that worked reported large negative
    // durations.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 90;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 1, 1000, 1,
                                     LatePayloadBindResult::Bound, 500, true,
                                     true, /*terminal=*/true);
    ctrl->RecordTransportClaim(kToken, kPkt, 1100, 1150, 1200, true);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);

    EXPECT_NE(snap.flags & kTxTransportFlagTerminalRecorded, 0U);
    EXPECT_NE(snap.terminal.present, 0);
    // ...and yet no seal.
    EXPECT_EQ(snap.sealRecorded, 0);
    EXPECT_FALSE(IsochTxQueueControl::TransportSealRecorded(snap));
    EXPECT_EQ(snap.sealStartHostTicks, 0U);
}

TEST(TxDecisionTelemetryTests, SealedPacketStillReportsASeal) {
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 91;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 1, 1000, 1,
                                     LatePayloadBindResult::RejectedInsideGuard,
                                     500, true, true, /*terminal=*/true);
    ctrl->RecordTransportSeal(kToken, kPkt,
                              SealResult::SealedDiscardingAlternative,
                              SealReason::LiveGuardRejection, 1100, 1150, false);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.sealRecorded, 1);
    EXPECT_TRUE(IsochTxQueueControl::TransportSealRecorded(snap));
    EXPECT_EQ(snap.sealResult,
              static_cast<uint8_t>(SealResult::SealedDiscardingAlternative));
    EXPECT_EQ(snap.sealStartHostTicks, 1100U);
}

TEST(TxDecisionTelemetryTests, SealedWithoutAlternativeStillSetsTheRecordedBit) {
    // The value is 0; only the recorded bit separates it from "never sealed".
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 92;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 1, 1000, 0,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, false, /*terminal=*/true);
    ctrl->RecordTransportSeal(kToken, kPkt, SealResult::SealedWithoutAlternative,
                              SealReason::FinalityFrontier, 1100, 1150, false);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.sealResult, 0);
    EXPECT_EQ(snap.sealRecorded, 1);
    EXPECT_TRUE(IsochTxQueueControl::TransportSealRecorded(snap));
}

TEST(TxDecisionTelemetryTests, UnavailableHardwarePositionSurvivesAsLastAttempt) {
    // The look that spots the offer fills firstAfterOffer with NotExamined and
    // pins its timestamp. The hardware read then fails: not terminal (the
    // packet stays open by design), and firstAfterOffer must keep its original
    // timestamp -- so without a separate home the reason service failed was
    // dropped entirely, and the packet was indistinguishable from one transport
    // never got to.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 93;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, /*passId=*/4,
                                     /*hostTicks=*/4000, 1,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, /*offerVisible=*/true,
                                     /*terminal=*/false);
    ctrl->RecordTransportExamination(kToken, kPkt, gen, /*passId=*/4,
                                     /*hostTicks=*/4100, 1,
                                     LatePayloadBindResult::RejectedUnavailableHwPos,
                                     0, false, /*offerVisible=*/true,
                                     /*terminal=*/false);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);

    // First service keeps the instant it actually happened.
    EXPECT_EQ(snap.firstAfterOffer.hostTicks, 4000U);
    EXPECT_EQ(snap.firstAfterOffer.bindResult,
              static_cast<uint8_t>(LatePayloadBindResult::NotExamined));
    // The packet is still open: no terminal decision, no seal.
    EXPECT_EQ(snap.flags & kTxTransportFlagTerminalRecorded, 0U);
    EXPECT_FALSE(IsochTxQueueControl::TransportSealRecorded(snap));
    // ...but why it failed is still on the record.
    EXPECT_EQ(snap.lastAttemptBindResult,
              static_cast<uint8_t>(LatePayloadBindResult::RejectedUnavailableHwPos));
    EXPECT_EQ(snap.lastAttemptPassId, 4U);
}

TEST(TxDecisionTelemetryTests, NoServiceIsDistinctFromFailedService) {
    // Same packet shape, but transport never reached it at all.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 94;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 4, 4000, 1,
                                     LatePayloadBindResult::NotExamined, 0,
                                     false, /*offerVisible=*/true,
                                     /*terminal=*/false);

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.lastAttemptBindResult,
              static_cast<uint8_t>(LatePayloadBindResult::NotExamined));
    EXPECT_EQ(snap.lastAttemptPassId, 0U);
}

TEST(TxDecisionTelemetryTests, SweepDoesNotClobberAConcludedAttempt) {
    // The no-offer sweep runs every pass with NotExamined. It must not erase a
    // verdict already reached for this packet.
    auto ctrl = MakeControl();
    ctrl->BeginCapture(7, 3);
    constexpr uint64_t kPkt = 95;
    const uint64_t gen = SlotGen(kPkt);

    ctrl->RecordTransportExamination(kToken, kPkt, gen, 4, 4000, 1,
                                     LatePayloadBindResult::RejectedUnavailableHwPos,
                                     0, false, true, /*terminal=*/false);
    for (uint64_t pass = 5; pass < 10; ++pass) {
        ctrl->RecordTransportExamination(kToken, kPkt, gen, pass, pass * 1000, 0,
                                         LatePayloadBindResult::NotExamined, 0,
                                         false, /*offerVisible=*/false,
                                         /*terminal=*/false);
    }

    TxTransportDecisionSnapshot snap{};
    ASSERT_EQ(ctrl->ReadTransportDecision(kPkt, kToken, gen, snap),
              TxDecisionJoinResult::Valid);
    EXPECT_EQ(snap.lastAttemptBindResult,
              static_cast<uint8_t>(LatePayloadBindResult::RejectedUnavailableHwPos));
    EXPECT_EQ(snap.lastAttemptPassId, 4U);
}
