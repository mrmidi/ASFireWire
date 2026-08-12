// SPDX-License-Identifier: Apache-2.0

#include "AudioEndpointProfileWire.hpp"

#include <cstring>
#include <algorithm>
#include <array>
#include <limits>

namespace ASFW::Audio::Devices::Wire {
namespace {

template <typename T>
[[nodiscard]] bool AppendSpan(std::vector<uint8_t>& bytes,
                              std::span<const T> values,
                              Section& section) noexcept {
    if (values.empty()) {
        section = {};
        return true;
    }
    if (bytes.size() > kAudioEndpointProfileWireMaxBytes ||
        values.size_bytes() > std::numeric_limits<uint16_t>::max() ||
        bytes.size() > std::numeric_limits<uint16_t>::max() ||
        values.size_bytes() > kAudioEndpointProfileWireMaxBytes - bytes.size()) {
        return false;
    }
    section.offset = static_cast<uint16_t>(bytes.size());
    section.byteSize = static_cast<uint16_t>(values.size_bytes());
    const size_t oldSize = bytes.size();
    bytes.resize(oldSize + values.size_bytes());
    if (!values.empty()) {
        std::memcpy(bytes.data() + oldSize, values.data(), values.size_bytes());
    }
    return true;
}

[[nodiscard]] bool SectionsAreContiguous(
    std::span<const uint8_t> bytes,
    const AudioEndpointProfileWireV1& header) noexcept {
    std::array<Section, 5> sections{header.rates, header.captureStreams,
                                    header.playbackStreams, header.timing,
                                    header.facets};
    std::vector<std::pair<size_t, size_t>> ranges;
    ranges.reserve(sections.size());
    for (const auto section : sections) {
        if ((section.offset == 0) != (section.byteSize == 0)) return false;
        if (section.byteSize == 0) continue;
        const size_t begin = section.offset;
        const size_t length = section.byteSize;
        if (begin < sizeof(AudioEndpointProfileWireV1) || begin > bytes.size() ||
            length > bytes.size() - begin) return false;
        ranges.emplace_back(begin, begin + length);
    }
    std::ranges::sort(ranges);
    size_t claimedEnd = sizeof(AudioEndpointProfileWireV1);
    for (const auto& range : ranges) {
        if (range.first != claimedEnd) return false;
        claimedEnd = range.second;
    }
    // Every byte in a V1 property is claimed by the fixed header or a typed
    // section. Reject gaps and unknown trailing data instead of silently
    // accepting a second, unvalidated wire format inside the property.
    return claimedEnd == bytes.size();
}

[[nodiscard]] constexpr bool ValidIsoChannel(uint8_t channel) noexcept {
    return channel == AudioStreamWireInfo::kInvalidIsoChannel || channel <= 0x3F;
}

[[nodiscard]] constexpr bool ValidEnumFields(
    const AudioEndpointProfileWireV1& header) noexcept {
    using DeviceProfiles::Audio::AudioFamilyProviderId;
    using DeviceProfiles::Audio::ProfileBuilderId;
    return header.familyProviderId >= static_cast<uint8_t>(AudioFamilyProviderId::GenericAvc) &&
           header.familyProviderId <= static_cast<uint8_t>(AudioFamilyProviderId::OXFW) &&
           header.profileBuilderId >= static_cast<uint16_t>(ProfileBuilderId::GenericAvc) &&
           header.profileBuilderId <=
               static_cast<uint16_t>(ProfileBuilderId::PreSonusStudioLive1602) &&
           header.identityStatus <= static_cast<uint8_t>(PersistentIdentityStatus::Ephemeral) &&
           header.captureWireFormat <=
               static_cast<uint8_t>(Encoding::AudioWireFormat::kRawPcm24In32) &&
           header.playbackWireFormat <=
               static_cast<uint8_t>(Encoding::AudioWireFormat::kRawPcm24In32) &&
           header.streamMode <= static_cast<uint8_t>(StreamModePolicy::Blocking) &&
           header.captureIsoChannelPolicy <=
               static_cast<uint8_t>(Duplex::IsoChannelPolicy::IRMSelectable) &&
           header.playbackIsoChannelPolicy <=
               static_cast<uint8_t>(Duplex::IsoChannelPolicy::IRMSelectable) &&
           header.prepareOrder[0] <= static_cast<uint8_t>(Duplex::HostDirection::kTransmit) &&
           header.prepareOrder[1] <= static_cast<uint8_t>(Duplex::HostDirection::kTransmit) &&
           header.startOrder[0] <= static_cast<uint8_t>(Duplex::HostDirection::kTransmit) &&
           header.startOrder[1] <= static_cast<uint8_t>(Duplex::HostDirection::kTransmit);
}

[[nodiscard]] constexpr bool ValidFlagsAndReserved(
    const AudioEndpointProfileWireV1& header) noexcept {
    return (header.txPacketFlags & ~0x0FU) == 0 &&
           (header.recoveryFlags & ~0x07U) == 0 &&
           (header.clockFlags & ~0x01U) == 0 &&
           (header.startFlags & ~0x07U) == 0 &&
           (header.stopFlags & ~0x01U) == 0 &&
           header._reserved[0] == 0 && header._reserved[1] == 0;
}

template <typename T>
[[nodiscard]] std::expected<std::vector<T>, WireError>
ReadSection(std::span<const uint8_t> bytes, Section section,
            size_t maxElements) noexcept {
    if ((section.offset == 0) != (section.byteSize == 0)) {
        return std::unexpected(WireError::InvalidSection);
    }
    if (section.byteSize == 0) {
        return std::vector<T>{};
    }
    const size_t begin = section.offset;
    const size_t length = section.byteSize;
    if (begin < sizeof(AudioEndpointProfileWireV1) || begin > bytes.size() ||
        length > bytes.size() - begin || length % sizeof(T) != 0) {
        return std::unexpected(WireError::InvalidSection);
    }
    const size_t count = length / sizeof(T);
    if (count > maxElements) {
        return std::unexpected(WireError::InvalidCount);
    }
    // Wire sections are byte-packed and are not guaranteed to satisfy T's
    // alignment. Copy into aligned storage rather than manufacturing a typed
    // pointer into the property blob.
    std::vector<T> values(count);
    std::memcpy(values.data(), bytes.data() + begin, length);
    return values;
}

} // namespace

std::expected<std::vector<uint8_t>, WireError>
Serialize(const ResolvedAudioEndpointProfile& profile) noexcept {
    if (!profile.endpointId || !profile.deviceInstanceId ||
        profile.unitInstanceId.device != profile.deviceInstanceId ||
        profile.currentSampleRateHz == 0 || profile.supportedRateCount == 0 ||
        profile.supportedRateCount > profile.supportedRates.size() ||
        profile.timingCount > profile.timing.size() ||
        profile.runtimeCaps.deviceToHostStreamCount > kMaxAudioStreamsPerDirection ||
        profile.runtimeCaps.hostToDeviceStreamCount > kMaxAudioStreamsPerDirection ||
        profile.facets.size() > 32) {
        return std::unexpected(WireError::InvalidCount);
    }

    AudioEndpointProfileWireV1 header{};
    header.version = kAudioEndpointProfileWireVersion;
    header.headerSize = sizeof(header);
    header.endpointId = profile.endpointId.value;
    header.deviceInstanceId = profile.deviceInstanceId.value;
    header.observedGuid = profile.observedGuid;
    header.unitDirectoryOffset = profile.unitInstanceId.unitDirectoryOffset;
    header.definitionId = static_cast<uint32_t>(profile.definitionId);
    header.variantId = profile.exactVariantId.value_or(0);
    header.equivalenceClassId = profile.equivalenceClassId.value_or(0);
    header.profileBuilderId = static_cast<uint16_t>(profile.profileBuilder);
    header.familyProviderId = static_cast<uint8_t>(profile.familyProvider);
    header.identityStatus = static_cast<uint8_t>(profile.identityStatus);
    header.captureWireFormat = static_cast<uint8_t>(profile.captureWireFormat);
    header.playbackWireFormat = static_cast<uint8_t>(profile.playbackWireFormat);
    header.captureFramesPerDataPacket = profile.captureFramesPerDataPacket;
    header.playbackFramesPerDataPacket = profile.playbackFramesPerDataPacket;
    header.captureFdf = profile.captureFdf;
    header.playbackFdf = profile.playbackFdf;
    header.captureFmt = profile.captureFmt;
    header.playbackFmt = profile.playbackFmt;
    header.txPacketFlags = (profile.txPacketPolicy.variableDbs ? 1U : 0U) |
                           (profile.txPacketPolicy.initializeNonAudioSlots ? 2U : 0U) |
                           (profile.txPacketPolicy.preserveFdfInNoDataPackets ? 4U : 0U) |
                           (profile.txPacketPolicy.emptyPacketsDuringIdle ? 8U : 0U);
    header.recoveryFlags =
        (profile.recoveryPolicy.recoverAfterTimingLoss ? 1U : 0U) |
        (profile.recoveryPolicy.recoverAfterCycleInconsistent ? 2U : 0U) |
        (profile.recoveryPolicy.recoverAfterLockLoss ? 4U : 0U);
    header.recoveryMaximumAttempts = profile.recoveryPolicy.maximumAttempts;
    header.clockFlags = profile.clockPolicy.hostRateChangeSupported ? 1U : 0U;
    header.streamMode = static_cast<uint8_t>(profile.streamMode);
    header.captureIsoChannelPolicy = static_cast<uint8_t>(profile.captureIsoChannelPolicy);
    header.playbackIsoChannelPolicy = static_cast<uint8_t>(profile.playbackIsoChannelPolicy);
    header.startFlags = (profile.startPolicy.startReceiveBeforeDeviceRx ? 1U : 0U) |
                        (profile.startPolicy.startTransmitBeforeDeviceTx ? 2U : 0U) |
                        (profile.startPolicy.requiresPreStreamClockLock ? 4U : 0U);
    header.prepareOrder[0] = static_cast<uint8_t>(profile.startPolicy.prepareOrder[0]);
    header.prepareOrder[1] = static_cast<uint8_t>(profile.startPolicy.prepareOrder[1]);
    header.startOrder[0] = static_cast<uint8_t>(profile.startPolicy.startOrder[0]);
    header.startOrder[1] = static_cast<uint8_t>(profile.startPolicy.startOrder[1]);
    header.stopFlags = profile.stopPolicy
        .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive ? 1U : 0U;
    header.postDeviceEnableDelayMs = profile.startPolicy.postDeviceEnableDelayMs;
    header.defaultNonAudioSlotWord = profile.txPacketPolicy.defaultNonAudioSlotWord;
    header.clockLockTimeoutMs = profile.clockPolicy.lockTimeoutMs;
    header.clockStableReadCount = profile.clockPolicy.stableReadCount;
    header.currentSampleRateHz = profile.currentSampleRateHz;
    header.hostInputPcmChannels = profile.runtimeCaps.hostInputPcmChannels;
    header.hostOutputPcmChannels = profile.runtimeCaps.hostOutputPcmChannels;
    header.deviceToHostAm824Slots = profile.runtimeCaps.deviceToHostAm824Slots;
    header.hostToDeviceAm824Slots = profile.runtimeCaps.hostToDeviceAm824Slots;
    header.deviceToHostIsoChannel = profile.runtimeCaps.deviceToHostIsoChannel;
    header.hostToDeviceIsoChannel = profile.runtimeCaps.hostToDeviceIsoChannel;

    std::vector<uint8_t> bytes(sizeof(header), 0);
    if (!AppendSpan(bytes,
                    std::span<const uint32_t>{profile.supportedRates.data(),
                                              profile.supportedRateCount},
                    header.rates)) {
        return std::unexpected(WireError::TooLarge);
    }

    std::array<StreamWireV1, kMaxAudioStreamsPerDirection> capture{};
    for (uint32_t i = 0; i < profile.runtimeCaps.deviceToHostStreamCount; ++i) {
        const auto& source = profile.runtimeCaps.deviceToHostStreams[i];
        capture[i] = {source.pcmChannels, source.am824Slots, source.isoChannel, {0, 0, 0}};
    }
    if (!AppendSpan(bytes,
                    std::span<const StreamWireV1>{capture.data(),
                        profile.runtimeCaps.deviceToHostStreamCount},
                    header.captureStreams)) {
        return std::unexpected(WireError::TooLarge);
    }

    std::array<StreamWireV1, kMaxAudioStreamsPerDirection> playback{};
    for (uint32_t i = 0; i < profile.runtimeCaps.hostToDeviceStreamCount; ++i) {
        const auto& source = profile.runtimeCaps.hostToDeviceStreams[i];
        playback[i] = {source.pcmChannels, source.am824Slots, source.isoChannel, {0, 0, 0}};
    }
    if (!AppendSpan(bytes,
                    std::span<const StreamWireV1>{playback.data(),
                        profile.runtimeCaps.hostToDeviceStreamCount},
                    header.playbackStreams)) {
        return std::unexpected(WireError::TooLarge);
    }

    std::array<TimingWireV1, kMaxResolvedRates> timing{};
    for (uint8_t i = 0; i < profile.timingCount; ++i) {
        const auto& source = profile.timing[i];
        timing[i] = {source.sampleRateHz, source.inputLatencyFrames,
                     source.outputLatencyFrames, source.inputSafetyFrames,
                     source.outputSafetyFrames, source.rxTransferDelayTicks,
                     source.txTransferDelayTicks, source.anchorTimeoutMs};
    }
    if (!AppendSpan(bytes,
                    std::span<const TimingWireV1>{timing.data(), profile.timingCount},
                    header.timing)) {
        return std::unexpected(WireError::TooLarge);
    }

    std::vector<FacetWireV1> facets;
    facets.reserve(profile.facets.size());
    for (const auto& facet : profile.facets) {
        facets.push_back(FacetWireV1{static_cast<uint8_t>(facet.kind), {0, 0, 0},
                                     facet.schemaId});
    }
    if (!AppendSpan(bytes, std::span<const FacetWireV1>{facets}, header.facets)) {
        return std::unexpected(WireError::TooLarge);
    }

    header.byteSize = static_cast<uint32_t>(bytes.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

std::expected<ResolvedAudioEndpointProfile, WireError>
Parse(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() < sizeof(AudioEndpointProfileWireV1)) {
        return std::unexpected(WireError::Truncated);
    }
    AudioEndpointProfileWireV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.version != kAudioEndpointProfileWireVersion) {
        return std::unexpected(WireError::UnsupportedVersion);
    }
    if (header.headerSize != sizeof(header) || header.byteSize != bytes.size() ||
        header.byteSize > kAudioEndpointProfileWireMaxBytes) {
        return std::unexpected(WireError::InvalidHeader);
    }
    if (header.endpointId == 0 || header.deviceInstanceId == 0 ||
        header.currentSampleRateHz == 0 || !ValidEnumFields(header) ||
        !ValidFlagsAndReserved(header) ||
        !ValidIsoChannel(header.deviceToHostIsoChannel) ||
        !ValidIsoChannel(header.hostToDeviceIsoChannel)) {
        return std::unexpected(WireError::InvalidValue);
    }
    if (!SectionsAreContiguous(bytes, header)) {
        return std::unexpected(WireError::InvalidSection);
    }

    auto rates = ReadSection<uint32_t>(bytes, header.rates, kMaxResolvedRates);
    auto capture = ReadSection<StreamWireV1>(bytes, header.captureStreams,
                                             kMaxAudioStreamsPerDirection);
    auto playback = ReadSection<StreamWireV1>(bytes, header.playbackStreams,
                                              kMaxAudioStreamsPerDirection);
    auto timing = ReadSection<TimingWireV1>(bytes, header.timing, kMaxResolvedRates);
    auto facets = ReadSection<FacetWireV1>(bytes, header.facets, 32);
    if (!rates) return std::unexpected(rates.error());
    if (!capture) return std::unexpected(capture.error());
    if (!playback) return std::unexpected(playback.error());
    if (!timing) return std::unexpected(timing.error());
    if (!facets) return std::unexpected(facets.error());
    if (rates->empty()) return std::unexpected(WireError::InvalidCount);
    if (std::ranges::any_of(*rates, [](uint32_t rate) { return rate == 0; }) ||
        std::ranges::any_of(*capture, [](const StreamWireV1& stream) {
            return stream._reserved[0] != 0 || stream._reserved[1] != 0 ||
                   stream._reserved[2] != 0 || !ValidIsoChannel(stream.isoChannel) ||
                   (stream.pcmChannels != 0 && stream.am824Slots < stream.pcmChannels);
        }) ||
        std::ranges::any_of(*playback, [](const StreamWireV1& stream) {
            return stream._reserved[0] != 0 || stream._reserved[1] != 0 ||
                   stream._reserved[2] != 0 || !ValidIsoChannel(stream.isoChannel) ||
                   (stream.pcmChannels != 0 && stream.am824Slots < stream.pcmChannels);
        }) ||
        std::ranges::any_of(*timing, [](const TimingWireV1& value) {
            return value.sampleRateHz == 0 || value.anchorTimeoutMs == 0;
        }) ||
        std::ranges::any_of(*facets, [](const FacetWireV1& facet) {
            return facet._reserved[0] != 0 || facet._reserved[1] != 0 ||
                   facet._reserved[2] != 0 ||
                   facet.kind > static_cast<uint8_t>(FacetKind::VendorControl);
        })) {
        return std::unexpected(WireError::InvalidValue);
    }

    ResolvedAudioEndpointProfile profile{};
    profile.endpointId = AudioEndpointId{header.endpointId};
    profile.deviceInstanceId = Discovery::DeviceInstanceId{header.deviceInstanceId};
    profile.unitInstanceId = Discovery::UnitInstanceId{
        profile.deviceInstanceId, header.unitDirectoryOffset};
    profile.observedGuid = header.observedGuid;
    profile.definitionId = static_cast<DeviceProfiles::Audio::DeviceDefinitionId>(header.definitionId);
    if (header.variantId != 0) profile.exactVariantId = header.variantId;
    if (header.equivalenceClassId != 0) profile.equivalenceClassId = header.equivalenceClassId;
    profile.profileBuilder = static_cast<DeviceProfiles::Audio::ProfileBuilderId>(header.profileBuilderId);
    profile.familyProvider = static_cast<DeviceProfiles::Audio::AudioFamilyProviderId>(header.familyProviderId);
    profile.identityStatus = static_cast<PersistentIdentityStatus>(header.identityStatus);
    profile.captureWireFormat = static_cast<Encoding::AudioWireFormat>(header.captureWireFormat);
    profile.playbackWireFormat = static_cast<Encoding::AudioWireFormat>(header.playbackWireFormat);
    profile.captureFramesPerDataPacket = header.captureFramesPerDataPacket;
    profile.playbackFramesPerDataPacket = header.playbackFramesPerDataPacket;
    profile.captureFdf = header.captureFdf;
    profile.playbackFdf = header.playbackFdf;
    profile.captureFmt = header.captureFmt;
    profile.playbackFmt = header.playbackFmt;
    profile.txPacketPolicy.variableDbs = (header.txPacketFlags & 1U) != 0;
    profile.txPacketPolicy.initializeNonAudioSlots = (header.txPacketFlags & 2U) != 0;
    profile.txPacketPolicy.preserveFdfInNoDataPackets = (header.txPacketFlags & 4U) != 0;
    profile.txPacketPolicy.emptyPacketsDuringIdle = (header.txPacketFlags & 8U) != 0;
    profile.txPacketPolicy.defaultNonAudioSlotWord = header.defaultNonAudioSlotWord;
    profile.recoveryPolicy.recoverAfterTimingLoss = (header.recoveryFlags & 1U) != 0;
    profile.recoveryPolicy.recoverAfterCycleInconsistent = (header.recoveryFlags & 2U) != 0;
    profile.recoveryPolicy.recoverAfterLockLoss = (header.recoveryFlags & 4U) != 0;
    profile.recoveryPolicy.maximumAttempts = header.recoveryMaximumAttempts;
    profile.clockPolicy.hostRateChangeSupported = (header.clockFlags & 1U) != 0;
    profile.clockPolicy.lockTimeoutMs = header.clockLockTimeoutMs;
    profile.clockPolicy.stableReadCount = header.clockStableReadCount;
    profile.streamMode = static_cast<StreamModePolicy>(header.streamMode);
    profile.captureIsoChannelPolicy = static_cast<Duplex::IsoChannelPolicy>(header.captureIsoChannelPolicy);
    profile.playbackIsoChannelPolicy = static_cast<Duplex::IsoChannelPolicy>(header.playbackIsoChannelPolicy);
    profile.startPolicy.startReceiveBeforeDeviceRx = (header.startFlags & 1U) != 0;
    profile.startPolicy.startTransmitBeforeDeviceTx = (header.startFlags & 2U) != 0;
    profile.startPolicy.requiresPreStreamClockLock = (header.startFlags & 4U) != 0;
    profile.startPolicy.prepareOrder = {
        static_cast<Duplex::HostDirection>(header.prepareOrder[0]),
        static_cast<Duplex::HostDirection>(header.prepareOrder[1])};
    profile.startPolicy.startOrder = {
        static_cast<Duplex::HostDirection>(header.startOrder[0]),
        static_cast<Duplex::HostDirection>(header.startOrder[1])};
    profile.stopPolicy.disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive =
        (header.stopFlags & 1U) != 0;
    profile.startPolicy.postDeviceEnableDelayMs = header.postDeviceEnableDelayMs;
    profile.currentSampleRateHz = header.currentSampleRateHz;

    profile.supportedRateCount = static_cast<uint8_t>(rates->size());
    for (size_t i = 0; i < rates->size(); ++i) profile.supportedRates[i] = (*rates)[i];
    profile.runtimeCaps.sampleRateHz = header.currentSampleRateHz;
    profile.runtimeCaps.hostInputPcmChannels = header.hostInputPcmChannels;
    profile.runtimeCaps.hostOutputPcmChannels = header.hostOutputPcmChannels;
    profile.runtimeCaps.deviceToHostAm824Slots = header.deviceToHostAm824Slots;
    profile.runtimeCaps.hostToDeviceAm824Slots = header.hostToDeviceAm824Slots;
    profile.runtimeCaps.deviceToHostIsoChannel = header.deviceToHostIsoChannel;
    profile.runtimeCaps.hostToDeviceIsoChannel = header.hostToDeviceIsoChannel;
    profile.runtimeCaps.deviceToHostStreamCount = static_cast<uint32_t>(capture->size());
    for (size_t i = 0; i < capture->size(); ++i) {
        profile.runtimeCaps.deviceToHostStreams[i].pcmChannels = (*capture)[i].pcmChannels;
        profile.runtimeCaps.deviceToHostStreams[i].am824Slots = (*capture)[i].am824Slots;
        profile.runtimeCaps.deviceToHostStreams[i].isoChannel = (*capture)[i].isoChannel;
    }
    profile.runtimeCaps.hostToDeviceStreamCount = static_cast<uint32_t>(playback->size());
    for (size_t i = 0; i < playback->size(); ++i) {
        profile.runtimeCaps.hostToDeviceStreams[i].pcmChannels = (*playback)[i].pcmChannels;
        profile.runtimeCaps.hostToDeviceStreams[i].am824Slots = (*playback)[i].am824Slots;
        profile.runtimeCaps.hostToDeviceStreams[i].isoChannel = (*playback)[i].isoChannel;
    }
    profile.timingCount = static_cast<uint8_t>(timing->size());
    for (size_t i = 0; i < timing->size(); ++i) {
        const auto& source = (*timing)[i];
        profile.timing[i] = {source.sampleRateHz, source.inputLatencyFrames,
                             source.outputLatencyFrames, source.inputSafetyFrames,
                             source.outputSafetyFrames, source.rxTransferDelayTicks,
                             source.txTransferDelayTicks, source.anchorTimeoutMs};
    }
    for (const auto& facet : *facets) {
        profile.facets.push_back({static_cast<FacetKind>(facet.kind), facet.schemaId});
    }
    return profile;
}

} // namespace ASFW::Audio::Devices::Wire
