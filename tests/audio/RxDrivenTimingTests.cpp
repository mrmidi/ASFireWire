#include "Audio/Config/InputSafetyPolicy.hpp"
#include "Audio/Engine/Direct/Rx/ZtsFrameProjector.hpp"
#include "Audio/Wire/AMDTP/RxSequenceReplay.hpp"
#include "Shared/Isoch/AudioTimingGeometry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Audio::Runtime::RxSequenceEntry;
using ASFW::Audio::Runtime::RxSequenceReplayReader;
using ASFW::Audio::Runtime::RxSequenceReplayState;
using ASFW::AudioEngine::Direct::Rx::ZtsFrameProjector;
using ASFW::IsochTransport::AudioTimingGeometry;

TEST(RxDrivenTimingTests, SixCycleBlockingGroupsCoverEveryCadencePhase) {
    constexpr std::array<uint32_t, 4> cadence{8, 8, 8, 0};
    constexpr std::array<uint32_t, 4> expected{40, 40, 32, 32};

    for (uint32_t phase = 0; phase < cadence.size(); ++phase) {
        uint32_t frames = 0;
        for (uint32_t cycle = 0; cycle < 6; ++cycle) {
            frames += cadence[(phase + cycle) % cadence.size()];
        }
        EXPECT_EQ(frames, expected[phase]);
    }
}

TEST(RxDrivenTimingTests, TwelveCycleSuperperiodContainsSeventyTwoFrames) {
    constexpr std::array<uint32_t, 4> cadence{8, 8, 8, 0};
    for (uint32_t phase = 0; phase < cadence.size(); ++phase) {
        uint32_t frames = 0;
        for (uint32_t cycle = 0; cycle < 12; ++cycle) {
            frames += cadence[(phase + cycle) % cadence.size()];
        }
        EXPECT_EQ(frames, 72U);
    }
}

TEST(RxDrivenTimingTests, RxRingWrapPreservesCadenceAndFramePhase) {
    constexpr std::array<uint32_t, 4> cadence{8, 8, 8, 0};
    uint64_t absoluteFrame = 0;

    for (uint32_t cycle = 0;
         cycle < AudioTimingGeometry::kRxDescriptorPackets;
         ++cycle) {
        absoluteFrame += cadence[cycle % cadence.size()];
    }

    EXPECT_EQ(
        AudioTimingGeometry::kRxDescriptorPackets % cadence.size(), 0U);
    EXPECT_EQ(absoluteFrame, 3024U);
    EXPECT_EQ(
        cadence[AudioTimingGeometry::kRxDescriptorPackets %
                cadence.size()],
        cadence[0]);
    EXPECT_EQ(absoluteFrame % 192U, 144U);

    for (uint32_t cycle = 0; cycle < 12; ++cycle) {
        absoluteFrame += cadence[cycle % cadence.size()];
    }
    EXPECT_EQ(absoluteFrame, 3096U);
}

TEST(RxDrivenTimingTests, ZtsGridAdvancesByDecodedFramesNotPacketCount) {
    ZtsFrameProjector projector{192};
    projector.Reset(0);

    constexpr std::array<uint32_t, 12> interruptFrames{
        40, 32, 40, 32, 40, 32, 40, 32, 40, 32, 40, 32};
    std::vector<uint64_t> crossingInterrupts;
    std::vector<uint64_t> crossingFrames;
    for (uint64_t interrupt = 0; interrupt < interruptFrames.size();
         ++interrupt) {
        projector.Advance(
            interruptFrames[interrupt],
            [&](uint64_t frame, uint32_t frameOffset) {
                crossingInterrupts.push_back(interrupt + 1);
                crossingFrames.push_back(frame);
                EXPECT_LE(frameOffset, interruptFrames[interrupt]);
            });
    }

    ASSERT_EQ(crossingFrames.size(), 2U);
    EXPECT_EQ(crossingFrames[0], 192U);
    EXPECT_EQ(crossingFrames[1], 384U);
    EXPECT_EQ(crossingInterrupts[0], 6U);
    EXPECT_EQ(crossingInterrupts[1], 11U);
}

TEST(RxDrivenTimingTests, SytOffsetRoundTripsAcrossDifferentTransferDelays) {
    constexpr uint32_t rxCycleTimer =
        ASFW::Timing::encodeCycleTimer(7, 7998, 0);
    constexpr uint32_t txCycleTimer =
        ASFW::Timing::encodeCycleTimer(8, 3, 0);
    constexpr uint32_t rawOffset = 4096;
    constexpr uint32_t rxTransferDelay = 12800;
    constexpr uint32_t txTransferDelay = 9728;

    const uint16_t incoming = ASFW::Audio::Runtime::ComputeReplaySyt(
        rawOffset, rxCycleTimer, rxTransferDelay);
    const uint32_t recovered =
        ASFW::Audio::Runtime::ComputeReplaySytOffset(
            incoming, rxCycleTimer, rxTransferDelay);
    const uint16_t outgoing = ASFW::Audio::Runtime::ComputeReplaySyt(
        recovered, txCycleTimer, txTransferDelay);

    EXPECT_EQ(recovered, rawOffset);
    EXPECT_EQ(
        outgoing,
        ASFW::Audio::Runtime::ComputeReplaySyt(
            rawOffset, txCycleTimer, txTransferDelay));
}

