#include "Audio/Config/InputSafetyPolicy.hpp"
#include "Audio/Wire/AMDTP/RxSequenceReplay.hpp"
#include "Shared/Isoch/AudioTimingGeometry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Audio::Runtime::RxSequenceEntry;
using ASFW::Audio::Runtime::RxSequenceReplayReader;
using ASFW::Audio::Runtime::RxSequenceReplayReadDiagnostic;
using ASFW::Audio::Runtime::RxSequenceReplayReadFailure;
using ASFW::Audio::Runtime::RxSequenceReplayState;
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

TEST(RxDrivenTimingTests, ZtsGridIsObservedAtDataPacketStartWithoutProjection) {
    constexpr std::array<uint32_t, 4> cadence{8, 8, 8, 0};
    constexpr uint64_t period =
        AudioTimingGeometry::kHalZeroTimestampPeriodFrames;
    static_assert(period % AudioTimingGeometry::kCadenceBlockFrames == 0);

    uint64_t absoluteFrame = 0;
    std::vector<uint64_t> observedFrames;
    for (uint64_t packet = 0; observedFrames.size() < 2; ++packet) {
        const uint32_t decodedFrames =
            cadence[packet % cadence.size()];
        if (decodedFrames != 0 &&
            absoluteFrame != 0 &&
            (absoluteFrame % period) == 0) {
            observedFrames.push_back(absoluteFrame);
        }
        absoluteFrame += decodedFrames;
    }

    ASSERT_EQ(observedFrames.size(), 2U);
    EXPECT_EQ(observedFrames[0], period);
    EXPECT_EQ(observedFrames[1], 2 * period);
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
    RxSequenceReplayReadDiagnostic diagnostic{};
    EXPECT_FALSE(reader.TryRead(replay, entry, &diagnostic));
    EXPECT_EQ(diagnostic.failure, RxSequenceReplayReadFailure::kEpochChanged);
    EXPECT_EQ(diagnostic.readerEpoch, 1U);
    EXPECT_EQ(diagnostic.replayEpoch, 2U);
    EXPECT_FALSE(replay.IsEstablished());
}

TEST(RxDrivenTimingTests, ReplayReadReportsInactiveAndAheadOfProducer) {
    RxSequenceReplayState replay{};
    replay.Reset();

    RxSequenceReplayReader reader{};
    RxSequenceEntry entry{};
    RxSequenceReplayReadDiagnostic diagnostic{};
    EXPECT_FALSE(reader.TryRead(replay, entry, &diagnostic));
    EXPECT_EQ(diagnostic.failure, RxSequenceReplayReadFailure::kReaderInactive);
    EXPECT_FALSE(diagnostic.replayEstablished);

    for (uint64_t cycle = 0;
         cycle < RxSequenceReplayState::kReadDelay;
         ++cycle) {
        replay.Publish(RxSequenceEntry{});
    }
    ASSERT_TRUE(replay.MarkEstablished());
    ASSERT_TRUE(reader.Begin(replay));
    while (reader.TryRead(replay, entry)) {
    }
    EXPECT_FALSE(reader.TryRead(replay, entry, &diagnostic));
    EXPECT_EQ(diagnostic.failure, RxSequenceReplayReadFailure::kAheadOfProducer);
    EXPECT_EQ(diagnostic.readerCursor, diagnostic.producerCursor);
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

TEST(RxDrivenTimingTests, GeometryUsesSixCycleInterruptsAndCurrentTxDepths) {
    EXPECT_EQ(AudioTimingGeometry::kRxPacketsPerGroup, 6U);
    EXPECT_EQ(AudioTimingGeometry::kTxPacketsPerGroup, 6U);
    EXPECT_EQ(AudioTimingGeometry::kMaximumNominalFramesPerInterrupt, 40U);
    EXPECT_EQ(
        AudioTimingGeometry::kHalZeroTimestampPeriodFrames,
        ASFW::IsochTransport::kActiveAudioHalBufferProfile
            .zeroTimestampPeriodFrames);
    EXPECT_EQ(AudioTimingGeometry::kRxDescriptorPackets, 504U);
    EXPECT_EQ(AudioTimingGeometry::kTxHardwareRingPackets, 48U);
    EXPECT_EQ(AudioTimingGeometry::kTxPreparationSlackPackets, 96U);
    EXPECT_EQ(AudioTimingGeometry::kTxCoverageLeadPackets, 144U);
    // 400-cycle content horizon at worst-case 44.1k cadence, plus one full
    // 512-frame client write window.
    EXPECT_EQ(AudioTimingGeometry::kTxExposureLeadPackets, 438U);
    EXPECT_EQ(AudioTimingGeometry::kTxFrameExposureWindowPackets, 534U);
    EXPECT_EQ(AudioTimingGeometry::kTxPreparationLeadPackets, 678U);
    EXPECT_EQ(AudioTimingGeometry::kTxSharedSlotPackets, 912U);
}

TEST(RxDrivenTimingTests, InputSafetyIsVisibilityMarginNotClientWindow) {
    // The IO buffer window must NOT inflate the safety offset (was 624). The
    // margin is one interrupt batch + jitter (40+64=104), floored by the
    // profile value and aligned up to the 32-frame grid.
    //   profile floor 128 wins over the 104 batch -> 128.
    EXPECT_EQ(ASFW::Audio::RequiredInputSafetyFrames(128, 40, 64), 128U);
    //   no profile floor -> interrupt batch 104 aligned up to 128.
    EXPECT_EQ(ASFW::Audio::RequiredInputSafetyFrames(0, 40, 64), 128U);
    //   a larger profile floor is honored, aligned: 200 -> 224.
    EXPECT_EQ(ASFW::Audio::RequiredInputSafetyFrames(200, 40, 64), 224U);
}

} // namespace

