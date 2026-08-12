#include <gtest/gtest.h>

#include "Audio/Duplex/DuplexStreamPlanner.hpp"

namespace {

using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::Devices::ResolvedAudioEndpointProfile;
using ASFW::Audio::Duplex::HostDirection;
using ASFW::Audio::Duplex::IsoChannelPolicy;
using ASFW::Audio::Duplex::StreamPlan;
using ASFW::Audio::Duplex::StreamPlanner;
using ASFW::Encoding::AudioWireFormat;
using ASFW::FW::FwSpeed;

ResolvedAudioEndpointProfile MakeProfile() {
    ResolvedAudioEndpointProfile profile{};
    profile.runtimeCaps = AudioStreamRuntimeCaps{
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
    return profile;
}

TEST(DuplexStreamProfileTests, PlansResolvedGeometryWithoutIdentityInputs) {
    const ResolvedAudioEndpointProfile resolved = MakeProfile();

    const StreamPlan plan = StreamPlanner::Resolve(resolved, FwSpeed::S400);

    EXPECT_EQ(plan.channels.captureStreamCount, 1U);
    EXPECT_EQ(plan.channels.playbackStreamCount, 1U);
    EXPECT_EQ(plan.channels.deviceToHostIsoChannel, 1U);
    EXPECT_EQ(plan.channels.hostToDeviceIsoChannel, 0U);
    EXPECT_EQ(plan.captureStreams[0].pcmChannels, 0U);
    EXPECT_EQ(plan.captureStreams[0].am824Slots, 17U);
    EXPECT_EQ(plan.playbackStreams[0].pcmChannels, 16U);
    EXPECT_EQ(plan.captureStreams[0].bandwidthUnits, 1076U);
    EXPECT_EQ(plan.playbackStreams[0].bandwidthUnits, 1076U);
    EXPECT_EQ(plan.captureStreams[0].allowedIsoChannels, uint64_t{1} << 1U);
    EXPECT_EQ(plan.playbackStreams[0].allowedIsoChannels, uint64_t{1} << 0U);
}

TEST(DuplexStreamProfileTests, CarriesResolvedWireAndLifecyclePoliciesVerbatim) {
    ResolvedAudioEndpointProfile resolved = MakeProfile();
    resolved.captureWireFormat = AudioWireFormat::kRawPcm24In32;
    resolved.playbackWireFormat = AudioWireFormat::kRawPcm24In32;
    resolved.captureIsoChannelPolicy = IsoChannelPolicy::IRMSelectable;
    resolved.playbackIsoChannelPolicy = IsoChannelPolicy::IRMSelectable;
    resolved.startPolicy.startReceiveBeforeDeviceRx = true;
    resolved.startPolicy.startTransmitBeforeDeviceTx = true;
    resolved.startPolicy.requiresPreStreamClockLock = false;
    resolved.startPolicy.startOrder = {HostDirection::kTransmit, HostDirection::kReceive};
    resolved.startPolicy.postDeviceEnableDelayMs = 0;
    resolved.stopPolicy
        .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive = true;

    const StreamPlan plan = StreamPlanner::Resolve(resolved, FwSpeed::S400);

    EXPECT_EQ(plan.captureWireFormat, AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(plan.playbackWireFormat, AudioWireFormat::kRawPcm24In32);
    EXPECT_EQ(plan.captureStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(plan.playbackStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_TRUE(plan.startOrder.startReceiveBeforeDeviceRx);
    EXPECT_TRUE(plan.startOrder.startTransmitBeforeDeviceTx);
    EXPECT_FALSE(plan.startOrder.requiresPreStreamClockLock);
    EXPECT_EQ(plan.startOrder.startOrder[0], HostDirection::kTransmit);
    EXPECT_EQ(plan.startOrder.startOrder[1], HostDirection::kReceive);
    EXPECT_EQ(plan.startOrder.postDeviceEnableDelayMs, 0U);
    EXPECT_TRUE(plan.stopOrder
                    .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive);
}

TEST(DuplexStreamProfileTests, UsesExplicitlyAssignedChannelsWithoutReResolvingIdentity) {
    ResolvedAudioEndpointProfile resolved = MakeProfile();
    resolved.runtimeCaps.hostInputPcmChannels = 18;
    resolved.runtimeCaps.deviceToHostAm824Slots = 20;
    resolved.runtimeCaps.deviceToHostStreamCount = 2;
    resolved.runtimeCaps.deviceToHostStreams[0] = {
        .pcmChannels = 8,
        .am824Slots = 9,
    };
    resolved.runtimeCaps.deviceToHostStreams[1] = {
        .pcmChannels = 10,
        .am824Slots = 11,
    };

    ASFW::Audio::AudioDuplexChannels assigned{
        .deviceToHostIsoChannel = 7,
        .hostToDeviceIsoChannel = 9,
        .captureStreamCount = 2,
        .playbackStreamCount = 1,
    };
    assigned.captureIsoChannels[1] = 12;

    const StreamPlan plan = StreamPlanner::Resolve(resolved, FwSpeed::S400, assigned);

    EXPECT_EQ(plan.captureStreams[0].isoChannel, 7U);
    EXPECT_EQ(plan.captureStreams[1].isoChannel, 12U);
    EXPECT_EQ(plan.captureStreams[0].pcmChannelOffset, 0U);
    EXPECT_EQ(plan.captureStreams[1].pcmChannelOffset, 8U);
    EXPECT_EQ(plan.captureStreams[0].pcmChannels, 8U);
    EXPECT_EQ(plan.captureStreams[1].pcmChannels, 10U);
    EXPECT_EQ(plan.playbackStreams[0].isoChannel, 9U);
}

} // namespace
