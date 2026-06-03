#include <AudioDriverKit/AudioDriverKit.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioDebugSnapshot.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioStreamMode;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::AudioWireFormat;
using ASFW::Audio::Runtime::CaptureDirectAudioDebugSnapshot;
using ASFW::Audio::Runtime::DirectAudioDebugLogState;
using ASFW::Audio::Runtime::DirectAudioDebugSnapshot;
using ASFW::Audio::Runtime::kDirectAudioDebugLogIntervalNs;
using ASFW::Audio::Runtime::ShouldLogDirectAudioDebugSnapshot;

TEST(DirectAudioDebugSnapshotTests, CapturesBindingCountersAndCursors) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> input{};
    std::array<int32_t, 16> output{};

    const AudioGraphBinding binding{
        .guid = 0x1122334455667788ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .inputBase = input.data(),
            .outputBase = output.data(),
            .inputFrameCapacity = 8,
            .outputFrameCapacity = 8,
            .inputChannels = 2,
            .outputChannels = 2,
        },
        .control = &control,
        .deviceToHostAm824Slots = 2,
        .hostToDeviceAm824Slots = 2,
        .streamMode = AudioStreamMode::kBlocking,
        .hostToDeviceWireFormat = AudioWireFormat::kAM824,
        .audioDevice = &audioDevice,
    };

    control.client.PublishBeginRead(100, 10, 32);
    control.client.PublishWriteEnd(200, 20, 32);
    control.counters.CountBeginRead();
    control.counters.CountWriteEnd();
    control.counters.txPackets.store(7, std::memory_order_relaxed);
    control.counters.txUnderruns.store(3, std::memory_order_relaxed);
    control.counters.txSilenceSubstitutions.store(2, std::memory_order_relaxed);

    const auto snapshot = CaptureDirectAudioDebugSnapshot(
        binding,
        true,
        32,
        64,
        32,
        1,
        2,
        true);

    EXPECT_TRUE(snapshot.bound);
    EXPECT_EQ(snapshot.inputBufferAddress,
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(input.data())));
    EXPECT_EQ(snapshot.outputBufferAddress,
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(output.data())));
    EXPECT_EQ(snapshot.inputFrameCapacity, 8U);
    EXPECT_EQ(snapshot.outputFrameCapacity, 8U);
    EXPECT_EQ(snapshot.inputChannels, 2U);
    EXPECT_EQ(snapshot.outputChannels, 2U);
    EXPECT_EQ(snapshot.ioBeginReadCount, 1U);
    EXPECT_EQ(snapshot.ioWriteEndCount, 1U);
    EXPECT_EQ(snapshot.inputBeginReadSampleFrame, 100U);
    EXPECT_EQ(snapshot.inputClientReadEndFrame, 132U);
    EXPECT_EQ(snapshot.outputWriteEndSampleFrame, 200U);
    EXPECT_EQ(snapshot.outputClientWriteEndFrame, 232U);
    EXPECT_EQ(snapshot.inputBeginReadFrameCount, 32U);
    EXPECT_EQ(snapshot.outputWriteEndFrameCount, 32U);
    EXPECT_EQ(snapshot.ioBufferFrameSize, 32U);
    EXPECT_EQ(snapshot.expectedIoBufferFrameSize, 64U);
    EXPECT_EQ(snapshot.lastSampleDelta, 32);
    EXPECT_EQ(snapshot.sampleTimeRegressionCount, 1U);
    EXPECT_EQ(snapshot.ioBufferFrameSizeChangeCount, 2U);
    EXPECT_EQ(snapshot.directTxPackets, 7U);
    EXPECT_EQ(snapshot.directTxUnderruns, 3U);
    EXPECT_EQ(snapshot.directTxSilenceSubstitutions, 2U);
    EXPECT_TRUE(snapshot.outputReaderAvailableAtWriteEnd);
}

TEST(DirectAudioDebugSnapshotTests, ThrottleLogsFirstBoundChangesAndBoundIntervals) {
    DirectAudioDebugLogState state{};
    DirectAudioDebugSnapshot snapshot{};
    snapshot.bound = true;

    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(state, snapshot, 100));
    EXPECT_FALSE(ShouldLogDirectAudioDebugSnapshot(state, snapshot, 101));
    EXPECT_FALSE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + kDirectAudioDebugLogIntervalNs - 1));
    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + kDirectAudioDebugLogIntervalNs));

    snapshot.bound = false;
    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + kDirectAudioDebugLogIntervalNs + 1));
    EXPECT_FALSE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + (2 * kDirectAudioDebugLogIntervalNs) + 1));

    snapshot.bound = true;
    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + (2 * kDirectAudioDebugLogIntervalNs) + 2));
}

} // namespace ASFW::Tests::AudioRuntime
