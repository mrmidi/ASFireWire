#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

#include "AudioWire/AMDTP/TimingUtils.hpp"
#include "Isoch/Core/IsochEventGroup.hpp"
#include "AudioEngine/DirectIsoch/Timing/SaffirePhaseLoop.hpp"
#include "AudioEngine/DirectIsoch/Sync/ExternalSyncBridge.hpp"

using ASFW::Isoch::Core::IsTimingGroupBoundary;
using ASFW::Isoch::Core::PreviousPacketIndex;
using ASFW::AudioEngine::DirectIsoch::SaffireTxPhaseLoop;
using ASFW::AudioEngine::DirectIsoch::TxPhaseGroupUpdate;
using ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge;
using ASFW::AudioEngine::DirectIsoch::ExternalSyncClockState;
using ASFW::Isoch::Core::TimingGroupPacketCount48k;

namespace {
// A device presentation phase carrying the device's fixed sub-cycle offset (the
// constant 0x0b0 the Saffire holds on the wire), at an arbitrary cycle.
constexpr int64_t kDeviceSubCycleOffset = 0x0b0;
int64_t DevicePhaseAt(uint32_t cycle) noexcept {
    return ASFW::Timing::tstampToOffsets(0, cycle, kDeviceSubCycleOffset);
}
}  // namespace

// ===========================================================================
// Pre-lock (no device clock yet): free-run a transfer-delay ahead of transmit.
// ===========================================================================

TEST(SaffirePhaseLoop, EmitsNoInfoBeforeFirstHardwareGroup) {
    SaffireTxPhaseLoop loop;

    const auto packet = loop.EmitPacket(/*projectedOffsetTicks=*/0,
                                        ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_FALSE(packet.phaseValid);
    EXPECT_FALSE(packet.leadAccepted);
    EXPECT_EQ(packet.syt, 0xFFFFu);
}

// Before the device clock is recovered the SYT seeds a transfer-delay ahead of the
// per-packet transmit cycle, so the device has a coherent ramp to start locking onto.
TEST(SaffirePhaseLoop, PreLockSeedsTransferDelayAheadOfTransmit) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 978, 0);

    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected, .recoveredValid = false});
    const auto packet = loop.EmitPacket(projected, ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_TRUE(packet.phaseValid);
    EXPECT_TRUE(packet.leadAccepted);
    EXPECT_EQ(packet.phaseTicks,
              ASFW::Timing::normalizeOffsetDomain(projected + SaffireTxPhaseLoop::kInitialLeadTicks));
    EXPECT_EQ(packet.syt, SaffireTxPhaseLoop::EncodeOffsetTicksToSyt(
                              projected + SaffireTxPhaseLoop::kInitialLeadTicks));
}

TEST(SaffirePhaseLoop, PacketEmitAdvancesByCadenceRingDelta) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 100, 0);
    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected, .recoveredValid = false});

    const auto first = loop.EmitPacket(projected, /*cadenceDeltaTicks=*/4096);
    const auto second = loop.EmitPacket(projected + 4096, /*cadenceDeltaTicks=*/4096);

    EXPECT_TRUE(first.leadAccepted);
    EXPECT_TRUE(second.leadAccepted);
    EXPECT_EQ(second.phaseTicks, ASFW::Timing::normalizeOffsetDomain(first.phaseTicks + 4096));
}

// ===========================================================================
// Device-slaved sub-cycle: cycle nibble from OUR transmit cycle, sub-cycle offset
// grafted from the recovered device clock.
// ===========================================================================

// The emitted SYT takes its CYCLE nibble from our own transmit cycle (+lead) and its
// sub-cycle OFFSET from the device's recovered phase — never the device's arbitrary
// absolute cycle.
TEST(SaffirePhaseLoop, GraftsDeviceSubCycleOntoBusCycle) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 100, 0);
    const int64_t devTarget = DevicePhaseAt(900);  // arbitrary device origin, sub-cycle 0x0b0

    loop.BeginGroup(TxPhaseGroupUpdate{
        .projectedOffsetTicks = projected,
        .recoveredDeviceOffsetTicks = devTarget,
        .recoveredValid = true,
    });
    const auto packet = loop.EmitPacket(projected, ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_TRUE(packet.leadAccepted);
    // Sub-cycle offset comes from the device.
    const int64_t cyc = ASFW::Timing::kTicksPerCycle;
    EXPECT_EQ(packet.syt & 0x0FFFu, static_cast<uint16_t>(devTarget % cyc));
    // Cycle nibble comes from our transmit cycle + lead, NOT the device's origin.
    const uint16_t busSyt = SaffireTxPhaseLoop::EncodeOffsetTicksToSyt(
        projected + SaffireTxPhaseLoop::kInitialLeadTicks);
    EXPECT_EQ((packet.syt >> 12) & 0x0Fu, (busSyt >> 12) & 0x0Fu);
}

