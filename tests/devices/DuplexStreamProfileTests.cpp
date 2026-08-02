#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DuplexStreamProfile.hpp"

namespace {

using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::Backends::DuplexHostDirection;
using ASFW::Audio::Backends::DuplexStreamProfile;
using ASFW::Audio::Backends::DuplexStreamProfileResolver;
using ASFW::DeviceProfiles::Audio::kAlesisVendorId;
using ASFW::DeviceProfiles::Audio::kApogeeDuetModelId;
using ASFW::DeviceProfiles::Audio::kApogeeVendorId;
using ASFW::DeviceProfiles::Audio::kFocusriteVendorId;
using ASFW::DeviceProfiles::Audio::kSPro24DspModelId;
using ASFW::DeviceProfiles::Audio::kTerraTecVendorId;
using ASFW::DeviceProfiles::Audio::kPhase88RackFwModelId;
using ASFW::Discovery::DeviceRecord;
using ASFW::Encoding::AudioWireFormat;

TEST(DuplexStreamProfileTests, OrdinaryDiceKeepsLegacyChannelsGeometryAndRecipe) {
    DeviceRecord record{};
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
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
    EXPECT_EQ(profile.playbackStreams[0].bandwidthUnits, 1076U);
    EXPECT_EQ(profile.captureStreams[0].bandwidthUnits, 1076U);
    EXPECT_EQ(profile.playbackStreams[0].allowedIsoChannels, uint64_t{1} << 0U);
    EXPECT_EQ(profile.captureStreams[0].allowedIsoChannels, uint64_t{1} << 1U);
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

TEST(DuplexStreamProfileTests, ApogeeDuetAllowsDynamicChannelsAndPreservesCmpInterleave) {
    DeviceRecord record{
        .vendorId = kApogeeVendorId,
        .modelId = kApogeeDuetModelId,
    };
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 2,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 2,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 48000,
    };

    const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

    EXPECT_EQ(profile.captureStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(profile.playbackStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(profile.captureStreams[0].bandwidthUnits, 596U);
    EXPECT_EQ(profile.playbackStreams[0].bandwidthUnits, 596U);
    EXPECT_TRUE(profile.startOrder.startReceiveBeforeDeviceRx);
    EXPECT_TRUE(profile.startOrder.startTransmitBeforeDeviceTx);
    EXPECT_EQ(profile.startOrder.postDeviceEnableDelayMs, 0U);
    EXPECT_TRUE(profile.stopOrder
                    .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive);
}

TEST(DuplexStreamProfileTests, Phase88PreservesLinuxBeBoBCmpBeforeHostStartOrdering) {
    DeviceRecord record{
        .vendorId = kTerraTecVendorId,
        .modelId = kPhase88RackFwModelId,
    };
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 10,
        .hostOutputPcmChannels = 10,
        .deviceToHostAm824Slots = 11,
        .hostToDeviceAm824Slots = 11,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

    // Linux establishes iPCR then oPCR before amdtp_domain_start(), which
    // starts the device-to-host RX path before host-to-device TX.
    EXPECT_EQ(profile.captureStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(profile.playbackStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(profile.captureStreams[0].am824Slots, 11U);
    EXPECT_EQ(profile.playbackStreams[0].am824Slots, 11U);
    EXPECT_FALSE(profile.startOrder.startReceiveBeforeDeviceRx);
    EXPECT_FALSE(profile.startOrder.startTransmitBeforeDeviceTx);
    EXPECT_FALSE(profile.startOrder.requiresPreStreamClockLock);
    EXPECT_EQ(profile.startOrder.startOrder[0], DuplexHostDirection::kReceive);
    EXPECT_EQ(profile.startOrder.startOrder[1], DuplexHostDirection::kTransmit);
    EXPECT_EQ(profile.startOrder.postDeviceEnableDelayMs, 0U);
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
