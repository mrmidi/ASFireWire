#include <gtest/gtest.h>

#include <optional>

#include "Audio/Protocols/Backends/DuplexStreamProfile.hpp"
#include "DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"

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
using ASFW::DeviceProfiles::Audio::kMackieVendorId;
using ASFW::DeviceProfiles::Audio::kOnyx400FModelId;
using ASFW::DeviceProfiles::Audio::kWeissInt202ModelId;
using ASFW::DeviceProfiles::Audio::kWeissInt203ModelId;
using ASFW::DeviceProfiles::Audio::kWeissVendorId;
using ASFW::Discovery::DeviceRecord;
using ASFW::Encoding::AudioWireFormat;

// Discovery resolves the catalog once and attaches that decision to the live
// route. These fixtures construct the same record/policy pairing for profile
// tests without involving a bus or DriverKit service.
constexpr uint32_t kDiceInterfaceVersion = 0x000001;
constexpr uint32_t kTa1394AvcSpecifier = 0x00A02D;
constexpr uint32_t kTa1394AvcVersion = 0x010001;
constexpr uint32_t kFireworksVersion = 0x010000;

[[nodiscard]] DeviceRecord MakeRecord(uint32_t vendorId,
                                      std::optional<uint32_t> modelId,
                                      uint32_t unitSpecifier,
                                      uint32_t unitVersion) {
    DeviceRecord record{};
    record.instanceId = ASFW::Discovery::DeviceInstanceId{1};
    record.deviceIncarnation = 1;
    record.routeEpoch = 1;
    record.gen = ASFW::Discovery::Generation{1};
    record.nodeId = 2;
    record.state = ASFW::Discovery::LifeState::Identified;
    record.guid = (static_cast<uint64_t>(vendorId) << 40U) | 0x04'0000'0000ULL;
    record.vendorId = vendorId;
    record.modelId = modelId.value_or(0U);
    record.unitSpecId = unitSpecifier;
    record.unitSwVersion = unitVersion;
    record.identity.observedGuid = record.guid;
    record.identity.nodeVendorOui = vendorId;
    record.identity.rootVendorId = vendorId;
    record.identity.rootModelId = modelId;
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
    record.link.isochToNode = ASFW::FW::FwSpeed::S400;
    ASFW::Discovery::UnitIdentityEvidence unit{};
    unit.unitDirectoryOffset = 5;
    unit.specifierId = unitSpecifier;
    unit.version = unitVersion;
    record.identity.units.push_back(unit);
    const auto plan = ASFW::DeviceProfiles::Audio::AudioDeviceCatalog::Resolve(record);
    EXPECT_TRUE(plan.has_value());
    if (plan.has_value()) {
        record.audioPolicy = std::make_shared<const ASFW::DeviceProfiles::Audio::ResolvedDevicePolicy>(
            ASFW::DeviceProfiles::Audio::ResolvedDevicePolicy{
                *plan,
                ASFW::Discovery::DeviceRouteToken{
                    .guid = record.guid,
                    .deviceIncarnation = record.deviceIncarnation,
                    .routeEpoch = record.routeEpoch,
                    .generation = record.gen,
                    .nodeId = record.nodeId}});
    }
    return record;
}

[[nodiscard]] DeviceRecord DiceRecord(uint32_t vendorId, uint32_t modelId) {
    return MakeRecord(vendorId, modelId, vendorId, kDiceInterfaceVersion);
}

[[nodiscard]] DeviceRecord AvcRecord(uint32_t vendorId, uint32_t modelId) {
    return MakeRecord(vendorId, modelId, kTa1394AvcSpecifier, kTa1394AvcVersion);
}

