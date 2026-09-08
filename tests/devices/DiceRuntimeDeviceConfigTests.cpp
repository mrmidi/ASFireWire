#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DiceRuntimeDeviceConfig.hpp"

namespace {

using ASFW::Audio::ApplyDiceRuntimeCapsToDeviceConfig;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::DicePublicationConfigResult;
using ASFW::Audio::Model::ASFWAudioDevice;
using ASFW::Audio::PrepareDiceDeviceConfigForPublication;
namespace DeviceIds = ASFW::DeviceProfiles::Audio;

TEST(DiceRuntimeDeviceConfigTests, AppliesDiscoveredPcmGeometryWithoutChannelTable) {
    ASFWAudioDevice config{};
    config.deviceName = "Generic DICE";
    // Seeded by the caller before runtime caps arrive: the profile's supported
    // rate set, and the 48 kHz DICE default that bring-up programs.
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;

    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 16,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 16,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 2,
        .hostToDeviceStreamCount = 1,
    };

    ASSERT_TRUE(ApplyDiceRuntimeCapsToDeviceConfig(caps, config));
    EXPECT_EQ(config.inputChannelCount, 16U);
    EXPECT_EQ(config.outputChannelCount, 2U);
    EXPECT_EQ(config.channelCount, 16U);
    EXPECT_EQ(config.deviceName, "Generic DICE");

    // The clock is not adopted from the device: the advertised set survives and
    // the default stays at what bring-up will actually program.
    EXPECT_EQ(config.currentSampleRate, 48000U);
    ASSERT_EQ(config.sampleRates.size(), 2U);
    EXPECT_EQ(config.sampleRates[0], 44100U);
    EXPECT_EQ(config.sampleRates[1], 48000U);
}

TEST(DiceRuntimeDeviceConfigTests, DeviceFoundAtNon48kKeepsTheAdvertisedRateSet) {
    // The regression: a device discovered at 44.1 kHz used to be published as a
    // 44.1-kHz-only device -- every other rate refused by ASFWAudioDevice's
    // validation -- while bring-up drove it at 48 kHz. Both halves must hold.
    ASFWAudioDevice config{};
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;

    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 14,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 14,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 44100,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    ASSERT_TRUE(ApplyDiceRuntimeCapsToDeviceConfig(caps, config));

    // Geometry still comes from the device.
    EXPECT_EQ(config.inputChannelCount, 14U);
    EXPECT_EQ(config.outputChannelCount, 2U);

    // The clock does not.
    EXPECT_EQ(config.currentSampleRate, 48000U);
    ASSERT_EQ(config.sampleRates.size(), 2U);
    EXPECT_EQ(config.sampleRates[0], 44100U);
    EXPECT_EQ(config.sampleRates[1], 48000U);
}

TEST(DiceRuntimeDeviceConfigTests, RejectsPartialCapsWithoutChangingFallbackConfig) {
    ASFWAudioDevice config{};
    const ASFWAudioDevice before = config;
    const AudioStreamRuntimeCaps partial{
        .hostInputPcmChannels = 16,
        .hostOutputPcmChannels = 0,
        .sampleRateHz = 48000,
    };

    EXPECT_FALSE(ApplyDiceRuntimeCapsToDeviceConfig(partial, config));
    EXPECT_EQ(config.inputChannelCount, before.inputChannelCount);
    EXPECT_EQ(config.outputChannelCount, before.outputChannelCount);
    EXPECT_EQ(config.channelCount, before.channelCount);
    EXPECT_EQ(config.currentSampleRate, before.currentSampleRate);
    EXPECT_EQ(config.sampleRates, before.sampleRates);
}

