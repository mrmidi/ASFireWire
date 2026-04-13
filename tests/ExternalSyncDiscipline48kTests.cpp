#include <gtest/gtest.h>

#include "../ASFWDriver/Isoch/Core/ExternalSyncDiscipline48k.hpp"

using ASFW::Isoch::Core::ExternalSyncDiscipline48k;

namespace {
uint16_t EncodeSytFromTick(int32_t tick) {
    constexpr int32_t kDomain = ExternalSyncDiscipline48k::kTickDomain;
    int32_t normalized = tick % kDomain;
    if (normalized < 0) {
        normalized += kDomain;
    }
    const uint16_t cycle4 = static_cast<uint16_t>((normalized / ExternalSyncDiscipline48k::kTicksPerCycle) & 0x0F);
    const uint16_t ticks12 = static_cast<uint16_t>(normalized % ExternalSyncDiscipline48k::kTicksPerCycle);
    return static_cast<uint16_t>((cycle4 << 12) | ticks12);
}
} // namespace

TEST(ExternalSyncDiscipline48k, FirstPassSnapsToRxPhase) {
    ExternalSyncDiscipline48k discipline;

    // TX at tick 0, RX at tick 1500 — first call should correct sub-packet error.
    auto result = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(0),
                                    EncodeSytFromTick(1500));
    EXPECT_TRUE(result.active);
    EXPECT_TRUE(result.locked);
    EXPECT_TRUE(result.firstPassSnap);
    EXPECT_EQ(result.correctionTicks, 1500);
    EXPECT_EQ(result.phaseErrorTicks, 1500);
    EXPECT_TRUE(result.safetyGateOpen);
}

TEST(ExternalSyncDiscipline48k, FirstPassSnapsNegativeError) {
    ExternalSyncDiscipline48k discipline;

    // TX at tick 1800, RX at tick 200 — error wraps negative in interval domain.
    auto result = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(1800),
                                    EncodeSytFromTick(200));
    EXPECT_TRUE(result.firstPassSnap);
    // Wrapped phase: (200 - 1800) mod 4096 with signed wrap = -1600
    EXPECT_EQ(result.correctionTicks, result.phaseErrorTicks);
    EXPECT_TRUE(result.safetyGateOpen);
}

TEST(ExternalSyncDiscipline48k, DeadbandProducesNoCorrectionAfterFirstPass) {
    ExternalSyncDiscipline48k discipline;

    // First pass: snap to zero error
    auto first = discipline.Update(/*enabled=*/true,
                                   EncodeSytFromTick(500),
                                   EncodeSytFromTick(500));
    EXPECT_TRUE(first.firstPassSnap);
    EXPECT_EQ(first.correctionTicks, 0);  // zero error, zero correction

    // Second call: small drift within deadband (512 ticks) — no correction
    auto second = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(500),
                                    EncodeSytFromTick(800));
    EXPECT_FALSE(second.firstPassSnap);
    EXPECT_TRUE(second.locked);
    EXPECT_EQ(second.correctionTicks, 0);
    EXPECT_EQ(second.phaseErrorTicks, 300);  // within deadband
}

TEST(ExternalSyncDiscipline48k, FullErrorCorrectionBeyondDeadband) {
    ExternalSyncDiscipline48k discipline;

    // First pass with zero error
    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(0));

    // Drift beyond deadband: 700 ticks > 512 deadband
    auto result = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(0),
                                    EncodeSytFromTick(700));
    EXPECT_FALSE(result.firstPassSnap);
    EXPECT_EQ(result.phaseErrorTicks, 700);
    EXPECT_EQ(result.correctionTicks, 700);  // full error, not +/-1
}

TEST(ExternalSyncDiscipline48k, NoCooldownBetweenCorrections) {
    ExternalSyncDiscipline48k discipline;

    // First pass
    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(0));

    // Two consecutive corrections beyond deadband — no cooldown
    auto r1 = discipline.Update(/*enabled=*/true,
                                EncodeSytFromTick(0),
                                EncodeSytFromTick(700));
    EXPECT_EQ(r1.correctionTicks, 700);

    auto r2 = discipline.Update(/*enabled=*/true,
                                EncodeSytFromTick(0),
                                EncodeSytFromTick(-800));
    EXPECT_EQ(r2.correctionTicks, -800);
}

