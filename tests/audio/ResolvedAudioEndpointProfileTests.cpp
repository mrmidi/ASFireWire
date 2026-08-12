// SPDX-License-Identifier: Apache-2.0

#include "ASFWDriver/Audio/Devices/AudioEndpointProfileWire.hpp"
#include "ASFWDriver/Audio/Duplex/DuplexStreamPlanner.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace {

using namespace ASFW;
using namespace ASFW::Audio;
using namespace ASFW::Audio::Devices;

ResolvedAudioEndpointProfile MakeProfile() {
    ResolvedAudioEndpointProfile profile{};
    profile.endpointId = AudioEndpointId{41};
    profile.deviceInstanceId = Discovery::DeviceInstanceId{7};
    profile.unitInstanceId = Discovery::UnitInstanceId{profile.deviceInstanceId, 0x24};
    profile.observedGuid = 0x0011223344556677ULL;
    profile.definitionId = DeviceProfiles::Audio::DeviceDefinitionId::FocusriteSPro24Dsp;
    profile.exactVariantId = 17;
    profile.equivalenceClassId = 4;
    profile.familyProvider = DeviceProfiles::Audio::AudioFamilyProviderId::DICE;
    profile.profileBuilder = DeviceProfiles::Audio::ProfileBuilderId::FocusriteSPro24Dsp;
    profile.identityStatus = PersistentIdentityStatus::Persistent;
    profile.captureWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
    profile.playbackWireFormat = Encoding::AudioWireFormat::kAM824;
    profile.captureIsoChannelPolicy = Audio::Duplex::IsoChannelPolicy::Fixed;
    profile.playbackIsoChannelPolicy = Audio::Duplex::IsoChannelPolicy::IRMSelectable;
    profile.startPolicy.requiresPreStreamClockLock = false;
    profile.startPolicy.postDeviceEnableDelayMs = 9;
    profile.currentSampleRateHz = 96000;
    profile.supportedRates = {44100, 48000, 88200, 96000};
    profile.supportedRateCount = 4;
    profile.runtimeCaps = {
        .hostInputPcmChannels = 24,
        .hostOutputPcmChannels = 16,
        .deviceToHostAm824Slots = 26,
        .hostToDeviceAm824Slots = 18,
        .sampleRateHz = 96000,
        .deviceToHostIsoChannel = 3,
        .hostToDeviceIsoChannel = 5,
        .deviceToHostStreamCount = 2,
        .hostToDeviceStreamCount = 1,
    };
    profile.runtimeCaps.deviceToHostStreams[0] = {3, 16, 18, 1};
    profile.runtimeCaps.deviceToHostStreams[1] = {4, 8, 8, 0};
    profile.runtimeCaps.hostToDeviceStreams[0] = {5, 16, 18, 1};
    profile.timing[0] = {96000, 32, 48, 16, 24, 12800, 13000, 750};
    profile.timingCount = 1;
    profile.facets = {{FacetKind::Clock, 1}, {FacetKind::Mixer, 0x53503234}};
    return profile;
}

TEST(AudioEndpointProfileWire, RoundTripsBoundedNumericSnapshot) {
    const auto encoded = Audio::Devices::Wire::Serialize(MakeProfile());
    ASSERT_TRUE(encoded.has_value());
    ASSERT_LE(encoded->size(), Audio::Devices::Wire::kAudioEndpointProfileWireMaxBytes);

    const auto decoded = Audio::Devices::Wire::Parse(*encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->endpointId, AudioEndpointId{41});
    EXPECT_EQ(decoded->deviceInstanceId, Discovery::DeviceInstanceId{7});
    EXPECT_EQ(decoded->unitInstanceId.unitDirectoryOffset, 0x24U);
    EXPECT_EQ(decoded->observedGuid, 0x0011223344556677ULL);
    EXPECT_EQ(decoded->exactVariantId, 17U);
    EXPECT_EQ(decoded->supportedRateCount, 4U);
    EXPECT_EQ(decoded->supportedRates[3], 96000U);
    EXPECT_EQ(decoded->runtimeCaps.deviceToHostStreamCount, 2U);
    EXPECT_EQ(decoded->runtimeCaps.deviceToHostStreams[1].pcmChannels, 8U);
    EXPECT_EQ(decoded->timing[0].anchorTimeoutMs, 750U);
    ASSERT_EQ(decoded->facets.size(), 2U);
    EXPECT_EQ(decoded->facets[1].schemaId, 0x53503234U);
}

