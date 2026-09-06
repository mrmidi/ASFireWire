// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Shared/AudioRuntimeTuning.hpp"

#include <gtest/gtest.h>

#include <initializer_list>

namespace {
using namespace ASFW::Audio::Shared;

constexpr uint32_t kGroup = AudioTimingGeometry::kTxPacketsPerGroup;

AudioRuntimeTuning WithGroups(uint32_t groups) {
    AudioRuntimeTuning t{};
    t.txDispatchSlackPackets = groups * kGroup;
    return t;
}

// The panel exists to sweep values without a rebuild, so the shipping defaults
// must be reproducible from it exactly -- otherwise "reset to default" silently
// installs a different driver than the one that shipped.
TEST(AudioRuntimeTuning, DefaultsReproduceTheCompileTimeGeometryExactly) {
    const AudioRuntimeTuning d{};
    EXPECT_EQ(d.PreparedTargetPackets(),
              AudioTimingGeometry::kTxPreparedTargetCycleSlots);
    EXPECT_EQ(d.MaxCoveredDeltaConsumedPackets(),
              AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets);
    EXPECT_EQ(d.frameRingFrames, AudioTimingGeometry::kFrameRingFrames);
    EXPECT_EQ(d.zeroTimestampPeriodFrames,
              AudioTimingGeometry::kHalZeroTimestampPeriodFrames);
    EXPECT_EQ(PreparedLeadFrames(d), 720U);

    const auto v = ValidateTuning(d);
    EXPECT_TRUE(v.Applicable());
    // A default that warns would train the operator to ignore warnings.
    EXPECT_EQ(v.warnings, 0U);
}

// The shipping slack sits exactly on the asserted floor
// (kTxMaxCoveredDeltaConsumedPackets >= 12 * kTxPacketsPerGroup is 72 >= 72),
// so the boundary must be inclusive: 12 groups is inside, 11 is outside.
TEST(AudioRuntimeTuning, AssertedFloorIsInclusiveAtTwelveGroups) {
    EXPECT_FALSE(ValidateTuning(WithGroups(12))
                     .Has(TuningWarning::kDispatchSlackBelowAssertedFloor));
    EXPECT_TRUE(ValidateTuning(WithGroups(11))
                    .Has(TuningWarning::kDispatchSlackBelowAssertedFloor));
    for (const uint32_t groups : {8U, 6U, 4U, 2U, 1U}) {
        const auto v = ValidateTuning(WithGroups(groups));
        EXPECT_TRUE(v.Applicable()) << groups << " groups must stay applicable";
        EXPECT_TRUE(v.Has(TuningWarning::kDispatchSlackBelowAssertedFloor))
            << groups << " groups must report crossing the floor";
    }
}

// Reducing is the supported direction; growing past the store is not, because
// the shared slot ring is cross-process memory sized at build time.
TEST(AudioRuntimeTuning, PreparedTargetIsBoundedByTheSharedSlotStore) {
    AudioRuntimeTuning grow{};
    grow.txDispatchSlackPackets =
        AudioTimingGeometry::kTxSharedSlotPackets; // target + guard > store
    EXPECT_EQ(ValidateTuning(grow).rejection,
              TuningRejection::kPreparedTargetExceedsSharedSlots);

    // Exactly filling the store is the default and must be accepted without the
    // over-provision note; anything smaller earns it.
    EXPECT_FALSE(ValidateTuning(AudioRuntimeTuning{})
                     .Has(TuningWarning::kSharedSlotRingOverProvisioned));
    EXPECT_TRUE(ValidateTuning(WithGroups(8))
                    .Has(TuningWarning::kSharedSlotRingOverProvisioned));
}

TEST(AudioRuntimeTuning, DegenerateAndMalformedRequestsAreRejectedByName) {
    AudioRuntimeTuning zero{};
    zero.txDispatchSlackPackets = 0;
    EXPECT_EQ(ValidateTuning(zero).rejection, TuningRejection::kDispatchSlackZero);

    // The ownership guard is the descriptor ring; a request that disagrees is
    // malformed rather than aggressive, and must not be silently corrected.
    AudioRuntimeTuning guard{};
    guard.txOwnershipGuardPackets =
        AudioTimingGeometry::kTxOwnershipGuardCycleSlots + 1;
    EXPECT_EQ(ValidateTuning(guard).rejection,
              TuningRejection::kOwnershipGuardMismatch);

    AudioRuntimeTuning ring{};
    ring.frameRingFrames = 8'000; // not a multiple of the 512-frame io budget
    EXPECT_EQ(ValidateTuning(ring).rejection,
              TuningRejection::kFrameRingNotMultipleOfIoBudget);

    AudioRuntimeTuning zts{};
    zts.zeroTimestampPeriodFrames = 3'000; // does not divide the 8192 ring
    EXPECT_EQ(ValidateTuning(zts).rejection,
              TuningRejection::kFrameRingNotMultipleOfZtsPeriod);

    // Must divide the ring and be an io-budget multiple, so the only surviving
    // failure is packet alignment: 8160/480 = 17, 8160/2040 = 4, 2040 % 32 = 24.
    // (Every divisor of a power-of-two ring >= 32 is itself a multiple of 32,
    // so this case is unreachable without a non-power-of-two ring.)
    AudioRuntimeTuning packetAlign{};
    packetAlign.frameRingFrames = 8'160;
    packetAlign.clientIoBudgetFrames = 480;
    packetAlign.zeroTimestampPeriodFrames = 2'040;
    EXPECT_EQ(ValidateTuning(packetAlign).rejection,
              TuningRejection::kZtsPeriodNotMultipleOfMaxPacketFrames);
}

// AudioGeometryPolicy rejects a safety offset that reaches the frame ring.
TEST(AudioRuntimeTuning, DeclarationsAreBoundedByTheFrameRing) {
    AudioRuntimeTuning safety{};
    safety.outputSafetyOffsetFrames = safety.frameRingFrames;
    EXPECT_EQ(ValidateTuning(safety).rejection,
              TuningRejection::kSafetyOffsetExceedsFrameRing);

    AudioRuntimeTuning latency{};
    latency.inputLatencyFrames = latency.frameRingFrames + 1;
    EXPECT_EQ(ValidateTuning(latency).rejection,
              TuningRejection::kLatencyExceedsFrameRing);

    // Declaring less than the profile resolved is how a recording ends up
    // misaligned, so it warns even though it is applicable.
    AudioRuntimeTuning profile{};
    profile.outputLatencyFrames = 67;
    AudioRuntimeTuning under{};
    under.outputLatencyFrames = 10;
    EXPECT_TRUE(ValidateTuning(under, profile)
                    .Has(TuningWarning::kDeclaredLatencyBelowDefault));
    EXPECT_TRUE(ValidateTuning(under, profile).Applicable());
}

// The panel's headline number. These are the values Logic and rtl_loopback
// printed for the Duet on 2026-09-06, so a regression here means the panel is
// lying about what the host will believe.
TEST(AudioRuntimeTuning, DeclaredLatencyMatchesObservedHostArithmetic) {
    AudioRuntimeTuning duet{};
    duet.outputLatencyFrames = 67;
    duet.inputLatencyFrames = 40;
    duet.outputSafetyOffsetFrames = 50;
    duet.inputSafetyOffsetFrames = 50;

    struct Expectation {
        uint32_t io, sched, roundTrip, outputPath;
    };
    // rtl_loopback "declared scheduling" / "declared round-trip", and Logic's
    // "9,6 ms Roundtrip (5,1 ms Output)" at a 128-frame buffer.
    for (const auto& e : {Expectation{32, 164, 271, 149},
                          Expectation{64, 228, 335, 181},
                          Expectation{128, 356, 463, 245}}) {
        const auto m = ComputeDeclaredLatency(duet, e.io);
        EXPECT_EQ(m.schedulingDistanceFrames, e.sched) << "io=" << e.io;
        EXPECT_EQ(m.roundTripFrames, e.roundTrip) << "io=" << e.io;
        EXPECT_EQ(m.outputPathFrames, e.outputPath) << "io=" << e.io;
        EXPECT_EQ(m.declaredHardwareFrames, 107U);
    }
}

// Apply cost drives what the UI warns about before the operator commits, so an
// under-report is the dangerous direction: a device vanishing from every app
// when the panel promised a stream re-arm.
TEST(AudioRuntimeTuning, ApplyCostReportsTheMostDisruptiveSelectedChange) {
    const AudioRuntimeTuning current{};
    const uint32_t all = TuningGroup::kTransmitDepth |
                         TuningGroup::kDeclarations | TuningGroup::kHalGeometry;

    EXPECT_EQ(CostOf(all, current, current), ApplyCost::kNothing);

    EXPECT_EQ(CostOf(static_cast<uint32_t>(TuningGroup::kTransmitDepth),
                     WithGroups(8), current),
              ApplyCost::kStreamRearm);

    AudioRuntimeTuning decl{};
    decl.outputLatencyFrames = 931;
    EXPECT_EQ(CostOf(static_cast<uint32_t>(TuningGroup::kDeclarations), decl,
                     current),
              ApplyCost::kStreamRearm);

    AudioRuntimeTuning hal{};
    hal.zeroTimestampPeriodFrames = 4'096;
    EXPECT_EQ(CostOf(static_cast<uint32_t>(TuningGroup::kHalGeometry), hal,
                     current),
              ApplyCost::kDeviceRepublish);
    // Republish outranks re-arm when both are selected.
    hal.txDispatchSlackPackets = 8 * kGroup;
    EXPECT_EQ(CostOf(all, hal, current), ApplyCost::kDeviceRepublish);

    // A group that is not selected must not contribute cost, so an operator
    // editing declarations cannot accidentally republish the device.
    AudioRuntimeTuning halOnlyEdit{};
    halOnlyEdit.zeroTimestampPeriodFrames = 4'096;
    EXPECT_EQ(CostOf(static_cast<uint32_t>(TuningGroup::kDeclarations),
                     halOnlyEdit, current),
              ApplyCost::kNothing);
}

} // namespace
