#include <gtest/gtest.h>

#include "Audio/Families/BeBoB/MAudio/MAudioTxClockBridge.hpp"
#include "Audio/Families/BeBoB/MAudio/MAudioClockSourcePolicy.hpp"
#include "Audio/Protocols/BeBoB/MAudioInternalTxTiming.hpp"
#include "Common/TimingUtils.hpp"

namespace ASFW::Audio::Families::BeBoB::MAudio {
namespace {

TEST(MAudioTxClockBridgeTests,
     RequiresQualifiedDataAndPublishesTheInitialTransmitGridBoundary) {
    TxClockBridge bridge{};
    constexpr uint32_t rate = ASFW::Audio::BeBoB::kMAudioInternalTxSampleRateHz;
    constexpr uint32_t period =
        ASFW::IsochTransport::HalBufferProfileForRate(
            ASFW::Audio::BeBoB::kMAudioInternalTxSampleRateHz)
            .zeroTimestampPeriodFrames;
    ASFW::Audio::Runtime::HardwareSampleTimeline timeline{};
    ASSERT_TRUE(bridge.Arm(timeline, 1, rate, period, 12'800));
    EXPECT_EQ(timeline.Source(), ASFW::Audio::Runtime::HardwareTimelineSource::Transmit);
    EXPECT_EQ(timeline.Epoch(), bridge.Epoch());

    constexpr uint64_t hostTicks = 1'000'000'000'000ULL;
    const uint32_t cycleTimer = ASFW::Timing::encodeCycleTimer(0, 100, 0);
    const uint64_t correlationBusTicks =
        static_cast<uint64_t>(ASFW::Timing::encodedTstampToOffsets(cycleTimer));

    // The first two completed groups qualify the special-firmware clock and
    // plant the capture reference. Neither may publish a HAL clock by itself.
    EXPECT_FALSE(bridge.ObserveWake(1, cycleTimer, correlationBusTicks,
                                    hostTicks, correlationBusTicks, {})
                     .ready);
    EXPECT_FALSE(bridge.ObserveWake(2, cycleTimer, correlationBusTicks,
                                    hostTicks, correlationBusTicks, {})
                     .ready);

    // Group three's first completed DATA packet is the first qualified
    // observation. Frame zero lies on the HAL grid, so this must produce the
    // initial anchor without any RX timestamp.
    const TxDataClockObservation packet{
        .completionBusTicks = correlationBusTicks - 20'000,
        .correlationBusTicks = correlationBusTicks,
        .sampleFrame = 0,
        .frameCount = 8,
    };
    const auto result = bridge.ObserveWake(
        3, cycleTimer, correlationBusTicks, hostTicks,
        correlationBusTicks, std::span{&packet, 1});
    ASSERT_TRUE(result.ready);
    EXPECT_EQ(result.boundary.epoch, bridge.Epoch());
    EXPECT_EQ(result.boundary.sampleFrame, 0);
    EXPECT_GT(result.boundary.hostTicks, 0);
}

TEST(MAudioTxClockBridgeTests, RejectsGeometryOutsideTheValidated48KGrid) {
    TxClockBridge bridge{};
    constexpr uint32_t period =
        ASFW::IsochTransport::HalBufferProfileForRate(
            ASFW::Audio::BeBoB::kMAudioInternalTxSampleRateHz)
            .zeroTimestampPeriodFrames;
    ASFW::Audio::Runtime::HardwareSampleTimeline timeline{};
    EXPECT_FALSE(bridge.Arm(timeline, 1, 96'000, period, 12'800));
    EXPECT_FALSE(bridge.Arm(timeline, 1, ASFW::Audio::BeBoB::kMAudioInternalTxSampleRateHz,
                            period + 1, 12'800));
}

TEST(MAudioTxClockBridgeTests, KeepsRxAndTxClockSourcesExclusiveForHalMirror) {
    EXPECT_TRUE(ShouldMirrorRxClockAnchor(false));
    EXPECT_FALSE(ShouldMirrorRxClockAnchor(true));
}

TEST(MAudioTxClockBridgeTests, UsesTheWireSytPhaseForEachDataPacket) {
    EXPECT_EQ(SytOffsetTicksForPacketIndex(0), 0);
    EXPECT_EQ(SytOffsetTicksForPacketIndex(1), 1024);
    EXPECT_EQ(SytOffsetTicksForPacketIndex(2), 2048);
    EXPECT_EQ(SytOffsetTicksForPacketIndex(3), 0); // NO-DATA phase
    EXPECT_EQ(SytOffsetTicksForPacketIndex(912), 0);
    EXPECT_EQ(SytOffsetTicksForPacketIndex(913), 1024);

    // Three consecutive DATA packets each carry eight 48 kHz frames. The
    // physical presentation times must therefore be 4096 bus ticks apart,
    // even though their transmit cycles are only 3072 ticks apart.
    constexpr uint32_t cycleTicks = ASFW::Timing::kTicksPerCycle;
    const uint32_t first = 0 * cycleTicks + SytOffsetTicksForPacketIndex(0);
    const uint32_t second = 1 * cycleTicks + SytOffsetTicksForPacketIndex(1);
    const uint32_t third = 2 * cycleTicks + SytOffsetTicksForPacketIndex(2);
    EXPECT_EQ(second - first, 4096U);
    EXPECT_EQ(third - second, 4096U);
}

} // namespace
} // namespace ASFW::Audio::Families::BeBoB::MAudio