TEST(ExternalSyncDiscipline48k, FirstPassIgnoresWholePacketIntervalOffset) {
    ExternalSyncDiscipline48k discipline;

    // A 3-packet delta should not cause a first-pass re-phase after the generator
    // has already been seeded from RX.
    auto first = discipline.Update(/*enabled=*/true,
                                   EncodeSytFromTick(0),
                                   EncodeSytFromTick(12288));
    EXPECT_TRUE(first.firstPassSnap);
    EXPECT_EQ(first.phaseErrorTicks, 0);
    EXPECT_EQ(first.correctionTicks, 0);
}

TEST(ExternalSyncDiscipline48k, SteadyStateIgnoresWholePacketIntervalJitter) {
    ExternalSyncDiscipline48k discipline;

    // First pass: snap to zero
    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(0));

    // Steady-state: a whole-packet-interval offset (4096) due to bridge sampling
    // latency should wrap to 0 in the packet-interval domain — no correction.
    auto result = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(0),
                                    EncodeSytFromTick(4096));
    EXPECT_EQ(result.phaseErrorTicks, 0);
    EXPECT_EQ(result.correctionTicks, 0);

    // 3-packet offset also wraps to zero in steady state
    auto threePacket = discipline.Update(/*enabled=*/true,
                                         EncodeSytFromTick(0),
                                         EncodeSytFromTick(12288));
    EXPECT_EQ(threePacket.phaseErrorTicks, 0);
    EXPECT_EQ(threePacket.correctionTicks, 0);
}

TEST(ExternalSyncDiscipline48k, DisableResetsFirstPass) {
    ExternalSyncDiscipline48k discipline;

    // Enable and complete first pass
    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(0));
    EXPECT_TRUE(discipline.locked());

    // Disable
    auto disabled = discipline.Update(/*enabled=*/false,
                                      EncodeSytFromTick(0),
                                      EncodeSytFromTick(0));
    EXPECT_FALSE(disabled.active);
    EXPECT_FALSE(disabled.locked);
    EXPECT_TRUE(disabled.staleOrUnlockEvent);

    // Re-enable: should get first-pass snap again
    auto reEnabled = discipline.Update(/*enabled=*/true,
                                       EncodeSytFromTick(0),
                                       EncodeSytFromTick(1000));
    EXPECT_TRUE(reEnabled.firstPassSnap);
    EXPECT_EQ(reEnabled.correctionTicks, 1000);
}

TEST(ExternalSyncDiscipline48k, DiagnosticsTrackMinMax) {
    ExternalSyncDiscipline48k discipline;

    // First pass
    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(200));

    // Various phase errors
    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(-300));

    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(800));

    EXPECT_LE(discipline.minPhaseError(), -300);
    EXPECT_GE(discipline.maxPhaseError(), 800);
}

TEST(ExternalSyncDiscipline48k, SafetyGateOpenForNormalOffsets) {
    ExternalSyncDiscipline48k discipline;

    // First pass with moderate error
    auto result = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(0),
                                    EncodeSytFromTick(1000));
    EXPECT_TRUE(result.safetyGateOpen);
}

TEST(ExternalSyncDiscipline48k, ResetRestoresFirstPassMode) {
    ExternalSyncDiscipline48k discipline;

    // Complete first pass
    discipline.Update(/*enabled=*/true,
                      EncodeSytFromTick(0),
                      EncodeSytFromTick(0));
    EXPECT_TRUE(discipline.locked());

    // Reset
    discipline.Reset();
    EXPECT_FALSE(discipline.locked());

    // Next call should be first-pass again
    auto result = discipline.Update(/*enabled=*/true,
                                    EncodeSytFromTick(0),
                                    EncodeSytFromTick(700));
    EXPECT_TRUE(result.firstPassSnap);
    EXPECT_EQ(result.correctionTicks, 700);
}