TEST(DiceRuntimeDeviceConfigTests, ProjectDefersMissingWireGeometryWithoutMutatingCandidateConfig) {
    for (uint32_t failure = 0; failure < 7; ++failure) {
        SCOPED_TRACE(failure);
        ASFWAudioDevice config{};
        config.vendorId = DeviceIds::kPreSonusVendorId;
        config.modelId = DeviceIds::kFireStudioProjectModelId;
        config.inputChannelCount = 16;
        config.outputChannelCount = 8;
        config.channelCount = 16;
        config.currentSampleRate = 48000;
        config.sampleRates = {48000};
        const ASFWAudioDevice before = config;
        AudioStreamRuntimeCaps caps{
            .hostInputPcmChannels = 10,
            .hostOutputPcmChannels = 10,
            .deviceToHostAm824Slots = 11,
            .hostToDeviceAm824Slots = 11,
            .sampleRateHz = 48000,
            .deviceToHostStreamCount = 1,
            .hostToDeviceStreamCount = 1,
        };
        if (failure == 0) caps.sampleRateHz = 0;
        if (failure == 1) caps.deviceToHostAm824Slots = 0;
        if (failure == 2) caps.hostToDeviceAm824Slots = 0;
        if (failure == 3) caps.deviceToHostStreamCount = 0;
        if (failure == 4) caps.hostToDeviceStreamCount = 0;
        if (failure == 5) caps.deviceToHostStreamCount = 5;
        if (failure == 6) caps.hostToDeviceStreamCount = 5;

        EXPECT_EQ(PrepareDiceDeviceConfigForPublication(&caps, true, config),
                  DicePublicationConfigResult::kDefer);
        EXPECT_EQ(config.inputChannelCount, before.inputChannelCount);
        EXPECT_EQ(config.outputChannelCount, before.outputChannelCount);
        EXPECT_EQ(config.channelCount, before.channelCount);
        EXPECT_EQ(config.currentSampleRate, before.currentSampleRate);
        EXPECT_EQ(config.sampleRates, before.sampleRates);
    }
}

TEST(DiceRuntimeDeviceConfigTests, PublicationFailurePolicyIsScopedToExactProjectIdentity) {
    const struct {
        const char* name;
        uint32_t vendor;
        uint32_t model;
        bool isProject;
    } devices[] = {
        {"FireStudio Project", DeviceIds::kPreSonusVendorId,
         DeviceIds::kFireStudioProjectModelId, true},
        {"StudioLive sibling", DeviceIds::kPreSonusVendorId,
         DeviceIds::kStudioLive1602ModelId, false},
        {"Other PreSonus model", DeviceIds::kPreSonusVendorId, 0x000008, false},
        {"Same model ID from another vendor", DeviceIds::kWeissVendorId,
         DeviceIds::kFireStudioProjectModelId, false},
        {"Focusrite", DeviceIds::kFocusriteVendorId, DeviceIds::kSPro40ModelId, false},
    };
    const struct {
        const char* name;
        bool geometryReadSucceeded;
        bool providePartialCaps;
    } failures[] = {
        {"No protocol or geometry read failed", false, false},
        {"Read succeeded but cached caps unavailable", true, false},
        {"Read failed with incomplete caps", false, true},
        {"Read succeeded with incomplete caps", true, true},
    };
    const AudioStreamRuntimeCaps partial{
        .hostInputPcmChannels = 10,
        .hostOutputPcmChannels = 0,
        .sampleRateHz = 44100,
    };

    for (const auto& device : devices) {
        for (const auto& failure : failures) {
            SCOPED_TRACE(device.name);
            SCOPED_TRACE(failure.name);
            ASFWAudioDevice config{};
            config.vendorId = device.vendor;
            config.modelId = device.model;
            config.inputChannelCount = 2;
            config.outputChannelCount = 2;
            config.channelCount = 2;
            config.currentSampleRate = 48000;
            config.sampleRates = {44100, 48000};
            const ASFWAudioDevice before = config;

            EXPECT_EQ(PrepareDiceDeviceConfigForPublication(
                          failure.providePartialCaps ? &partial : nullptr,
                          failure.geometryReadSucceeded, config),
                      device.isProject ? DicePublicationConfigResult::kDefer
                                       : DicePublicationConfigResult::kProfileFallback);
            EXPECT_EQ(config.inputChannelCount, before.inputChannelCount);
            EXPECT_EQ(config.outputChannelCount, before.outputChannelCount);
            EXPECT_EQ(config.channelCount, before.channelCount);
            EXPECT_EQ(config.currentSampleRate, before.currentSampleRate);
            EXPECT_EQ(config.sampleRates, before.sampleRates);
        }
    }
}

