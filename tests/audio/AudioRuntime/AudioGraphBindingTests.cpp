#include <AudioDriverKit/AudioDriverKit.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioStreamMode;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::AudioWireFormat;

AudioGraphBinding MakeBinding(AudioTransportControlBlock* control,
                              IOUserAudioDevice* audioDevice,
                              float* input,
                              const float* output) {
    return AudioGraphBinding{
        .guid = 0x1122334455667788ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .inputBase = input,
            .outputBase = output,
            .inputFrameCapacity = input ? 8U : 0U,
            .outputFrameCapacity = output ? 8U : 0U,
            .inputChannels = input ? 2U : 0U,
            .outputChannels = output ? 2U : 0U,
        },
        .control = control,
        .deviceToHostAm824Slots = input ? 2U : 0U,
        .hostToDeviceAm824Slots = output ? 2U : 0U,
        .streamMode = AudioStreamMode::kBlocking,
        .hostToDeviceWireFormat = AudioWireFormat::kAM824,
        .audioDevice = audioDevice,
    };
}

TEST(AudioGraphBindingTests, InvalidWithoutGuid) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};

    auto binding = MakeBinding(&control, &audioDevice, input.data(), nullptr);
    binding.guid = 0;

    EXPECT_FALSE(binding.IsValid());
}

TEST(AudioGraphBindingTests, InvalidWithoutSampleRate) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};

    auto binding = MakeBinding(&control, &audioDevice, input.data(), nullptr);
    binding.sampleRateHz = 0;

    EXPECT_FALSE(binding.IsValid());
}

TEST(AudioGraphBindingTests, InvalidWithoutControl) {
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};

    const auto binding = MakeBinding(nullptr, &audioDevice, input.data(), nullptr);

    EXPECT_FALSE(binding.IsValid());
}

TEST(AudioGraphBindingTests, InvalidWithoutAudioDevice) {
    AudioTransportControlBlock control{};
    std::array<float, 16> input{};

    const auto binding = MakeBinding(&control, nullptr, input.data(), nullptr);

    EXPECT_FALSE(binding.IsValid());
}

TEST(AudioGraphBindingTests, ValidWithInputOnly) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};

    const auto binding = MakeBinding(&control, &audioDevice, input.data(), nullptr);

    EXPECT_TRUE(binding.IsValid());
    EXPECT_TRUE(binding.HasInput());
    EXPECT_FALSE(binding.HasOutput());
}

TEST(AudioGraphBindingTests, ValidWithOutputOnly) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> output{};

    const auto binding = MakeBinding(&control, &audioDevice, nullptr, output.data());

    EXPECT_TRUE(binding.IsValid());
    EXPECT_FALSE(binding.HasInput());
    EXPECT_TRUE(binding.HasOutput());
}

TEST(AudioGraphBindingTests, ValidWithDuplex) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<float, 16> input{};
    std::array<float, 16> output{};

    const auto binding = MakeBinding(&control, &audioDevice, input.data(), output.data());

    EXPECT_TRUE(binding.IsValid());
    EXPECT_TRUE(binding.HasInput());
    EXPECT_TRUE(binding.HasOutput());
}

} // namespace