TEST(DuplexStreamProfileTests, OrdinaryDiceKeepsLegacyChannelsGeometryAndRecipe) {
    DeviceRecord record{};
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
    record.link.isochToNode = ASFW::FW::FwSpeed::S400;
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
    // Packet term only: 17 slots x 8 blocks x 4 bytes + 8 CIP bytes = 552 payload,
    // 138 quadlets + 3 header quadlets = 564 units at S400. The per-allocation bus
    // overhead is charged by the reservation from the live gap count, not here.
    EXPECT_EQ(profile.playbackStreams[0].packetBandwidthUnits, 564U);
    EXPECT_EQ(profile.captureStreams[0].packetBandwidthUnits, 564U);
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
    DeviceRecord record = DiceRecord(kFocusriteVendorId, kSPro24DspModelId);
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
    DeviceRecord record = AvcRecord(kApogeeVendorId, kApogeeDuetModelId);
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
    record.link.isochToNode = ASFW::FW::FwSpeed::S400;
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
    EXPECT_EQ(profile.captureStreams[0].packetBandwidthUnits, 84U);
    EXPECT_EQ(profile.playbackStreams[0].packetBandwidthUnits, 84U);
    EXPECT_TRUE(profile.startOrder.startReceiveBeforeDeviceRx);
    EXPECT_TRUE(profile.startOrder.startTransmitBeforeDeviceTx);
    EXPECT_EQ(profile.startOrder.postDeviceEnableDelayMs, 0U);
    EXPECT_TRUE(profile.stopOrder
                    .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive);
}

