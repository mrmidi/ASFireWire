#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Wire/AMDTP/RxSytCadence.hpp"
#include "ASFWDriver/Audio/Wire/AMDTP/TxTimingModel.hpp"

namespace ASFW::Testing {
namespace {

using Driver::RxSytCadence;
using Driver::TxTimingModel;
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
    EXPECT_EQ(decision.health, LeadHealth::kTightWarn);
    EXPECT_EQ(decision.leadTicks, 0);
    EXPECT_EQ(decision.syt, SytFromTicks(recovered));
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
    EXPECT_EQ(decision.syt, 0x92B0);
    EXPECT_EQ(decision.leadTicks, 0);
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

    EXPECT_EQ(Timing::SYTDiffInOffsets(
                  SytFromTicks(model.OutputPhaseTicks()), first.syt),
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
    EXPECT_EQ(Timing::extOffsetDiff(
                  model.OutputPhaseTicks(), recovered),
              Timing::kTicksPerSample48k);
    EXPECT_LT(model.OutputPhaseTicks(), kEightSecondTicks);
}

} // namespace ASFW::Testing