TEST(RxDrivenTimingTests, ReaderReBeginReanchorsAfterHistoryOverwritten) {
    RxSequenceReplayState replay{};
    replay.Reset();

    auto publishCycles = [&replay](uint64_t firstCycle, uint64_t count) {
        for (uint64_t cycle = firstCycle;
             cycle < firstCycle + count;
             ++cycle) {
            RxSequenceEntry entry{};
            entry.firstAudioFrame = cycle * 6;
            entry.dataBlocks = static_cast<uint16_t>(
                (cycle % 4) == 3 ? 0 : 8);
            entry.sytOffset = entry.dataBlocks == 0
                                  ? RxSequenceReplayState::kNoInfo
                                  : static_cast<uint32_t>(cycle);
            replay.Publish(entry);
        }
    };

    publishCycles(0, RxSequenceReplayState::kCapacity);
    ASSERT_TRUE(replay.MarkEstablished());

    RxSequenceReplayReader reader{};
    ASSERT_TRUE(reader.Begin(replay));
    const uint32_t epochAtBegin = replay.Epoch();

    // The producer runs a full capacity plus one ahead while the reader is
    // idle: the reader's cursor now points below the retained window.
    publishCycles(RxSequenceReplayState::kCapacity,
                  RxSequenceReplayState::kCapacity + 1);

    RxSequenceEntry entry{};
    RxSequenceReplayReadDiagnostic diagnostic{};
    ASSERT_FALSE(reader.TryRead(replay, entry, &diagnostic));
    EXPECT_EQ(diagnostic.failure,
              RxSequenceReplayReadFailure::kHistoryOverwritten);

    // Re-anchoring is a repositioning of the same established stream, not an
    // epoch transition: Begin() lands kReadDelay behind the live producer and
    // the next read succeeds with the entry published for that cursor.
    ASSERT_TRUE(reader.Begin(replay));
    EXPECT_EQ(replay.Epoch(), epochAtBegin);
    EXPECT_EQ(reader.NextCursor(),
              replay.ProducerCursor() - RxSequenceReplayState::kReadDelay);

    ASSERT_TRUE(reader.TryRead(replay, entry, &diagnostic));
    const uint64_t expectedCycle =
        replay.ProducerCursor() - RxSequenceReplayState::kReadDelay;
    EXPECT_EQ(entry.firstAudioFrame, expectedCycle * 6);
}