TEST(AudioEndpointProfileWire, RejectsVersionTruncationAndMalformedSection) {
    auto encoded = Audio::Devices::Wire::Serialize(MakeProfile());
    ASSERT_TRUE(encoded.has_value());

    auto badVersion = *encoded;
    badVersion[0] = 0x7f;
    EXPECT_EQ(Audio::Devices::Wire::Parse(badVersion).error(),
              Audio::Devices::Wire::WireError::UnsupportedVersion);

    EXPECT_EQ(Audio::Devices::Wire::Parse(
                  std::span<const uint8_t>{encoded->data(), encoded->size() - 1}).error(),
              Audio::Devices::Wire::WireError::InvalidHeader);

    auto badSection = *encoded;
    Audio::Devices::Wire::AudioEndpointProfileWireV1 header{};
    std::memcpy(&header, badSection.data(), sizeof(header));
    header.rates.offset = static_cast<uint16_t>(sizeof(header) - 1);
    std::memcpy(badSection.data(), &header, sizeof(header));
    EXPECT_EQ(Audio::Devices::Wire::Parse(badSection).error(),
              Audio::Devices::Wire::WireError::InvalidSection);
}

TEST(AudioEndpointProfileWire, RejectsCountsBeyondContract) {
    auto profile = MakeProfile();
    profile.supportedRateCount = static_cast<uint8_t>(profile.supportedRates.size() + 1);
    EXPECT_EQ(Audio::Devices::Wire::Serialize(profile).error(),
              Audio::Devices::Wire::WireError::InvalidCount);
}

TEST(AudioEndpointProfileWire, RejectsOverlappingSectionsUnknownEnumsAndReservedBits) {
    auto encoded = Audio::Devices::Wire::Serialize(MakeProfile());
    ASSERT_TRUE(encoded.has_value());

    Audio::Devices::Wire::AudioEndpointProfileWireV1 header{};
    std::memcpy(&header, encoded->data(), sizeof(header));

    auto overlap = *encoded;
    auto overlapHeader = header;
    overlapHeader.timing.offset = overlapHeader.rates.offset;
    std::memcpy(overlap.data(), &overlapHeader, sizeof(overlapHeader));
    EXPECT_EQ(Audio::Devices::Wire::Parse(overlap).error(),
              Audio::Devices::Wire::WireError::InvalidSection);

    auto unknownEnum = *encoded;
    auto enumHeader = header;
    enumHeader.familyProviderId = 0xFF;
    std::memcpy(unknownEnum.data(), &enumHeader, sizeof(enumHeader));
    EXPECT_EQ(Audio::Devices::Wire::Parse(unknownEnum).error(),
              Audio::Devices::Wire::WireError::InvalidValue);

    auto reserved = *encoded;
    auto reservedHeader = header;
    reservedHeader._reserved[0] = 1;
    std::memcpy(reserved.data(), &reservedHeader, sizeof(reservedHeader));
    EXPECT_EQ(Audio::Devices::Wire::Parse(reserved).error(),
              Audio::Devices::Wire::WireError::InvalidValue);

    auto unclaimedTail = *encoded;
    unclaimedTail.push_back(0);
    auto tailHeader = header;
    tailHeader.byteSize = static_cast<uint32_t>(unclaimedTail.size());
    std::memcpy(unclaimedTail.data(), &tailHeader, sizeof(tailHeader));
    EXPECT_EQ(Audio::Devices::Wire::Parse(unclaimedTail).error(),
              Audio::Devices::Wire::WireError::InvalidSection);
}

TEST(AudioEndpointProfileWire, SupportsAbsentOptionalSectionsButRequiresRates) {
    auto profile = MakeProfile();
    profile.facets.clear();
    profile.timingCount = 0;
    const auto encoded = Audio::Devices::Wire::Serialize(profile);
    ASSERT_TRUE(encoded.has_value());
    const auto decoded = Audio::Devices::Wire::Parse(*encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->facets.empty());
    EXPECT_EQ(decoded->timingCount, 0U);

    profile.supportedRateCount = 0;
    EXPECT_EQ(Audio::Devices::Wire::Serialize(profile).error(),
              Audio::Devices::Wire::WireError::InvalidCount);
}

TEST(DuplexStreamPlanner, UsesResolvedPoliciesWithoutIdentityMatching) {
    auto profile = MakeProfile();
    const auto plan = Audio::Duplex::StreamPlanner::Resolve(profile, FW::FwSpeed::S400);

    EXPECT_EQ(plan.channels.captureStreamCount, 2U);
    EXPECT_EQ(plan.captureStreams[0].isoChannel, 3U);
    EXPECT_EQ(plan.captureStreams[1].pcmChannelOffset, 16U);
    EXPECT_EQ(plan.captureStreams[1].pcmChannels, 8U);
    EXPECT_EQ(plan.captureStreams[0].allowedIsoChannels, uint64_t{1} << 3U);
    EXPECT_EQ(plan.playbackStreams[0].allowedIsoChannels, ~uint64_t{0});
    EXPECT_EQ(plan.captureWireFormat, Encoding::AudioWireFormat::kRawPcm24In32);
}

} // namespace
