#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

#include "AudioWire/AMDTP/TimingUtils.hpp"
#include "Isoch/Core/IsochEventGroup.hpp"
#include "AudioEngine/DirectIsoch/Timing/SaffirePhaseLoop.hpp"

using ASFW::Isoch::Core::IsTimingGroupBoundary;
using ASFW::Isoch::Core::PreviousPacketIndex;
using ASFW::AudioEngine::DirectIsoch::SaffireTxPhaseLoop;
using ASFW::AudioEngine::DirectIsoch::TxPhaseGroupUpdate;
using ASFW::Isoch::Core::TimingGroupPacketCount48k;

TEST(SaffirePhaseLoop, EmitsNoInfoBeforeFirstHardwareGroup) {
    SaffireTxPhaseLoop loop;

    const auto packet = loop.EmitPacket(/*projectedOffsetTicks=*/0,
                                        ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_FALSE(packet.phaseValid);
    EXPECT_FALSE(packet.leadAccepted);
    EXPECT_EQ(packet.syt, 0xFFFFu);
}

// Seed comes from the FIRST EmitPacket's per-packet cycle, not the coarse group
// cycle BeginGroup is handed. Here BeginGroup is given a wildly different cycle; the
// phase still lands kInitialLeadTicks ahead of the per-packet cycle.
TEST(SaffirePhaseLoop, FirstEmitSeedsLeadFromPacketCycleNotGroupCycle) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 978, 0);

    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected - 99999});
    const auto packet = loop.EmitPacket(projected, ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_TRUE(packet.phaseValid);
    EXPECT_TRUE(packet.leadAccepted);
    EXPECT_EQ(packet.leadTicks, SaffireTxPhaseLoop::kInitialLeadTicks);
    EXPECT_EQ(packet.phaseTicks,
              ASFW::Timing::normalizeOffsetDomain(projected + SaffireTxPhaseLoop::kInitialLeadTicks));
    EXPECT_EQ(packet.syt, SaffireTxPhaseLoop::EncodeOffsetTicksToSyt(
                              projected + SaffireTxPhaseLoop::kInitialLeadTicks));
}

TEST(SaffirePhaseLoop, PacketEmitAdvancesByCadenceRingDelta) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 100, 0);
    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected});

    const auto first = loop.EmitPacket(projected, /*cadenceDeltaTicks=*/4096);
    const auto second = loop.EmitPacket(projected + 4096, /*cadenceDeltaTicks=*/4096);

    EXPECT_TRUE(first.leadAccepted);
    EXPECT_TRUE(second.leadAccepted);
    EXPECT_EQ(second.phaseTicks, ASFW::Timing::normalizeOffsetDomain(first.phaseTicks + 4096));
}

// The recovered device offset lives in a foreign (device-clock) origin and must NOT
// move the presentation phase, even with recoveredValid=true.
TEST(SaffirePhaseLoop, RecoveredDeviceOffsetDoesNotAffectPhase) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 100, 0);
    loop.BeginGroup(TxPhaseGroupUpdate{
        .projectedOffsetTicks = projected,
        .recoveredDeviceOffsetTicks = ASFW::Timing::tstampToOffsets(0, 5000, 1234),
        .recoveredValid = true,
    });
    const auto packet = loop.EmitPacket(projected, ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_EQ(packet.phaseTicks,
              ASFW::Timing::normalizeOffsetDomain(projected + SaffireTxPhaseLoop::kInitialLeadTicks));
}

// ===========================================================================
// THE regression test for the field bug: the emitted SYT must advance SMOOTHLY by
// the sample cadence (4096 ticks/packet), carrying sub-cycle offsets — NOT snap to
// whole-cycle boundaries (offset always 0, stepping 3072/6144). The latter is what
// the device's clock recovery saw on the wire and turned into garbage audio.
//
// This drives the loop the way the pipeline does: `projected` is the CYCLE-QUANTIZED
// transmit cycle (offset always 0), stepping 1 or 2 cycles to average the 4/3 data-
// packet cadence. The previous per-packet "servo" snapped onto that quantized cycle
// every packet; this test fails hard on that code and passes on the free-run loop.
// ===========================================================================
TEST(SaffirePhaseLoop, SytAdvancesSmoothlyByCadenceDespiteCycleQuantizedProjected) {
    SaffireTxPhaseLoop loop;
    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = 0});

    constexpr int64_t kCadence = int64_t(ASFW::Timing::kSytPacketStepTicks48k); // 4096
    uint16_t prevSyt = 0;
    bool havePrev = false;
    bool sawNonZeroOffset = false;

    for (int i = 0; i < 256; ++i) {
        // Cycle-quantized transmit cycle: 1.333 cycles/packet on average, integer
        // cycles only (sub-cycle offset always 0), exactly like request.transmitCycle.
        const uint32_t cycle =
            static_cast<uint32_t>((static_cast<int64_t>(i) * 4) / 3) % ASFW::Timing::kCyclesPerSecond;
        const int64_t projected = ASFW::Timing::tstampToOffsets(0, cycle, 0);

        const auto pkt = loop.EmitPacket(projected, kCadence);
        ASSERT_TRUE(pkt.leadAccepted);
        ASSERT_NE(pkt.syt, 0xFFFFu);

        if ((pkt.syt & 0x0FFFu) != 0u) {
            sawNonZeroOffset = true;
        }
        if (havePrev) {
            EXPECT_EQ(ASFW::Timing::SYTDiffInOffsets(pkt.syt, prevSyt), kCadence)
                << "SYT must step by the smooth cadence, not snap to whole cycles (packet " << i << ")";
        }
        prevSyt = pkt.syt;
        havePrev = true;
    }

    // The bug pinned every SYT offset to 0x000; the fix progresses through offsets.
    EXPECT_TRUE(sawNonZeroOffset);
}

