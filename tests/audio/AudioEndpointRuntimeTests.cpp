#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Config/AudioConstants.hpp"

namespace {

ASFW::Audio::Model::ASFWAudioDevice MakeDeviceConfig() {
    ASFW::Audio::Model::ASFWAudioDevice config{};
    config.guid = 0x1020304050607080ULL;
    config.channelCount = 8;
    config.inputChannelCount = 6;
    config.outputChannelCount = 4;
    config.currentSampleRate = 48000;
    return config;
}

} // namespace

TEST(AudioEndpointRuntime, MissingConfigDoesNotPublishBinding) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    EXPECT_FALSE(runtime.CopyDirectAudioBinding(snapshot));
    EXPECT_FALSE(snapshot.valid);
}

TEST(AudioEndpointRuntime, BadCopyArgsZeroOutputs) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    runtime.UpdateConfig(MakeDeviceConfig());

    IOMemoryDescriptor* inputMemory = reinterpret_cast<IOMemoryDescriptor*>(0x1);
    IOMemoryDescriptor* controlMemory = reinterpret_cast<IOMemoryDescriptor*>(0x2);
    uint32_t outputFrames = 99;
    uint32_t outputChannels = 98;
    uint32_t inputFrames = 97;
    uint32_t inputChannels = 96;
    uint32_t sampleRateHz = 95;
    uint64_t generation = 94;

    const kern_return_t kr = runtime.CopyDirectAudioMemory(nullptr,
                                                           &inputMemory,
                                                           &controlMemory,
                                                           &outputFrames,
                                                           &outputChannels,
                                                           &inputFrames,
                                                           &inputChannels,
                                                           &sampleRateHz,
                                                           &generation);

    EXPECT_EQ(kr, kIOReturnBadArgument);
    EXPECT_EQ(inputMemory, nullptr);
    EXPECT_EQ(controlMemory, nullptr);
    EXPECT_EQ(outputFrames, 0u);
    EXPECT_EQ(outputChannels, 0u);
    EXPECT_EQ(inputFrames, 0u);
    EXPECT_EQ(inputChannels, 0u);
    EXPECT_EQ(sampleRateHz, 0u);
    EXPECT_EQ(generation, 0u);
}

TEST(AudioEndpointRuntime, CopyDirectAudioMemoryAllocatesCompleteDuplexBinding) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    runtime.UpdateConfig(MakeDeviceConfig());

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;

    const kern_return_t kr = runtime.CopyDirectAudioMemory(&outputMemory,
                                                           &inputMemory,
                                                           &controlMemory,
                                                           &outputFrames,
                                                           &outputChannels,
                                                           &inputFrames,
                                                           &inputChannels,
                                                           &sampleRateHz,
                                                           &generation);

    ASSERT_EQ(kr, kIOReturnSuccess);
    ASSERT_NE(outputMemory, nullptr);
    ASSERT_NE(inputMemory, nullptr);
    ASSERT_NE(controlMemory, nullptr);
    // V3: the published frames are the ACTIVE ring of the rate (the ZTS
    // period); the memory itself is allocated once at the maximum ring.
    const uint32_t activeRing =
        ASFW::IsochTransport::HalBufferProfileForRate(48000).frameRingFrames;
    EXPECT_EQ(outputFrames, activeRing);
    EXPECT_EQ(inputFrames, activeRing);
    uint64_t outputBytes = 0;
    uint64_t inputBytes = 0;
    ASSERT_EQ(outputMemory->GetLength(&outputBytes), kIOReturnSuccess);
    ASSERT_EQ(inputMemory->GetLength(&inputBytes), kIOReturnSuccess);
    EXPECT_EQ(outputBytes,
              uint64_t{ASFW::Isoch::Config::kAudioOutputRingFrames} * 4u * sizeof(float));
    EXPECT_EQ(inputBytes,
              uint64_t{ASFW::Isoch::Config::kAudioRingBufferFrames} * 6u * sizeof(int32_t));
    EXPECT_EQ(outputChannels, 4u);
    EXPECT_EQ(inputChannels, 6u);
    EXPECT_EQ(sampleRateHz, 48000u);
    EXPECT_GT(generation, 0u);

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(snapshot));
    EXPECT_TRUE(snapshot.IsValidDuplex());
    EXPECT_EQ(snapshot.outputFrames, outputFrames);
    EXPECT_EQ(snapshot.outputChannels, outputChannels);
    EXPECT_EQ(snapshot.inputFrames, inputFrames);
    EXPECT_EQ(snapshot.inputChannels, inputChannels);
    EXPECT_EQ(snapshot.sampleRateHz, sampleRateHz);
    EXPECT_EQ(snapshot.generation, generation);

    outputMemory->release();
    inputMemory->release();
    controlMemory->release();
}

