#include <AudioDriverKit/AudioDriverKit.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "AudioEngine/Direct/FireWireAudioEngine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioStreamMode;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::AudioWireFormat;
using ASFW::AudioEngine::Direct::FireWireAudioEngine;

AudioGraphBinding MakeDuplexBinding(AudioTransportControlBlock& control,
                                    IOUserAudioDevice& audioDevice,
                                    int32_t* input,
                                    const int32_t* output) {
    return AudioGraphBinding{
        .guid = 0x1122334455667788ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .inputBase = input,
            .outputBase = output,
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
}

void PublishPlaybackWriteEnd(AudioTransportControlBlock& control,
                             uint64_t sampleFrame,
                             uint64_t hostTime,
                             uint32_t frameCount) {
    control.client.PublishWriteEnd(sampleFrame, hostTime, frameCount);
    control.playbackRingWriteFrame.store(control.client.OutputWrittenEndFrame(),
                                         std::memory_order_release);
}

TEST(DirectAudioEngineTests, BindValidGraphBindsSubcomponents) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> input{};
    std::array<int32_t, 16> output{};
    FireWireAudioEngine engine{};

    const auto binding = MakeDuplexBinding(control, audioDevice, input.data(), output.data());

    EXPECT_TRUE(engine.Bind(binding));
    EXPECT_TRUE(engine.IsBound());
    EXPECT_TRUE(engine.InputWriter().IsBound());
    EXPECT_TRUE(engine.OutputReader().IsBound());
    EXPECT_EQ(engine.InputWriter().Frame(1), input.data() + 2);
    EXPECT_EQ(engine.OutputReader().Frame(1), output.data() + 2);
}

TEST(DirectAudioEngineTests, BindInvalidGraphClearsState) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> input{};
    std::array<int32_t, 16> output{};
    FireWireAudioEngine engine{};

    const auto valid = MakeDuplexBinding(control, audioDevice, input.data(), output.data());
    ASSERT_TRUE(engine.Bind(valid));

    AudioGraphBinding invalid = valid;
    invalid.guid = 0;

    EXPECT_FALSE(engine.Bind(invalid));
    EXPECT_FALSE(engine.IsBound());
    EXPECT_FALSE(engine.InputWriter().IsBound());
    EXPECT_FALSE(engine.OutputReader().IsBound());
}

TEST(DirectAudioEngineTests, UnbindClearsState) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> input{};
    std::array<int32_t, 16> output{};
    FireWireAudioEngine engine{};

    const auto binding = MakeDuplexBinding(control, audioDevice, input.data(), output.data());
    ASSERT_TRUE(engine.Bind(binding));

    engine.Unbind();

    EXPECT_FALSE(engine.IsBound());
    EXPECT_FALSE(engine.InputWriter().IsBound());
    EXPECT_FALSE(engine.OutputReader().IsBound());
}

TEST(DirectAudioEngineTests, OutputReaderUsesPlaybackRingCursorAvailability) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> input{};
    std::array<int32_t, 16> output{};
    FireWireAudioEngine engine{};

    const auto binding = MakeDuplexBinding(control, audioDevice, input.data(), output.data());
    ASSERT_TRUE(engine.Bind(binding));

    EXPECT_FALSE(engine.OutputReader().IsFrameRangeAvailable(1000, 128));

    PublishPlaybackWriteEnd(control, 1000, 55, 128);

    EXPECT_TRUE(engine.OutputReader().IsFrameRangeAvailable(1000, 128));
    EXPECT_FALSE(engine.OutputReader().IsFrameRangeAvailable(1100, 64));
}

TEST(DirectAudioEngineTests, OutputReaderRejectsOverflowingFrameRange) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> input{};
    std::array<int32_t, 16> output{};
    FireWireAudioEngine engine{};

    const auto binding = MakeDuplexBinding(control, audioDevice, input.data(), output.data());
    ASSERT_TRUE(engine.Bind(binding));

    constexpr uint64_t kMaxFrame = std::numeric_limits<uint64_t>::max();

    EXPECT_FALSE(engine.OutputReader().IsFrameRangeAvailable(kMaxFrame - 1, 4));
}

} // namespace