// THE regression for the field bug: across the device's cycle-quantized blocking
// cadence the emitted SYT must hold the device's CONSTANT sub-cycle offset (0x0b0) —
// the original bug emitted offset 0x000 (cycle-quantized, no device sub-cycle) and the
// device's clock recovery turned the drift into garbage.
TEST(SaffirePhaseLoop, SlavedSytHoldsDeviceConstantSubCycleOffset) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 50, 0);
    loop.BeginGroup(TxPhaseGroupUpdate{
        .projectedOffsetTicks = projected,
        .recoveredDeviceOffsetTicks = DevicePhaseAt(900),
        .recoveredValid = true,
    });

    // Drive the real blocking cadence on the transmit cycle: 3 data packets stepping one
    // cycle, then +2 cycles across the nodata gap.
    constexpr int64_t kCadence[] = {ASFW::Timing::kTicksPerCycle,
                                    ASFW::Timing::kTicksPerCycle,
                                    2 * ASFW::Timing::kTicksPerCycle};

    int64_t transmit = projected;
    for (int i = 0; i < 60; ++i) {
        const auto pkt = loop.EmitPacket(ASFW::Timing::normalizeOffsetDomain(transmit), kCadence[i % 3]);
        ASSERT_TRUE(pkt.leadAccepted) << "packet " << i;
        ASSERT_NE(pkt.syt, 0xFFFFu) << "packet " << i;
        EXPECT_EQ(pkt.syt & 0x0FFFu, kDeviceSubCycleOffset)
            << "sub-cycle offset must hold the device constant, not 0x000 (packet " << i << ")";
        transmit += kCadence[i % 3];
    }
}

// ===========================================================================
// Integration: the FULL RX→TX path through the production recovery code.
// Feed a synthetic Saffire device SYT stream into ExternalSyncClockState /
// ExternalSyncBridge (the real cadence-ring + recovered-offset recovery), then run
// SaffireTxPhaseLoop off the recovered clock exactly as IsochAudioTxPipeline does, and
// assert the host SYT reproduces the device's constant sub-cycle offset. This is the
// test that would have caught the smooth-4096 misdiagnosis: it exercises the real
// wiring, not hand-fed phase inputs.
// ===========================================================================

namespace {
// One blocking-mode device packet: NODATA on every 4th bus cycle, otherwise a data
// packet presenting at a CONSTANT sub-cycle offset (0x0b0) at cycle = bus + lead.
uint16_t DevicePacketSyt(uint32_t busCycle) noexcept {
    constexpr uint32_t kDeviceLeadCycles = 4;
    if ((busCycle % 4u) == 3u) {
        return 0xFFFFu;  // NODATA
    }
    return SaffireTxPhaseLoop::EncodeOffsetTicksToSyt(
        ASFW::Timing::tstampToOffsets(0, busCycle + kDeviceLeadCycles, kDeviceSubCycleOffset));
}
}  // namespace

TEST(TxSyncRecoveryIntegration, HostSytReproducesDeviceConstantOffsetThroughRecovery) {
    ExternalSyncBridge bridge;
    ExternalSyncClockState rx;
    bridge.active.store(true, std::memory_order_release);

    uint32_t deviceBusCycle = 200;
    uint64_t hostTicks = 100'000;
    auto feedDevicePacket = [&]() {
        rx.ObserveSample(bridge, hostTicks, DevicePacketSyt(deviceBusCycle),
                         ExternalSyncBridge::kFdf48k, /*dbs=*/17);
        ++deviceBusCycle;
        hostTicks += ASFW::Timing::kTicksPerCycle;
    };

    // Warm the recovery up to establishment (cadence ring needs 512 deltas).
    int fed = 0;
    while (!bridge.ReadCadenceSnapshot().established && fed < 5000) {
        feedDevicePacket();
        ++fed;
    }
    auto snap = bridge.ReadCadenceSnapshot();
    ASSERT_TRUE(snap.established) << "RX recovery never established (fed " << fed << ")";

    // The recovered device phase itself carries the constant sub-cycle offset.
    EXPECT_EQ(SaffireTxPhaseLoop::EncodeOffsetTicksToSyt(snap.recoveredDeviceOffsetTicks) & 0x0FFFu,
              static_cast<uint16_t>(kDeviceSubCycleOffset));

    // Run the TX phase loop off the recovered clock, advancing the device clock in
    // lockstep (interleaved RX feed) the way the live pipeline does.
    SaffireTxPhaseLoop loop;
    loop.SeedCadenceReadIndex(snap.writeIndex);

    int64_t projTicks = ASFW::Timing::tstampToOffsets(0, 300, 0);
    bool sawLockedPacket = false;
    for (int grp = 0; grp < 32; ++grp) {
        for (int k = 0; k < 8; ++k) {  // advance the device clock ~one group
            feedDevicePacket();
        }
        snap = bridge.ReadCadenceSnapshot();
        loop.BeginGroup(TxPhaseGroupUpdate{
            .projectedOffsetTicks = ASFW::Timing::normalizeOffsetDomain(projTicks),
            .recoveredDeviceOffsetTicks = snap.recoveredDeviceOffsetTicks,
            .recoveredValid = snap.established,
        });
        for (int p = 0; p < 6; ++p) {  // ~6 data packets per group at 48k blocking
            const uint16_t delta = bridge.ReadCadenceDelta(loop.CadenceReadIndex());
            const auto pkt = loop.EmitPacket(ASFW::Timing::normalizeOffsetDomain(projTicks), delta);
            projTicks += delta;  // transmit cycle tracks device rate → lead stays in window
            if (!pkt.leadAccepted) {
                continue;
            }
            sawLockedPacket = true;
            EXPECT_EQ(pkt.syt & 0x0FFFu, static_cast<uint16_t>(kDeviceSubCycleOffset))
                << "host SYT must reproduce the device constant sub-cycle offset (grp "
                << grp << " pkt " << p << ")";
        }
    }
    EXPECT_TRUE(sawLockedPacket);
}

// ===========================================================================
// Timing-group helpers (unchanged).
// ===========================================================================

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