TEST(AudioEndpointRuntime, PlaybackOnlyPresentationKeepsPhysicalReturnRingForDuplexTransport) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    auto config = MakeDeviceConfig();
    config.channelCount = 2;
    config.inputChannelCount = 0;  // CoreAudio-visible topology.
    config.outputChannelCount = 2;
    runtime.UpdateConfig(config);

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;

    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory,
                                            &inputMemory,
                                            &controlMemory,
                                            &outputFrames,
                                            &outputChannels,
                                            &inputFrames,
                                            &inputChannels,
                                            &sampleRateHz,
                                            &generation),
              kIOReturnSuccess);
    EXPECT_EQ(outputChannels, 2U);
    EXPECT_EQ(inputChannels, 2U);

    outputMemory->release();
    inputMemory->release();
    controlMemory->release();
}

// A rate change moves the active ring inside the unchanged allocation: the same
// descriptors are republished with the new ring and a bumped generation, so no
// memory is replaced under CoreAudio (V3, TIMING_GEOMETRY_OWNERSHIP.md).
TEST(AudioEndpointRuntime, RateChangeMovesActiveRingAndReusesMemory) {
    ASFW::Audio::AudioEndpointRuntime runtime(0x1020304050607080ULL);
    runtime.UpdateConfig(MakeDeviceConfig());

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory, &inputMemory, &controlMemory,
                                            &outputFrames, &outputChannels, &inputFrames,
                                            &inputChannels, &sampleRateHz, &generation),
              kIOReturnSuccess);
    EXPECT_EQ(outputFrames, 12288u);

    runtime.SetCurrentSampleRate(96000);

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot at96{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(at96));
    EXPECT_EQ(at96.sampleRateHz, 96000u);
    EXPECT_EQ(at96.outputFrames, 24576u);
    EXPECT_EQ(at96.inputFrames, 24576u);
    EXPECT_GT(at96.generation, generation);

    IOMemoryDescriptor* outputMemory2 = nullptr;
    IOMemoryDescriptor* inputMemory2 = nullptr;
    IOMemoryDescriptor* controlMemory2 = nullptr;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory2, &inputMemory2, &controlMemory2,
                                            &outputFrames, &outputChannels, &inputFrames,
                                            &inputChannels, &sampleRateHz, &generation),
              kIOReturnSuccess);
    EXPECT_EQ(outputMemory2, outputMemory);
    EXPECT_EQ(inputMemory2, inputMemory);
    EXPECT_EQ(outputFrames, 24576u);
    EXPECT_EQ(sampleRateHz, 96000u);

    // Back to 48 kHz: the ring shrinks again inside the same allocation.
    runtime.SetCurrentSampleRate(48000);
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot at48{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(at48));
    EXPECT_EQ(at48.outputFrames, 12288u);
    EXPECT_GT(at48.generation, at96.generation);

    for (IOMemoryDescriptor* memory : {outputMemory, inputMemory, controlMemory, outputMemory2,
                                       inputMemory2, controlMemory2}) {
        memory->release();
    }
}
