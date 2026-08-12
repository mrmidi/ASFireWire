// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "ResolvedAudioEndpointProfile.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ASFW::Audio::Devices::Wire {

inline constexpr uint16_t kAudioEndpointProfileWireVersion = 1;
inline constexpr size_t kAudioEndpointProfileWireMaxBytes = 4096;

struct Section final {
    uint16_t offset{0};
    uint16_t byteSize{0};
} __attribute__((packed));

struct AudioEndpointProfileWireV1 final {
    uint16_t version{0};
    uint16_t headerSize{0};
    uint32_t byteSize{0};
    uint64_t endpointId{0};
    uint64_t deviceInstanceId{0};
    uint64_t observedGuid{0};
    uint32_t unitDirectoryOffset{0};
    uint32_t definitionId{0};
    uint32_t variantId{0};
    uint32_t equivalenceClassId{0};
    uint16_t profileBuilderId{0};
    uint8_t familyProviderId{0};
    uint8_t identityStatus{0};
    uint8_t captureWireFormat{0};
    uint8_t playbackWireFormat{0};
    uint8_t captureFramesPerDataPacket{0};
    uint8_t playbackFramesPerDataPacket{0};
    uint8_t captureFdf{0};
    uint8_t playbackFdf{0};
    uint8_t captureFmt{0};
    uint8_t playbackFmt{0};
    uint8_t txPacketFlags{0};
    uint8_t recoveryFlags{0};
    uint8_t recoveryMaximumAttempts{0};
    uint8_t clockFlags{0};
    uint8_t streamMode{0};
    uint8_t captureIsoChannelPolicy{0};
    uint8_t playbackIsoChannelPolicy{0};
    uint8_t startFlags{0};
    uint8_t prepareOrder[2]{};
    uint8_t startOrder[2]{};
    uint8_t stopFlags{0};
    uint32_t postDeviceEnableDelayMs{0};
    uint32_t defaultNonAudioSlotWord{0};
    uint32_t clockLockTimeoutMs{0};
    uint32_t clockStableReadCount{0};
    uint32_t currentSampleRateHz{0};
    uint32_t hostInputPcmChannels{0};
    uint32_t hostOutputPcmChannels{0};
    uint32_t deviceToHostAm824Slots{0};
    uint32_t hostToDeviceAm824Slots{0};
    uint8_t deviceToHostIsoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint8_t hostToDeviceIsoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint8_t _reserved[2]{};
    Section rates{};
    Section captureStreams{};
    Section playbackStreams{};
    Section timing{};
    Section facets{};
} __attribute__((packed));

struct StreamWireV1 final {
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint8_t _reserved[3]{};
} __attribute__((packed));

struct TimingWireV1 final {
    uint32_t sampleRateHz{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputLatencyFrames{0};
    uint32_t inputSafetyFrames{0};
    uint32_t outputSafetyFrames{0};
    uint32_t rxTransferDelayTicks{0};
    uint32_t txTransferDelayTicks{0};
    uint32_t anchorTimeoutMs{0};
} __attribute__((packed));

struct FacetWireV1 final {
    uint8_t kind{0};
    uint8_t _reserved[3]{};
    uint32_t schemaId{0};
} __attribute__((packed));

enum class WireError : uint8_t {
    TooLarge,
    Overflow,
    Truncated,
    UnsupportedVersion,
    InvalidHeader,
    InvalidSection,
    InvalidCount,
    InvalidValue,
};

[[nodiscard]] std::expected<std::vector<uint8_t>, WireError>
Serialize(const ResolvedAudioEndpointProfile& profile) noexcept;

[[nodiscard]] std::expected<ResolvedAudioEndpointProfile, WireError>
Parse(std::span<const uint8_t> bytes) noexcept;

} // namespace ASFW::Audio::Devices::Wire
