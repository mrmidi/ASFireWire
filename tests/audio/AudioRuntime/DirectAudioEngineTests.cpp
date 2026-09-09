#include <AudioDriverKit/AudioDriverKit.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/Engine/Direct/FireWireAudioEngine.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioStreamMode;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::AudioWireFormat;
using ASFW::AudioEngine::Direct::FireWireAudioEngine;

AudioGraphBinding MakeDuplexBinding(AudioTransportControlBlock& control,
                                    IOUserAudioDevice& audioDevice,
                                    float* input,
                                    const float* output) {
    return AudioGraphBinding{
        .endpointId = ASFW::Audio::Devices::AudioEndpointId{1},
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .inputBase = input,
            .outputBase = output,
            .activeInputRingFrames = 8,
            .activeOutputRingFrames = 8,
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

TEST(DirectAudioEngineTests, BindValidGraphBindsSubcomponents) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};
    std::array<float, 16> output{};
    FireWireAudioEngine engine{};

    const auto binding = MakeDuplexBinding(control, audioDevice, input.data(), output.data());

    EXPECT_TRUE(engine.Bind(binding));
    EXPECT_TRUE(engine.IsBound());
    EXPECT_TRUE(engine.InputWriter().IsBound());
    EXPECT_EQ(engine.InputWriter().Frame(1), input.data() + 2);
}

TEST(DirectAudioEngineTests, BindInvalidGraphClearsState) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};
    std::array<float, 16> output{};
    FireWireAudioEngine engine{};

    const auto valid = MakeDuplexBinding(control, audioDevice, input.data(), output.data());
    ASSERT_TRUE(engine.Bind(valid));

    AudioGraphBinding invalid = valid;
    invalid.endpointId = {};

    EXPECT_FALSE(engine.Bind(invalid));
    EXPECT_FALSE(engine.IsBound());
    EXPECT_FALSE(engine.InputWriter().IsBound());
}

TEST(DirectAudioEngineTests, UnbindClearsState) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};
    std::array<float, 16> output{};
    FireWireAudioEngine engine{};

    const auto binding = MakeDuplexBinding(control, audioDevice, input.data(), output.data());
    ASSERT_TRUE(engine.Bind(binding));

    engine.Unbind();

    EXPECT_FALSE(engine.IsBound());
    EXPECT_FALSE(engine.InputWriter().IsBound());
}

} // namespace
