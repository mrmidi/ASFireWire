#include <gtest/gtest.h>

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

TEST(SaffirePhaseLoop, FirstGroupSeedsPresentationLead) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 978, 0);

    loop.BeginGroup(TxPhaseGroupUpdate{
        .projectedOffsetTicks = projected,
        .recoveredDeviceOffsetTicks = 0,
        .recoveredValid = false,
    });
    const auto packet = loop.EmitPacket(projected, ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_TRUE(packet.phaseValid);
    EXPECT_TRUE(packet.leadAccepted);
    EXPECT_EQ(packet.leadTicks, SaffireTxPhaseLoop::kInitialLeadTicks);
    EXPECT_EQ(packet.syt, SaffireTxPhaseLoop::EncodeOffsetTicksToSyt(projected +
                                                                     SaffireTxPhaseLoop::kInitialLeadTicks));
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

TEST(SaffirePhaseLoop, GroupUpdateRebasesToRecoveredDevicePhaseOutsideDeadband) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 100, 0);
    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected});

    const int64_t recovered = projected + 512;
    loop.BeginGroup(TxPhaseGroupUpdate{
        .projectedOffsetTicks = projected,
        .recoveredDeviceOffsetTicks = recovered,
        .recoveredValid = true,
    });

    EXPECT_EQ(loop.OutputPhaseTicks(),
              ASFW::Timing::normalizeOffsetDomain(recovered +
                                                   SaffireTxPhaseLoop::kInitialLeadTicks));
}

TEST(SaffirePhaseLoop, OutOfWindowLeadEmitsNoInfoButKeepsAdvancing) {
    SaffireTxPhaseLoop loop;
    const int64_t projected = ASFW::Timing::tstampToOffsets(0, 100, 0);
    loop.BeginGroup(TxPhaseGroupUpdate{.projectedOffsetTicks = projected});

    const auto packet = loop.EmitPacket(projected - 20000, ASFW::Timing::kSytPacketStepTicks48k);

    EXPECT_TRUE(packet.phaseValid);
    EXPECT_FALSE(packet.leadAccepted);
    EXPECT_EQ(packet.syt, 0xFFFFu);
    EXPECT_NE(loop.OutputPhaseTicks(), packet.phaseTicks);
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
