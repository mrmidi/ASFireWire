// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Engine/Direct/Rx/RxCaptureChannelMap.hpp"

#include "AudioIdentity.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../Duplex/DuplexPolicies.hpp"
#include "../Protocols/AudioTypes.hpp"
#include "../Shared/Configuration/DeviceConfiguration.hpp"
#include "../Wire/AMDTP/AmdtpTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ASFW::Audio::Devices {

inline constexpr size_t kMaxResolvedRates = 8;
inline constexpr size_t kMaxConfigurationCapabilities = 8;

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
    /// See AmdtpTxPolicy::substituteSilenceOnPcmUnavailable.
    bool substituteSilenceOnPcmUnavailable{false};

    /// Send cadence packets full-size, carrying data blocks whose audio slots
    /// hold a no-audio label, rather than header-only. Required by the M-Audio
    /// "special" firmware; see AmdtpTxPolicy::cadencePacketsCarryDataBlocks for
    /// the wire evidence. The label itself is not plumbed: it stays a constant
    /// in the packetizer until a second device needs a different one.
    bool cadencePacketsCarryDataBlocks{false};
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

// An immutable, endpoint-specific projection of one semantic configuration.
// The coordinator selects records from this envelope; it never infers stream
// geometry from control values. This keeps vendor protocol semantics out of
// the DriverKit side and makes maximum direct-memory capacity calculable before
// the nub is published.
struct ConfigurationCapabilityRecord final {
    Configuration::DeviceConfiguration configuration{};
    AudioStreamRuntimeCaps runtimeCaps{};
    /// Capture presentation can change with the selected formation.  Most
    /// devices leave this identity, while the M-Audio special firmware uses a
    /// 10- or 16-channel planar-slot map depending on its optical input mode.
    AudioEngine::Direct::Rx::RxCaptureChannelMap captureChannelMap{};
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
    std::array<ConfigurationCapabilityRecord, kMaxConfigurationCapabilities>
        configurationCapabilities{};
    uint8_t configurationCapabilityCount{0};

    Encoding::AudioWireFormat captureWireFormat{Encoding::AudioWireFormat::kAM824};
    Encoding::AudioWireFormat playbackWireFormat{Encoding::AudioWireFormat::kAM824};
    // Set by the family builder for devices whose capture slot order is not the
    // channel order. Identity everywhere else; the duplex coordinator forwards
    // it without interpreting it.
    AudioEngine::Direct::Rx::RxCaptureChannelMap captureChannelMap{};
    // Host PCM channel to device-facing AM824 slot mapping. This crosses the
    // nub seam because the AudioDriverKit packetizer owns playback encoding.
    ::ASFW::Audio::Wire::PcmSlotMap playbackChannelMap{};
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

    // Device-reported channel names from DICE TX/RX name sections.
    // In-memory only — not wire-serialized; carried to the nub by
    // AudioNubPublisher for IORegistry channel name properties.
    std::vector<std::string> deviceInputChannelNames;
    std::vector<std::string> deviceOutputChannelNames;

    [[nodiscard]] const RateTimingPolicy* TimingFor(uint32_t sampleRateHz) const noexcept {
        for (uint8_t i = 0; i < timingCount; ++i) {
            if (timing[i].sampleRateHz == sampleRateHz) {
                return &timing[i];
            }
        }
        return timingCount != 0 ? &timing[0] : nullptr;
    }

    [[nodiscard]] const ConfigurationCapabilityRecord* ConfigurationFor(
        const Configuration::DeviceConfiguration& configuration) const noexcept {
        const uint8_t count = std::min(configurationCapabilityCount,
                                       static_cast<uint8_t>(configurationCapabilities.size()));
        for (uint8_t i = 0; i < count; ++i) {
            const auto& candidate = configurationCapabilities[i];
            if (candidate.configuration.sampleRate == configuration.sampleRate &&
                candidate.configuration.opticalInput == configuration.opticalInput &&
                candidate.configuration.opticalOutput == configuration.opticalOutput) {
                return &candidate;
            }
        }
        return nullptr;
    }

    /// Returns the map whose declared geometry exactly matches the active
    /// runtime caps.  This is intentionally capability-owned rather than a
    /// profile-wide constant: a formation change may change capture width and
    /// therefore which fixed slot table is safe to apply.
    [[nodiscard]] AudioEngine::Direct::Rx::RxCaptureChannelMap
    CaptureChannelMapForRuntimeCaps(const AudioStreamRuntimeCaps& caps) const noexcept {
        const uint8_t count = std::min(configurationCapabilityCount,
                                       static_cast<uint8_t>(configurationCapabilities.size()));
        for (uint8_t i = 0; i < count; ++i) {
            const auto& candidate = configurationCapabilities[i].runtimeCaps;
            if (candidate.sampleRateHz == caps.sampleRateHz &&
                candidate.hostInputPcmChannels == caps.hostInputPcmChannels &&
                candidate.hostOutputPcmChannels == caps.hostOutputPcmChannels &&
                candidate.deviceToHostAm824Slots == caps.deviceToHostAm824Slots &&
                candidate.hostToDeviceAm824Slots == caps.hostToDeviceAm824Slots) {
                return configurationCapabilities[i].captureChannelMap;
            }
        }
        // Non-configurable devices and a failed/unknown configuration retain
        // the safe initial map. The decoder still validates it against live DBS
        // before using it.
        return captureChannelMap;
    }
};

} // namespace ASFW::Audio::Devices
