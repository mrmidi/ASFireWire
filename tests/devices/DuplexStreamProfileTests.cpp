#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DuplexStreamProfile.hpp"

namespace {

using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::Backends::DuplexHostDirection;
using ASFW::Audio::Backends::DuplexStreamProfile;
using ASFW::Audio::Backends::DuplexStreamProfileResolver;
using ASFW::DeviceProfiles::Audio::kAlesisVendorId;
using ASFW::DeviceProfiles::Audio::kFocusriteVendorId;
using ASFW::DeviceProfiles::Audio::kSPro24DspModelId;
using ASFW::Discovery::DeviceRecord;
using ASFW::Encoding::AudioWireFormat;

TEST(DuplexStreamProfileTests, OrdinaryDiceKeepsLegacyChannelsGeometryAndRecipe) {
    DeviceRecord record{};
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 16,
        .hostOutputPcmChannels = 16,
        .deviceToHostAm824Slots = 17,
        .hostToDeviceAm824Slots = 17,
        .sampleRateHz = 48000,
        .deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

    EXPECT_EQ(profile.channels.captureStreamCount, 1U);
    EXPECT_EQ(profile.channels.playbackStreamCount, 1U);
    EXPECT_EQ(profile.channels.deviceToHostIsoChannel, 1U);
    EXPECT_EQ(profile.channels.hostToDeviceIsoChannel, 0U);
    EXPECT_EQ(profile.captureStreams[0].pcmChannels, 0U);
    EXPECT_EQ(profile.captureStreams[0].am824Slots, 17U);
    EXPECT_EQ(profile.captureWireFormat, AudioWireFormat::kAM824);
    EXPECT_EQ(profile.playbackWireFormat, AudioWireFormat::kAM824);
    EXPECT_EQ(profile.playbackBandwidthUnits, 320U);
    EXPECT_EQ(profile.captureBandwidthUnits, 576U);
    EXPECT_EQ(profile.startOrder.postDeviceEnableDelayMs, 2U);
    EXPECT_FALSE(profile.startOrder.startReceiveBeforeDeviceRx);
    EXPECT_FALSE(profile.startOrder.startTransmitBeforeDeviceTx);
    EXPECT_EQ(profile.startOrder.prepareOrder[0], DuplexHostDirection::kReceive);
    EXPECT_EQ(profile.startOrder.prepareOrder[1], DuplexHostDirection::kTransmit);
    EXPECT_EQ(profile.startOrder.startOrder[0], DuplexHostDirection::kReceive);
    EXPECT_EQ(profile.startOrder.startOrder[1], DuplexHostDirection::kTransmit);
}

TEST(DuplexStreamProfileTests, SPro24DspResolvesRawPcmOnBothDirectionsWhenGeometryMatches) {
    DeviceRecord record{
        .vendorId = kFocusriteVendorId,
        .modelId = kSPro24DspModelId,
    };
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 8,
        .hostOutputPcmChannels = 8,
        .deviceToHostAm824Slots = 9,
        .hostToDeviceAm824Slots = 9,
    };

    const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

    EXPECT_EQ(profile.captureWireFormat, AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(profile.playbackWireFormat, AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(profile.captureStreams[0].am824Slots, 9U);
}

TEST(DuplexStreamProfileTests, AlesisModelsClampAdvertisedCaptureStreamsToOne) {
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 32,
        .hostOutputPcmChannels = 32,
        .deviceToHostAm824Slots = 34,
        .hostToDeviceAm824Slots = 34,
        .deviceToHostIsoChannel = 5,
        .hostToDeviceIsoChannel = 8,
        .deviceToHostStreamCount = 2,
        .hostToDeviceStreamCount = 2,
    };
    caps.deviceToHostStreams[0] = {.isoChannel = 5, .pcmChannels = 16, .am824Slots = 17};
    caps.deviceToHostStreams[1] = {.isoChannel = 6, .pcmChannels = 16, .am824Slots = 17};

    for (const uint32_t modelId : {0x000000U, 0x000001U}) {
        DeviceRecord record{
            .vendorId = kAlesisVendorId,
            .modelId = modelId,
        };
        const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

        EXPECT_EQ(profile.channels.captureStreamCount, 1U) << modelId;
        EXPECT_EQ(profile.channels.playbackStreamCount, 2U) << modelId;
        EXPECT_EQ(profile.captureStreams[0].isoChannel, 5U) << modelId;
        EXPECT_EQ(profile.captureStreams[0].pcmChannels, 0U) << modelId;
        EXPECT_EQ(profile.captureStreams[0].am824Slots, 34U) << modelId;
        EXPECT_EQ(profile.channels.PlaybackChannel(1), 0U) << modelId;
    }
}

} // namespace