TEST(DuplexStreamProfileTests, Phase88PreservesLinuxBeBoBCmpBeforeHostStartOrdering) {
    DeviceRecord record = AvcRecord(kTerraTecVendorId, kPhase88RackFwModelId);
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
    record.link.isochToNode = ASFW::FW::FwSpeed::S400;
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

TEST(DuplexStreamProfileTests, Onyx400FUsesBeBoBOrderingWithoutPreStreamClockLock) {
    DeviceRecord record = MakeRecord(kMackieVendorId, kOnyx400FModelId,
                                     kTa1394AvcSpecifier, kFireworksVersion);
    record.link.localToNode = ASFW::FW::FwSpeed::S400;
    record.link.isochToNode = ASFW::FW::FwSpeed::S400;
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 10,
        .hostOutputPcmChannels = 10,
        .deviceToHostAm824Slots = 11,
        .hostToDeviceAm824Slots = 11,
        .sampleRateHz = 44100,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

    // Field-verified 2026-09-13: the Fireworks unit never reports a pre-connection
    // clock lock, so the DICE-style gate must be off, and CMP owns the channel.
    EXPECT_EQ(profile.captureStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(profile.playbackStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(profile.captureStreams[0].am824Slots, 11U);
    EXPECT_EQ(profile.playbackStreams[0].am824Slots, 11U);
    EXPECT_FALSE(profile.startOrder.startReceiveBeforeDeviceRx);
    EXPECT_FALSE(profile.startOrder.startTransmitBeforeDeviceTx);
    EXPECT_FALSE(profile.startOrder.requiresPreStreamClockLock);
    EXPECT_EQ(profile.startOrder.startOrder[0], DuplexHostDirection::kReceive);
    EXPECT_EQ(profile.startOrder.startOrder[1], DuplexHostDirection::kTransmit);
    EXPECT_TRUE(profile.captureTrustConfiguredStride);
}

TEST(DuplexStreamProfileTests, WeissIntStartsHostTransmitFirstWithoutPreEnableSourceLock) {
    AudioStreamRuntimeCaps caps{
        // CoreAudio presentation is output-only, but the DICE wire topology is
        // still two PCM channels in both directions.
        .hostInputPcmChannels = 0,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 2,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };

    for (const uint32_t modelId : {kWeissInt202ModelId, kWeissInt203ModelId}) {
        DeviceRecord record = DiceRecord(kWeissVendorId, modelId);
        const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

        EXPECT_FALSE(profile.startOrder.requiresPreStreamClockLock) << modelId;
        EXPECT_EQ(profile.startOrder.startOrder[0], DuplexHostDirection::kTransmit) << modelId;
        EXPECT_EQ(profile.startOrder.startOrder[1], DuplexHostDirection::kReceive) << modelId;
        EXPECT_EQ(profile.channels.captureStreamCount, 1U) << modelId;
        EXPECT_EQ(profile.channels.playbackStreamCount, 1U) << modelId;
    }
}

// Was AlesisModelsClampAdvertisedCaptureStreamsToOne, asserting that both Alesis
// rows had capture forced to one stream. That clamp is disabled: it was
// libffado's PLAYBACK workaround (m_nb_rx, dice_avdevice.cpp:1686-1700)
// transcribed onto capture, and the vendor's own driver clamps neither
// direction. See the #if 0 block in DuplexStreamProfile::ResolveChannels.
//
// This now pins the opposite: the device's advertised capture count survives.
// It is deliberately the same two model ids and the same caps, so the diff
// against the old expectation is the behaviour change itself.
//
// TODO(FW-DICE-ALESIS): if hardware shows a MultiMix over-reporting PLAYBACK,
// the clamp comes back against playbackStreamCount and this test grows a
// playback case -- it does not revert.
TEST(DuplexStreamProfileTests, AlesisModelsKeepTheAdvertisedCaptureStreamCount) {
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
        DeviceRecord record = DiceRecord(kAlesisVendorId, modelId);
        const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

        // Both streams the device advertised are armed. Under the clamp this
        // was 1, which on the recorded MultiMix meant dropping MAIN_IN L/R.
        EXPECT_EQ(profile.channels.captureStreamCount, 2U) << modelId;
        EXPECT_EQ(profile.channels.playbackStreamCount, 2U) << modelId;
        EXPECT_EQ(profile.captureStreams[0].isoChannel, 5U) << modelId;

        // Per-stream slots, not the device's aggregate. This is the clamp's
        // second and worse effect: Build() selects per-stream geometry only
        // when captureStreamCount > 1 (DuplexStreamProfile.hpp:346), so forcing
        // the count to 1 also made stream 0 report deviceToHostAm824Slots -- 34
        // here -- when the stream physically carries 17. packetBandwidthUnits is
        // derived from am824Slots, so the IRM reservation was sized from the
        // aggregate too.
        EXPECT_EQ(profile.captureStreams[0].am824Slots, 17U) << modelId;
        EXPECT_EQ(profile.captureStreams[1].am824Slots, 17U) << modelId;
        EXPECT_EQ(profile.captureStreams[0].pcmChannels, 16U) << modelId;
        EXPECT_EQ(profile.captureStreams[1].pcmChannels, 16U) << modelId;

        // The second capture stream gets an iso channel of its own rather than
        // being discarded.
        EXPECT_NE(profile.channels.CaptureChannel(1),
                  profile.channels.CaptureChannel(0)) << modelId;
    }
}

} // namespace

TEST(DuplexStreamProfileTests, MotuCaptureCarriesTheModelPortMap) {
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = 14,
        .hostOutputPcmChannels = 14,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    // MOTU publishes root model_id 0 -- not "no model_id", which is a
    // different thing the catalog can tell apart.
    DeviceRecord record = MakeRecord(
        ASFW::DeviceProfiles::Audio::kMotuVendorId, 0U,
        ASFW::DeviceProfiles::Audio::kMotuVendorId,
        ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion);

    const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);

    EXPECT_EQ(profile.captureWireFormat, AudioWireFormat::kMotuV2);
    EXPECT_EQ(profile.captureMotuPcmChunks, 14U);
    ASSERT_EQ(profile.captureMotuPorts.size(), 14U);
    EXPECT_EQ(profile.captureMotuPorts.data(), ASFW::Encoding::Motu::kUltraLiteCapture);
}

TEST(DuplexStreamProfileTests, NonMotuDevicesCarryNoPortMap) {
    DeviceRecord record{};
    AudioStreamRuntimeCaps caps{.hostInputPcmChannels = 2, .hostOutputPcmChannels = 2};
    const DuplexStreamProfile profile = DuplexStreamProfileResolver::Resolve(record, caps);
    EXPECT_TRUE(profile.captureMotuPorts.empty());
}