// The free-run must NOT re-sync on the per-packet quantization swing, but MUST
// re-sync on real multi-cycle desync.
TEST(SaffirePhaseLoop, ResyncsOnlyOnGrossDesyncNotPerPacketSwing) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = 100000;
    const int64_t seed = projected + SaffireTxPhaseLoop::kInitialLeadTicks;
    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected});
    loop.EmitPacket(projected, ASFW::Timing::kSytPacketStepTicks48k); // seed, then advance by one step

    // A within-a-cycle swing must NOT snap: emit one cycle ahead, err well under the
    // re-sync band → phase keeps free-running, unchanged (seed advanced by one step).
    const auto small = loop.EmitPacket(projected + ASFW::Timing::kTicksPerCycle,
                                       ASFW::Timing::kSytPacketStepTicks48k);
    EXPECT_EQ(small.phaseTicks,
              ASFW::Timing::normalizeOffsetDomain(seed + ASFW::Timing::kSytPacketStepTicks48k));

    // A gross multi-cycle desync re-syncs back to the target lead.
    const auto gross = loop.EmitPacket(projected + 100000, ASFW::Timing::kSytPacketStepTicks48k);
    EXPECT_EQ(gross.leadTicks, SaffireTxPhaseLoop::kInitialLeadTicks);
    EXPECT_EQ(gross.phaseTicks,
              ASFW::Timing::normalizeOffsetDomain((projected + 100000) +
                                                  SaffireTxPhaseLoop::kInitialLeadTicks));
}

// Under ppm cadence drift the phase keeps emitting a valid SYT every packet, the SYT
// stays smooth (steps by the cadence) except for the rare gross-desync re-sync, and
// the lead stays bounded.
TEST(SaffirePhaseLoop, SytStaysSmoothAndBoundedUnderPpmDrift) {
    SaffireTxPhaseLoop loop;

    constexpr int64_t kStep = int64_t(ASFW::Timing::kSytPacketStepTicks48k);
    const int64_t cadenceFast = kStep + 2; // device phase advances faster than bus cycle

    int64_t projected = 1'000'000;
    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected});

    int64_t maxAbsLeadErr = 0;
    int resyncGlitches = 0;
    uint16_t prevSyt = 0;
    bool havePrev = false;
    for (int i = 0; i < 5000; ++i) {
        const auto pkt = loop.EmitPacket(projected, cadenceFast);
        ASSERT_NE(pkt.syt, 0xFFFFu);
        const int64_t err = pkt.leadTicks - SaffireTxPhaseLoop::kInitialLeadTicks;
        maxAbsLeadErr = std::max(maxAbsLeadErr, err < 0 ? -err : err);
        if (havePrev && ASFW::Timing::SYTDiffInOffsets(pkt.syt, prevSyt) != cadenceFast) {
            ++resyncGlitches; // the rare gross-desync correction
        }
        prevSyt = pkt.syt;
        havePrev = true;
        projected = ASFW::Timing::normalizeOffsetDomain(projected + kStep);
    }

    // Lead never wanders past the re-sync band, and re-syncs are rare (a handful over
    // 5000 packets), so the SYT is overwhelmingly smooth.
    EXPECT_LE(maxAbsLeadErr, SaffireTxPhaseLoop::kResyncBandTicks);
    EXPECT_LT(resyncGlitches, 10);
}

TEST(IsochEventGroup, SaffireTimingGroupIsEightPackets) {
    EXPECT_EQ(TimingGroupPacketCount48k(), 8u);
    EXPECT_FALSE(IsTimingGroupBoundary(6));
    EXPECT_TRUE(IsTimingGroupBoundary(7));
    EXPECT_TRUE(IsTimingGroupBoundary(15));
}

TEST(IsochEventGroup, PreviousPacketIndexWrapsInRing) {
    EXPECT_EQ(PreviousPacketIndex(0, 192), 191u);
    EXPECT_EQ(PreviousPacketIndex(75, 192), 74u);
    EXPECT_EQ(PreviousPacketIndex(0, 0), 0u);
}
