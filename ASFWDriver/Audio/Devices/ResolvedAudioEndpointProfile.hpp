// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AudioIdentity.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../Duplex/DuplexPolicies.hpp"
#include "../Protocols/AudioTypes.hpp"
#include "../Wire/AMDTP/AmdtpTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ASFW::Audio::Devices {

inline constexpr size_t kMaxResolvedRates = 8;

enum class StreamModePolicy : uint8_t { NonBlocking, Blocking };

enum class FacetKind : uint8_t {
    Clock = 0,
    Routing,
    Mixer,
    Parameters,
    VendorControl,
};

struct FacetDescriptor final {
    FacetKind kind{FacetKind::Clock};
    uint32_t schemaId{0};
};

struct RateTimingPolicy final {
    uint32_t sampleRateHz{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputLatencyFrames{0};
    uint32_t inputSafetyFrames{0};
    uint32_t outputSafetyFrames{0};
    uint32_t rxTransferDelayTicks{12800};
    uint32_t txTransferDelayTicks{12800};
    uint32_t anchorTimeoutMs{500};
};

struct TxPacketPolicy final {
    bool variableDbs{false};
    uint32_t defaultNonAudioSlotWord{0x80000000U};
    bool initializeNonAudioSlots{true};
    bool preserveFdfInNoDataPackets{false};
    bool emptyPacketsDuringIdle{false};
    bool advanceDbcOnNoData{false};
};

struct RecoveryPolicy final {
    bool recoverAfterTimingLoss{true};
    bool recoverAfterCycleInconsistent{true};
    bool recoverAfterLockLoss{true};
    uint8_t maximumAttempts{3};
};

struct ClockPolicy final {
    bool hostRateChangeSupported{true};
    uint32_t lockTimeoutMs{1000};
    uint32_t stableReadCount{3};
};

// Immutable output of static catalog resolution plus a safe family probe.
// Neither DriverKit nor the duplex planner performs identity matching after
// this object is built.
struct ResolvedAudioEndpointProfile final {
    AudioEndpointId endpointId{};
    Discovery::DeviceInstanceId deviceInstanceId{};
    Discovery::UnitInstanceId unitInstanceId{};
    uint64_t observedGuid{0}; // diagnostics only

    DeviceProfiles::Audio::DeviceDefinitionId definitionId{
        DeviceProfiles::Audio::DeviceDefinitionId::Unknown};
    std::optional<uint32_t> exactVariantId;
    std::optional<uint32_t> equivalenceClassId;
    DeviceProfiles::Audio::AudioFamilyProviderId familyProvider{
        DeviceProfiles::Audio::AudioFamilyProviderId::None};
    DeviceProfiles::Audio::ProfileBuilderId profileBuilder{
        DeviceProfiles::Audio::ProfileBuilderId::None};
    std::vector<DeviceProfiles::Audio::DeviceDefinitionId> candidateDefinitionIds;
    std::vector<DeviceProfiles::Audio::MatchProvenance> matchProvenance;

    std::string vendorName;
    std::string deviceName{"FireWire Audio"};
    PersistentDeviceKey persistentKey{};
    PersistentIdentityStatus identityStatus{PersistentIdentityStatus::Ephemeral};
    std::string coreAudioUid;

    std::array<uint32_t, kMaxResolvedRates> supportedRates{};
    uint8_t supportedRateCount{0};
    uint32_t currentSampleRateHz{48000};
    AudioStreamRuntimeCaps runtimeCaps{};

    Encoding::AudioWireFormat captureWireFormat{Encoding::AudioWireFormat::kAM824};
    Encoding::AudioWireFormat playbackWireFormat{Encoding::AudioWireFormat::kAM824};
    uint8_t captureFramesPerDataPacket{8};
    uint8_t playbackFramesPerDataPacket{8};
    uint8_t captureFdf{0x02};
    uint8_t playbackFdf{0x02};
    uint8_t captureFmt{0x10};
    uint8_t playbackFmt{0x10};
    TxPacketPolicy txPacketPolicy{};
    StreamModePolicy streamMode{StreamModePolicy::Blocking};
    Duplex::IsoChannelPolicy captureIsoChannelPolicy{Duplex::IsoChannelPolicy::Fixed};
    Duplex::IsoChannelPolicy playbackIsoChannelPolicy{Duplex::IsoChannelPolicy::Fixed};
    Duplex::StartPolicy startPolicy{};
    Duplex::StopPolicy stopPolicy{};
    RecoveryPolicy recoveryPolicy{};
    ClockPolicy clockPolicy{};

    std::array<RateTimingPolicy, kMaxResolvedRates> timing{};
    uint8_t timingCount{0};
    std::vector<FacetDescriptor> facets;

    [[nodiscard]] const RateTimingPolicy* TimingFor(uint32_t sampleRateHz) const noexcept {
        for (uint8_t i = 0; i < timingCount; ++i) {
            if (timing[i].sampleRateHz == sampleRateHz) {
                return &timing[i];
            }
        }
        return timingCount != 0 ? &timing[0] : nullptr;
    }
};

} // namespace ASFW::Audio::Devices