TEST(DiceRuntimeDeviceConfigTests, ProjectPublishesDiscoveredGeometryAtBothSupportedRates) {
    for (const uint32_t rate : {44100U, 48000U}) {
        SCOPED_TRACE(rate);
        ASFWAudioDevice config{};
        config.vendorId = DeviceIds::kPreSonusVendorId;
        config.modelId = DeviceIds::kFireStudioProjectModelId;
        config.inputChannelCount = 2;
        config.outputChannelCount = 2;
        config.sampleRates = {44100U, 48000U};
        config.currentSampleRate = 48000U;
        const AudioStreamRuntimeCaps caps{
            .hostInputPcmChannels = 10,
            .hostOutputPcmChannels = 10,
            .deviceToHostAm824Slots = 11,
            .hostToDeviceAm824Slots = 11,
            .sampleRateHz = rate,
            .deviceToHostStreamCount = 1,
            .hostToDeviceStreamCount = 1,
        };

        EXPECT_EQ(PrepareDiceDeviceConfigForPublication(&caps, true, config),
                  DicePublicationConfigResult::kRuntimeGeometry);
        EXPECT_EQ(config.inputChannelCount, 10U);
        EXPECT_EQ(config.outputChannelCount, 10U);
        EXPECT_EQ(config.channelCount, 10U);
        EXPECT_EQ(config.currentSampleRate, 48000U);
        EXPECT_EQ(config.sampleRates, (std::vector<uint32_t>{44100U, 48000U}));
    }
}

TEST(DiceRuntimeDeviceConfigTests, LegacyPublicationStillEnrichesFromAvailableCapsAfterReadFailure) {
    ASFWAudioDevice config{};
    config.vendorId = DeviceIds::kFocusriteVendorId;
    config.modelId = DeviceIds::kSPro40ModelId;
    config.inputChannelCount = 2;
    config.outputChannelCount = 2;
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;
    // The established enrichment path accepts HAL-facing channel counts
    // without requiring the additional Project-only wire geometry fields.
    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 16,
        .hostOutputPcmChannels = 8,
        .sampleRateHz = 48000,
    };

    EXPECT_EQ(PrepareDiceDeviceConfigForPublication(&caps, false, config),
              DicePublicationConfigResult::kRuntimeGeometry);
    EXPECT_EQ(config.inputChannelCount, 16U);
    EXPECT_EQ(config.outputChannelCount, 8U);
    EXPECT_EQ(config.channelCount, 16U);
}

TEST(DiceRuntimeDeviceConfigTests, ProjectDoesNotPublishAfterFailedGeometryReadEvenWithCaps) {
    ASFWAudioDevice config{};
    config.vendorId = DeviceIds::kPreSonusVendorId;
    config.modelId = DeviceIds::kFireStudioProjectModelId;
    config.inputChannelCount = 2;
    config.outputChannelCount = 2;
    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 10,
        .hostOutputPcmChannels = 10,
        .deviceToHostAm824Slots = 11,
        .hostToDeviceAm824Slots = 11,
        .sampleRateHz = 44100,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    EXPECT_EQ(PrepareDiceDeviceConfigForPublication(&caps, false, config),
              DicePublicationConfigResult::kDefer);
    EXPECT_EQ(config.inputChannelCount, 2U);
    EXPECT_EQ(config.outputChannelCount, 2U);
}

TEST(DiceRuntimeDeviceConfigTests, AppliesPlaybackOnlyCoreAudioGeometryWithDuplexWireCaps) {
    ASFWAudioDevice config{};
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;
    const AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 0,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 2,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    ASSERT_TRUE(ApplyDiceRuntimeCapsToDeviceConfig(caps, config));
    EXPECT_EQ(config.inputChannelCount, 0U);
    EXPECT_EQ(config.outputChannelCount, 2U);
    EXPECT_EQ(config.channelCount, 2U);
}

} // namespace
