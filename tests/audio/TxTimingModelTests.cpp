#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Wire/AMDTP/RxSytCadence.hpp"
#include "ASFWDriver/Audio/Wire/AMDTP/TxTimingModel.hpp"
#include "ASFWDriver/Audio/Wire/AMDTP/TxAnchorTracker.hpp"

namespace ASFW::Testing {
namespace {

using Driver::RxSytCadence;
using Driver::TxTimingModel;
using Driver::TxAnchorTracker;
using LeadHealth = TxTimingModel::LeadHealth;

constexpr int64_t kEightSecondTicks =
    8LL * static_cast<int64_t>(Timing::kTicksPerSecond);

uint16_t SytFromTicks(int64_t ticks) {
    ticks %= Timing::kSytFieldDomainTicks;
    if (ticks < 0) {
        ticks += Timing::kSytFieldDomainTicks;
    }
    const uint32_t cycle =
        static_cast<uint32_t>(ticks / Timing::kTicksPerCycle);
    const uint32_t offset =
        static_cast<uint32_t>(ticks % Timing::kTicksPerCycle);
    return static_cast<uint16_t>((cycle << 12) | offset);
}

uint32_t CycleTimerFromTicks(int64_t ticks) {
    ticks %= 128LL * static_cast<int64_t>(Timing::kTicksPerSecond);
    if (ticks < 0) {
        ticks += 128LL * static_cast<int64_t>(Timing::kTicksPerSecond);
    }
    const uint32_t seconds =
        static_cast<uint32_t>(ticks / Timing::kTicksPerSecond);
    const int64_t secondTicks = ticks % Timing::kTicksPerSecond;
    const uint32_t cycle =
        static_cast<uint32_t>(secondTicks / Timing::kTicksPerCycle);
    const uint32_t offset =
        static_cast<uint32_t>(secondTicks % Timing::kTicksPerCycle);
    return (seconds << Timing::kCycleTimerSecondsShift) |
           (cycle << Timing::kCycleTimerCyclesShift) |
           offset;
}

int64_t FillCadenceToLock(RxSytCadence& cadence,
                          int64_t firstPresentationTicks = 40'000) {
    int64_t phase = firstPresentationTicks;
    for (uint32_t i = 0; i < RxSytCadence::kWarmupUpdates; ++i) {
        const int64_t packetCycle = phase - Timing::kTicksPerCycle;
        EXPECT_TRUE(cadence.Observe(
            SytFromTicks(phase), CycleTimerFromTicks(packetCycle)));
        phase += Timing::kSytPacketStepTicks48k;
    }
    return Timing::normalizeOffsetDomain(
        phase - Timing::kSytPacketStepTicks48k);
}

} // namespace

TEST(RxSytCadenceTests, RequiresBaselinePlusFullHistoryBeforeLock) {
    RxSytCadence cadence{};
    cadence.Reset();

    int64_t phase = 40'000;
    for (uint32_t i = 0; i < RxSytCadence::kEntryCount; ++i) {
        EXPECT_TRUE(cadence.Observe(
            SytFromTicks(phase),
            CycleTimerFromTicks(phase - Timing::kTicksPerCycle)));
        phase += Timing::kSytPacketStepTicks48k;
    }

    RxSytCadence::Snapshot before{};
    ASSERT_TRUE(cadence.TrySnapshot(before));
    EXPECT_FALSE(before.established);
    EXPECT_EQ(before.validUpdates, RxSytCadence::kEntryCount);

    EXPECT_TRUE(cadence.Observe(
        SytFromTicks(phase),
        CycleTimerFromTicks(phase - Timing::kTicksPerCycle)));

    RxSytCadence::Snapshot after{};
    ASSERT_TRUE(cadence.TrySnapshot(after));
    EXPECT_TRUE(after.established);
    EXPECT_EQ(after.validUpdates, RxSytCadence::kWarmupUpdates);
}

TEST(RxSytCadenceTests, StoresNominalThenObservedSytDeltas) {
    RxSytCadence cadence{};
    cadence.Reset();

    EXPECT_TRUE(cadence.Observe(0xD8B0, CycleTimerFromTicks(30'000)));
    EXPECT_TRUE(cadence.Observe(0xF0B0, CycleTimerFromTicks(34'000)));

    EXPECT_EQ(cadence.ReadEntry(256), 4096);
    EXPECT_EQ(cadence.ReadEntry(257), 4096);
}

TEST(TxTimingModelTests, EmitsNoInfoBeforeRxCadenceLock) {
    RxSytCadence cadence{};
    cadence.Reset();
    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    const auto decision = model.PeekNextDataSyt(100'000, cadence);
    EXPECT_EQ(decision.syt, 0xFFFF);
    EXPECT_EQ(decision.health, LeadHealth::kNotSeeded);
    EXPECT_FALSE(model.IsSeeded());
}

TEST(TxTimingModelTests, FirstLockedPacketMatchesRecoveredRxPhase) {
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    const int64_t packetAnchor = recovered;
    const auto decision = model.PeekNextDataSyt(packetAnchor, cadence);

    EXPECT_TRUE(decision.seededThisCall);
    // With the Saffire-faithful fix, the seed is one cycle (3072 ticks) ahead
    // of the anchor, and the forced adjust preserves that lead. The lead is
    // now ~initialLeadTicks, not 0.
    EXPECT_EQ(decision.leadTicks, TxTimingModel::Config{}.initialLeadTicks);
    // The fill phase is one cycle ahead of the recovered RX phase; the wire
    // SYT is that phase pushed out by the IEC 61883-6 transfer delay.
    EXPECT_EQ(decision.syt,
              SytFromTicks(recovered + TxTimingModel::Config{}.initialLeadTicks +
                           TxTimingModel::Config{}.xmitTransferDelayTicks));
}

TEST(TxTimingModelTests, GoldCaptureKeepsTxOnRecoveredRxSyt) {
    RxSytCadence cadence{};
    cadence.Reset();

    constexpr int64_t kGoldRecoveredPhase =
        1001LL * Timing::kTicksPerCycle + 0x02B0;
    const int64_t firstPhase =
        kGoldRecoveredPhase -
        static_cast<int64_t>(RxSytCadence::kWarmupUpdates - 1) *
            Timing::kSytPacketStepTicks48k;
    const int64_t recovered = FillCadenceToLock(cadence, firstPhase);
    ASSERT_EQ(SytFromTicks(recovered), 0x92B0);

    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    const auto decision = model.PeekNextDataSyt(recovered, cadence);
    // With the Saffire-faithful fix, the seed is one cycle (3072) ahead of
    // the recovered phase. Wire SYT = recovered + 3072 + 12800 transfer delay.
    EXPECT_EQ(decision.syt,
              SytFromTicks(recovered +
                           TxTimingModel::Config{}.initialLeadTicks +
                           TxTimingModel::Config{}.xmitTransferDelayTicks));
    EXPECT_EQ(decision.leadTicks, TxTimingModel::Config{}.initialLeadTicks);
}

TEST(TxTimingModelTests, SkippedDataSlotReacquiresFromExecutionAnchor) {
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    const TxTimingModel::Config config{};
    TxTimingModel model{};
    model.Configure(config);

    const auto first = model.PeekNextDataSyt(recovered, cadence);
    ASSERT_NE(first.syt, 0xFFFF);
    model.CommitDataPacket();
    const int64_t stalePhase = model.OutputPhaseTicks();

    model.RearmAfterSkippedDataSlot();
    EXPECT_FALSE(model.IsSeeded());

    const int64_t nextAnchor =
        Timing::normalizeOffsetDomain(recovered + 20 * Timing::kTicksPerCycle);
    const auto reacquired = model.PeekNextDataSyt(nextAnchor, cadence);

    ASSERT_TRUE(reacquired.seededThisCall);
    ASSERT_NE(reacquired.syt, 0xFFFF);
    EXPECT_NE(model.OutputPhaseTicks(), stalePhase);
    EXPECT_EQ(reacquired.syt,
              SytFromTicks(model.OutputPhaseTicks() +
                           config.xmitTransferDelayTicks));
    // With the Saffire-faithful fix, the reacquired lead is ~initialLeadTicks.
    EXPECT_GE(reacquired.leadTicks, 0);
    EXPECT_LT(reacquired.leadTicks, config.acceptLeadTicks);
}

TEST(TxTimingModelTests, AdvancesByDelayedObservedCadenceEntry) {
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});
    const int64_t anchor = recovered;

    const auto first = model.PeekNextDataSyt(anchor, cadence);
    ASSERT_NE(first.syt, 0xFFFF);
    model.CommitDataPacket();

    // Compare in the wire domain: both sides carry the same constant
    // transfer-delay addend, so the per-packet advance is the cadence step.
    const int64_t delay = TxTimingModel::Config{}.xmitTransferDelayTicks;
    EXPECT_EQ(Timing::SYTDiffInOffsets(
                  SytFromTicks(model.OutputPhaseTicks() + delay), first.syt),
              Timing::kSytPacketStepTicks48k);
}

TEST(TxTimingModelTests, RejectsLeadAtSaffireGateAndRearms) {
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    ASSERT_NE(model.PeekNextDataSyt(recovered, cadence).syt, 0xFFFF);
    model.CommitDataPacket();
    ASSERT_NE(model.PeekNextDataSyt(recovered, cadence).syt, 0xFFFF);
    model.CommitDataPacket();

    const auto rejected = model.PeekNextDataSyt(recovered, cadence);
    EXPECT_EQ(rejected.syt, 0xFFFF);
    EXPECT_EQ(rejected.health, LeadHealth::kGate);
    EXPECT_FALSE(model.IsSeeded());
}

TEST(TxTimingModelTests, CadenceResetForcesFreshAcquisition) {
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});
    ASSERT_NE(model.PeekNextDataSyt(
                  recovered,
                  cadence)
                  .syt,
              0xFFFF);

    cadence.Reset();
    const auto afterReset = model.PeekNextDataSyt(0, cadence);
    EXPECT_EQ(afterReset.syt, 0xFFFF);
    EXPECT_EQ(afterReset.health, LeadHealth::kNotSeeded);
}

TEST(TxTimingModelTests, PhaseCorrectionRemovesHalfSampleLatticeError) {
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    const int64_t anchor =
        Timing::normalizeOffsetDomain(recovered + 256);
    const auto decision = model.PeekNextDataSyt(anchor, cadence);

    ASSERT_NE(decision.syt, 0xFFFF);
    // With the Saffire-faithful fix, the phase is one cycle (3072) ahead of
    // the recovered RX lattice, not exactly one sample (512) ahead.
    const int64_t phaseVsRecovered = Timing::extOffsetDiff(
        model.OutputPhaseTicks(), recovered);
    EXPECT_GE(phaseVsRecovered, TxTimingModel::Config{}.initialLeadTicks);
    EXPECT_LE(phaseVsRecovered,
              TxTimingModel::Config{}.initialLeadTicks + Timing::kTicksPerSample48k);
    EXPECT_LT(model.OutputPhaseTicks(), kEightSecondTicks);
}

TEST(TxTimingModelTests, SeedLeadIsIndependentOfRxStartupHistory) {
    // Regression for the duplex-epoch bug (DICE bring-up, 2026-06-11): a seed
    // derived from RX-local frame counters imported the startup-dependent
    // RX/TX cursor origin skew, freezing a different ch0-vs-ch1 SYT offset on
    // the wire each run (48/15/24 frames across captures; modeled in
    // tools/syt_duplex_epoch_sim.py). The absolute lead must derive from the
    // packet execution anchor only: sessions that joined the device stream at
    // wildly different points, with different post-lock histories, must seed
    // the exact same anchor-relative phase.
    struct Session final {
        int64_t firstPhase;
        uint32_t extraObserves;
    };
    constexpr int64_t kStep = Timing::kSytPacketStepTicks48k;
    constexpr Session kSessions[] = {
        {40'000, 0},
        {40'000 + 3'000 * kStep, 137},      // joined ~3000 packets later
        {40'000 + 30'017 * kStep + 7, 41},  // off-lattice device origin
    };
    constexpr int64_t kAnchorAfterRecovered = 2 * Timing::kTicksPerCycle + 333;

    bool haveExpected = false;
    int64_t expectedLead = 0;
    int64_t expectedPhaseVsRecovered = 0;
    for (const auto& session : kSessions) {
        RxSytCadence cadence{};
        cadence.Reset();
        int64_t recovered = FillCadenceToLock(cadence, session.firstPhase);
        for (uint32_t i = 0; i < session.extraObserves; ++i) {
            recovered = Timing::normalizeOffsetDomain(recovered + kStep);
            EXPECT_TRUE(cadence.Observe(
                SytFromTicks(recovered),
                CycleTimerFromTicks(recovered - Timing::kTicksPerCycle)));
        }

        TxTimingModel model{};
        model.Configure(TxTimingModel::Config{});
        const int64_t anchor =
            Timing::normalizeOffsetDomain(recovered + kAnchorAfterRecovered);
        const auto decision = model.PeekNextDataSyt(anchor, cadence);
        ASSERT_NE(decision.syt, 0xFFFF);

        const int64_t phaseVsRecovered =
            Timing::extOffsetDiff(model.OutputPhaseTicks(), recovered);
        if (!haveExpected) {
            haveExpected = true;
            expectedLead = decision.leadTicks;
            expectedPhaseVsRecovered = phaseVsRecovered;
        } else {
            EXPECT_EQ(decision.leadTicks, expectedLead);
            EXPECT_EQ(phaseVsRecovered, expectedPhaseVsRecovered);
        }
    }
}

TEST(TxTimingModelTests, WireSytCarriesTransferDelayWhileGateMeasuresRawPhase) {
    // IEC 61883-6: SYT = event time + TRANSFER_DELAY (blocking @48 k =
    // 12,800 ticks, Linux amdtp-stream.c parity — see
    // TRANSFER_DELAY_AND_OTHER.md §2). The delay is an encode-time addend
    // only: the lead/health gate keeps measuring the raw fill phase against
    // the execution anchor, so the Saffire governor band is unchanged.
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel::Config config{};
    ASSERT_EQ(config.xmitTransferDelayTicks, 12800);

    TxTimingModel model{};
    model.Configure(config);

    const auto decision = model.PeekNextDataSyt(recovered, cadence);
    ASSERT_NE(decision.syt, 0xFFFF);

    // Gate quantity: raw phase vs anchor — no transfer delay folded in.
    EXPECT_EQ(decision.leadTicks,
              Timing::extOffsetDiff(model.OutputPhaseTicks(), recovered));
    // Wire quantity: presentation = phase + transfer delay, i.e. the SYT
    // leads the raw phase by exactly the configured delay.
    EXPECT_EQ(decision.syt,
              SytFromTicks(model.OutputPhaseTicks() +
                           config.xmitTransferDelayTicks));
    EXPECT_EQ(Timing::SYTDiffInOffsets(
                  decision.syt, SytFromTicks(model.OutputPhaseTicks())),
              config.xmitTransferDelayTicks);

    // Rate ladder: the power-of-two families collapse to the same 4096-tick
    // accumulation term (static_asserted in the header); the 44.1 k family
    // and 32 k differ.
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 44100), 13162);
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 32000), 14848);
}

TEST(TxTimingModelTests, ReportsWireLeadWithoutUsingItAsAResetPolicy) {
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel model{};
    const TxTimingModel::Config config{};
    model.Configure(config);

    ASSERT_NE(model.PeekNextDataSyt(recovered, cadence).syt, 0xFFFF);
    model.CommitDataPacket();

    const int64_t phase = model.OutputPhaseTicks();
    const int64_t lateRawLead = -(config.xmitTransferDelayTicks + 1024);
    const int64_t anchor =
        Timing::normalizeOffsetDomain(phase - lateRawLead);
    const auto decision = model.PeekNextDataSyt(anchor, cadence);

    EXPECT_EQ(decision.health, LeadHealth::kLate);
    EXPECT_EQ(decision.leadTicks, lateRawLead);
    EXPECT_EQ(decision.wireLeadTicks, -1024);
    EXPECT_NE(decision.syt, 0xFFFF);
    EXPECT_TRUE(model.IsSeeded());
}

TEST(TxTimingModelTests, SeedPinsLeadWithinOneCorrectionGridForAnyAnchor) {
    // Companion invariant to SeedLeadIsIndependentOfRxStartupHistory: the
    // forced seed correction may only add the sub-grid snap to the execution
    // anchor, so the absolute lead stays within one cadence correction grid
    // (rollingCadence / (sytInterval << 8); the aging index trails the writer
    // by 256 entries, so the rolling sum spans 256 packets and the grid is
    // exactly one frame, 512 ticks at 48 kHz) and the corrected phase lands
    // on the recovered RX frame lattice. The frame-mapped experiment violated
    // this with leads millions of ticks in the past that only the
    // mod-16-cycle SYT field made look plausible on the wire.
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    RxSytCadence::Snapshot snapshot{};
    ASSERT_TRUE(cadence.TrySnapshot(snapshot));
    const TxTimingModel::Config config{};
    const int64_t grid = static_cast<int64_t>(snapshot.rollingCadenceTicks) /
                         (static_cast<int64_t>(config.sytIntervalFrames) << 8);
    ASSERT_EQ(grid, Timing::kTicksPerSample48k);

    constexpr int64_t kAnchorOffsets[] = {0, 1, 333, 1023, 4095, 100'000,
                                          -5'000};
    for (const int64_t anchorOffset : kAnchorOffsets) {
        TxTimingModel model{};
        model.Configure(config);
        const int64_t anchor =
            Timing::normalizeOffsetDomain(recovered + anchorOffset);
        const auto decision = model.PeekNextDataSyt(anchor, cadence);
        ASSERT_NE(decision.syt, 0xFFFF);
        // With the Saffire-faithful fix, the lead is ~initialLeadTicks (3072),
        // not within one grid cell. The lead must still be within the accept
        // window (below acceptLeadTicks).
        EXPECT_GE(decision.leadTicks, 0);
        EXPECT_LT(decision.leadTicks, config.acceptLeadTicks);
        // The corrected phase must still land on the recovered RX frame lattice.
        const int64_t latticeError =
            ((Timing::extOffsetDiff(model.OutputPhaseTicks(), recovered) %
              grid) + grid) % grid;
        EXPECT_EQ(latticeError, 0);
    }
}

TEST(TxTimingModelTests, ForcedAdjustReturnsCandidatePlusCorrection) {
    // Verify the Saffire-faithful forced return base: when forceAdjust fires,
    // phasePost must equal candidatePhase + correctionTicks, NOT
    // executionAnchor + correctionTicks.
    RxSytCadence cadence{};
    cadence.Reset();
    const int64_t recovered = FillCadenceToLock(cadence);

    TxTimingModel::Config config{};
    TxTimingModel model{};
    model.Configure(config);

    // Use an anchor offset that forces the deadband to be exceeded on seed.
    const int64_t anchor =
        Timing::normalizeOffsetDomain(recovered + 256);
    const auto decision = model.PeekNextDataSyt(anchor, cadence);

    ASSERT_NE(decision.syt, 0xFFFF);
    ASSERT_TRUE(decision.forceAdjustFired);

    // phasePost must be candidatePhase + correctionTicks (mod domain).
    const int64_t expected =
        Timing::normalizeOffsetDomain(
            decision.phaseTicksPre + decision.correctionTicks);
    EXPECT_EQ(decision.phaseTicksPost, expected);

    // It must NOT be executionAnchor + correctionTicks.
    const int64_t wrongBase =
        Timing::normalizeOffsetDomain(
            decision.packetAnchorTicks + decision.correctionTicks);
    if (decision.correctionTicks != 0) {
        EXPECT_NE(decision.phaseTicksPost, wrongBase);
    }
}

// ============================================================================
// TxAnchorTracker tests
// ============================================================================

TEST(TxAnchorTrackerTests, AcceptsExactMatch) {
    TxAnchorTracker tracker{};
    const int64_t phase1 = 100'000;
    auto r1 = tracker.ObserveCallbackPhase(0, phase1);
    EXPECT_EQ(r1.status, TxAnchorTracker::Status::kAccepted);
    EXPECT_FALSE(r1.resetRequired);

    const int64_t phase2 = phase1 + TxAnchorTracker::kTicksPerCycle;
    auto r2 = tracker.ObserveCallbackPhase(1, phase2);
    EXPECT_EQ(r2.status, TxAnchorTracker::Status::kAccepted);
    EXPECT_EQ(r2.continuityDiffTicks, 0);
    EXPECT_FALSE(r2.resetRequired);
}

TEST(TxAnchorTrackerTests, ToleratesOneCycleGlitch) {
    TxAnchorTracker tracker{};
    const int64_t phase1 = 100'000;
    (void)tracker.ObserveCallbackPhase(0, phase1);

    // First one-cycle glitch: accepted.
    const int64_t glitch1 = phase1 + 2 * TxAnchorTracker::kTicksPerCycle;
    auto r1 = tracker.ObserveCallbackPhase(1, glitch1);
    EXPECT_EQ(r1.status, TxAnchorTracker::Status::kOneCycleGlitchAccepted);
    EXPECT_FALSE(r1.resetRequired);

    // Second one-cycle glitch: accepted.
    const int64_t glitch2 =
        r1.callbackPhaseTicks + 2 * TxAnchorTracker::kTicksPerCycle;
    auto r2 = tracker.ObserveCallbackPhase(2, glitch2);
    EXPECT_EQ(r2.status, TxAnchorTracker::Status::kOneCycleGlitchAccepted);
    EXPECT_FALSE(r2.resetRequired);

    // Third one-cycle glitch: reset.
    const int64_t glitch3 =
        r2.callbackPhaseTicks + 2 * TxAnchorTracker::kTicksPerCycle;
    auto r3 = tracker.ObserveCallbackPhase(3, glitch3);
    EXPECT_EQ(r3.status, TxAnchorTracker::Status::kDiscontinuityReset);
    EXPECT_TRUE(r3.resetRequired);
}

TEST(TxAnchorTrackerTests, ResetsOnDiscontinuity) {
    TxAnchorTracker tracker{};
    const int64_t phase1 = 100'000;
    (void)tracker.ObserveCallbackPhase(0, phase1);

    // Non-one-cycle discontinuity: reset immediately.
    const int64_t badPhase = phase1 + 5000;  // not 0 or 3072
    auto r = tracker.ObserveCallbackPhase(1, badPhase);
    EXPECT_EQ(r.status, TxAnchorTracker::Status::kDiscontinuityReset);
    EXPECT_TRUE(r.resetRequired);
}

TEST(TxAnchorTrackerTests, ReacquiresAfterReset) {
    TxAnchorTracker tracker{};
    const int64_t phase1 = 100'000;
    (void)tracker.ObserveCallbackPhase(0, phase1);

    // Discontinuity.
    (void)tracker.ObserveCallbackPhase(1, phase1 + 5000);
    EXPECT_FALSE(tracker.IsValid());

    // Reacquire.
    const int64_t phase2 = 200'000;
    auto r = tracker.ObserveCallbackPhase(2, phase2);
    EXPECT_EQ(r.status, TxAnchorTracker::Status::kAccepted);
    EXPECT_TRUE(tracker.IsValid());
    EXPECT_FALSE(r.resetRequired);
}

} // namespace ASFW::Testing
