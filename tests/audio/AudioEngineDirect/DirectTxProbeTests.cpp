#include <AudioDriverKit/AudioDriverKit.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "AudioEngine/Direct/DirectOutputReader.hpp"
#include "AudioEngine/Direct/Tx/DirectTxProbe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace ASFW::Tests::AudioEngineDirect {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioStreamMode;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::AudioWireFormat;
using ASFW::AudioEngine::Direct::DirectOutputReader;
using ASFW::AudioEngine::Direct::Tx::DirectTxProbe;
using ASFW::AudioEngine::Direct::Tx::DirectTxReadRequest;
using ASFW::AudioEngine::Direct::Tx::DirectTxReadStatus;

AudioGraphBinding MakeOutputBinding(AudioTransportControlBlock& control,
                                    IOUserAudioDevice& audioDevice,
                                    const int32_t* output,
                                    uint32_t frameCapacity,
                                    uint32_t channels) {
    return AudioGraphBinding{
        .guid = 0x1122334455667788ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .outputBase = output,
            .outputFrameCapacity = frameCapacity,
            .outputChannels = channels,
        },
        .control = &control,
        .hostToDeviceAm824Slots = channels,
        .streamMode = AudioStreamMode::kBlocking,
        .hostToDeviceWireFormat = AudioWireFormat::kAM824,
        .audioDevice = &audioDevice,
    };
}

static void PublishPlaybackWriteEnd(AudioTransportControlBlock& control,
                                    uint64_t sampleFrame,
                                    uint64_t hostTime,
                                    uint32_t frameCount) {
    control.client.PublishWriteEnd(sampleFrame, hostTime, frameCount);
    control.playbackRingOldestValidFrame.store(sampleFrame,
                                               std::memory_order_relaxed);
    control.playbackRingWriteFrame.store(control.client.OutputWrittenEndFrame(),
                                         std::memory_order_release);
}

TEST(DirectTxProbeTests, InvalidReaderReturnsInvalidBinding) {
    DirectOutputReader reader{};
    DirectTxProbe probe(reader);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 0,
        .frameCount = 1,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kInvalidBinding);
}

TEST(DirectTxProbeTests, ZeroFrameCountReturnsInvalidRange) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 0,
        .frameCount = 0,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kInvalidRange);
}

TEST(DirectTxProbeTests, ZeroChannelsReturnsInvalidRange) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 0,
        .frameCount = 1,
        .channels = 0,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kInvalidRange);
}

TEST(DirectTxProbeTests, ChannelMismatchReturnsInvalidRange) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 0,
        .frameCount = 1,
        .channels = 4,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kInvalidRange);
}

TEST(DirectTxProbeTests, UnwrittenRangeReturnsUnderrun) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    PublishPlaybackWriteEnd(control, 100, 0, 7);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 100,
        .frameCount = 8,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kUnderrun);
    EXPECT_EQ(result.writtenEndFrame, 107U);
    EXPECT_EQ(result.requestedEndFrame, 108U);
    EXPECT_EQ(result.firstFramePtr, nullptr);
}

TEST(DirectTxProbeTests, ExactWrittenEndIsAvailable) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    PublishPlaybackWriteEnd(control, 100, 0, 8);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 100,
        .frameCount = 8,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kAvailable);
    EXPECT_EQ(result.writtenEndFrame, 108U);
    EXPECT_EQ(result.requestedEndFrame, 108U);
    EXPECT_EQ(result.firstFramePtr, output.data() + ((100U % 8U) * 2U));
}

TEST(DirectTxProbeTests, WrittenBeyondRequestedEndIsAvailable) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    PublishPlaybackWriteEnd(control, 100, 0, 16);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 100,
        .frameCount = 8,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kAvailable);
    EXPECT_EQ(result.firstFramePtr, output.data() + ((100U % 8U) * 2U));
}

TEST(DirectTxProbeTests, ExactOldestBoundaryIsAvailable) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    PublishPlaybackWriteEnd(control, 100, 0, 16);
    control.playbackRingOldestValidFrame.store(108, std::memory_order_release);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 108,
        .frameCount = 8,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kAvailable);
    EXPECT_EQ(result.oldestValidFrame, 108U);
    EXPECT_EQ(result.requestedEndFrame, 116U);
}

TEST(DirectTxProbeTests, FrameBeforeOldestBoundaryIsStaleOverwritten) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    PublishPlaybackWriteEnd(control, 100, 0, 16);
    control.playbackRingOldestValidFrame.store(108, std::memory_order_release);

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = 107,
        .frameCount = 1,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kStaleOverwritten);
    EXPECT_EQ(result.oldestValidFrame, 108U);
    EXPECT_EQ(result.firstFramePtr, nullptr);
}

TEST(DirectTxProbeTests, OverflowingRangeReturnsInvalidRange) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    DirectTxProbe probe(reader);

    constexpr uint64_t kMaxFrame = std::numeric_limits<uint64_t>::max();

    const auto result = probe.Probe(DirectTxReadRequest{
        .firstFrame = kMaxFrame - 1,
        .frameCount = 4,
        .channels = 2,
    });

    EXPECT_EQ(result.status, DirectTxReadStatus::kInvalidRange);
}

} // namespace ASFW::Tests::AudioEngineDirect