TEST(RxDrivenTimingTests, ReplayPreservesPhysicalCyclesAndStartsHalfRingBehind) {
    RxSequenceReplayState replay{};
    replay.Reset();

    constexpr std::array<uint8_t, 4> cadence{8, 8, 8, 0};
    for (uint64_t cycle = 0;
         cycle < RxSequenceReplayState::kCapacity + 248;
         ++cycle) {
        RxSequenceEntry entry{};
        entry.firstAudioFrame = cycle * 6;
        entry.sourceCycleTimer =
            ASFW::Timing::encodeCycleTimer(
                static_cast<uint32_t>((cycle / 8000) & 0x7f),
                static_cast<uint32_t>(cycle % 8000),
                0);
        entry.dataBlocks = cadence[cycle % cadence.size()];
        entry.dbc = static_cast<uint8_t>((cycle / 4) * 24);
        entry.sytOffset =
            entry.dataBlocks == 0
                ? RxSequenceReplayState::kNoInfo
                : static_cast<uint32_t>(cycle * 4096);
        replay.Publish(entry);
    }
    ASSERT_TRUE(replay.MarkEstablished());

    RxSequenceReplayReader reader{};
    ASSERT_TRUE(reader.Begin(replay));
    EXPECT_EQ(
        reader.NextCursor(),
        replay.ProducerCursor() - RxSequenceReplayState::kReadDelay);

    for (uint32_t i = 0; i < 16; ++i) {
        RxSequenceEntry entry{};
        ASSERT_TRUE(reader.TryRead(replay, entry));
        const uint64_t sourceCycle =
            replay.ProducerCursor() -
            RxSequenceReplayState::kReadDelay + i;
        EXPECT_EQ(entry.dataBlocks, cadence[sourceCycle % cadence.size()]);
        EXPECT_EQ(entry.firstAudioFrame, sourceCycle * 6);
    }
}

TEST(RxDrivenTimingTests, ReplayEpochChangeInvalidatesActiveReader) {
    RxSequenceReplayState replay{};
    replay.Reset();
    for (uint64_t cycle = 0;
         cycle < RxSequenceReplayState::kReadDelay;
         ++cycle) {
        RxSequenceEntry entry{};
        entry.sourceCycleTimer =
            ASFW::Timing::encodeCycleTimer(
                static_cast<uint32_t>((cycle / 8000) & 0x7f),
                static_cast<uint32_t>(cycle % 8000),
                0);
        replay.Publish(entry);
    }
    ASSERT_TRUE(replay.MarkEstablished());

    RxSequenceReplayReader reader{};
    ASSERT_TRUE(reader.Begin(replay));
    replay.Reset();

    RxSequenceEntry entry{};
    EXPECT_FALSE(reader.TryRead(replay, entry));
    EXPECT_FALSE(replay.IsEstablished());
}

TEST(RxDrivenTimingTests, ReplayCannotEstablishWithoutHalfRingHistory) {
    RxSequenceReplayState replay{};
    replay.Reset();
    for (uint64_t cycle = 0;
         cycle + 1 < RxSequenceReplayState::kReadDelay;
         ++cycle) {
        replay.Publish(RxSequenceEntry{});
    }

    EXPECT_FALSE(replay.MarkEstablished());
    EXPECT_FALSE(replay.IsEstablished());
}

TEST(RxDrivenTimingTests, GeometryUsesSixCycleInterruptsAndFortyEightSixtyTx) {
    EXPECT_EQ(AudioTimingGeometry::kRxPacketsPerGroup, 6U);
    EXPECT_EQ(AudioTimingGeometry::kTxPacketsPerGroup, 6U);
    EXPECT_EQ(AudioTimingGeometry::kMaximumNominalFramesPerInterrupt, 40U);
    EXPECT_EQ(AudioTimingGeometry::kHalZeroTimestampPeriodFrames, 192U);
    EXPECT_EQ(AudioTimingGeometry::kRxDescriptorPackets, 504U);
    EXPECT_EQ(AudioTimingGeometry::kTxHardwareRingPackets, 48U);
    EXPECT_EQ(AudioTimingGeometry::kTxPreparationSlackPackets, 12U);
    EXPECT_EQ(AudioTimingGeometry::kTxPreparationLeadPackets, 60U);
    EXPECT_EQ(AudioTimingGeometry::kTxSharedSlotPackets, 192U);
}

TEST(RxDrivenTimingTests, InputSafetyCoversClientTransferAndInterruptBatch) {
    EXPECT_EQ(
        ASFW::Audio::RequiredInputSafetyFrames(128, 48, 512, 40, 64),
        624U);
}

} // namespace
