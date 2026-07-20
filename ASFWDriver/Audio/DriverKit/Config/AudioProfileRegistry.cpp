// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.cpp
// Global profile registry dispatcher.

#include "AudioProfileRegistry.hpp"
#include "AudioStreamProfile.hpp"
#include "AVC/ApogeeDuetProfile.hpp"
#include "AVC/Phase88Profile.hpp"
#include "DICE/DiceProfileRegistry.hpp"

#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace {

class RMEFireface800Stage3Profile final : public ASFW::Isoch::Audio::IAudioStreamProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override {
        return "RME Fireface 800 (Stage 5C)";
    }

    [[nodiscard]] ASFW::Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return ASFW::Encoding::AudioWireFormat::kRawPcm24In32;
    }

    [[nodiscard]] ASFW::Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return ASFW::Encoding::AudioWireFormat::kRawPcm24In32;
    }

    [[nodiscard]] uint32_t TxChannelCount() const noexcept override { return 12; }
    [[nodiscard]] uint32_t RxChannelCount() const noexcept override { return 12; }
    [[nodiscard]] uint32_t TxMidiSlots() const noexcept override { return 0; }
    [[nodiscard]] uint32_t RxMidiSlots() const noexcept override { return 0; }
    [[nodiscard]] uint32_t TxDbs() const noexcept override { return 12; }
    [[nodiscard]] uint32_t RxDbs() const noexcept override { return 12; }

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override {
        (void)sampleRate;
        return 64;
    }

    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override {
        (void)sampleRate;
        return 64;
    }

    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override {
        (void)sampleRate;
        return 128;
    }

    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override {
        (void)sampleRate;
        return 128;
    }

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override {
        // Stage 5C intentionally publishes only the hardware-verified current mode.
        return {192000u};
    }

    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        ASFW::Isoch::Audio::AudioStreamConfig& outConfig) const noexcept override {
        outConfig = {};
        outConfig.direction = ASFW::Isoch::Audio::AudioStreamDirection::HostToDevice;
        outConfig.sampleRate = 192000u;
        outConfig.streamMode = ASFW::Encoding::StreamMode::kBlocking;
        outConfig.sid = 0u;
        outConfig.pcmChannels = 12u;
        outConfig.dbs = 12u;
        outConfig.midiSlots = 0u;
        outConfig.framesPerDataPacket = 32u;
        outConfig.fdf = 0u;
        outConfig.fmt = 0u;
        outConfig.includeCipHeader = false;
        return true;
    }

    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        ASFW::Isoch::Audio::AudioStreamConfig& outConfig) const noexcept override {
        outConfig = {};
        outConfig.direction = ASFW::Isoch::Audio::AudioStreamDirection::DeviceToHost;
        outConfig.sampleRate = 192000u;
        outConfig.streamMode = ASFW::Encoding::StreamMode::kBlocking;
        outConfig.sid = 0u;
        outConfig.pcmChannels = 12u;
        outConfig.dbs = 12u;
        outConfig.midiSlots = 0u;
        outConfig.framesPerDataPacket = 32u;
        outConfig.fdf = 0u;
        outConfig.fmt = 0u;
        outConfig.includeCipHeader = false;
        return true;
    }

    [[nodiscard]] ASFW::Isoch::Audio::AudioStreamTxPolicy
    TxStreamPolicy() const noexcept override {
        ASFW::Isoch::Audio::AudioStreamTxPolicy policy{};
        policy.hostToDevicePcmEncoding = ASFW::Encoding::AudioWireFormat::kRawPcm24In32;
        policy.variableDbs = false;
        policy.initializeNonAudioSlots = false;
        policy.preserveFdfInNoDataPackets = false;
        policy.emptyPacketsDuringIdle = true;
        return policy;
    }
};

} // namespace

namespace ASFW::Isoch::Audio {

const IAudioDeviceProfile* AudioProfileRegistry::FindProfile(uint32_t vendorId,
                                                             uint32_t modelId,
                                                             uint64_t guid) noexcept {
    static AVC::Profiles::ApogeeDuetProfile apogeeDuetProfile{};
    if (vendorId == DeviceProfiles::Audio::kApogeeVendorId &&
        modelId == DeviceProfiles::Audio::kApogeeDuetModelId) {
        return &apogeeDuetProfile;
    }

    static AVC::Profiles::Phase88Profile phase88Profile{};
    if (vendorId == DeviceProfiles::Audio::kTerraTecVendorId &&
        modelId == DeviceProfiles::Audio::kPhase88RackFwModelId) {
        return &phase88Profile;
    }

    static RMEFireface800Stage3Profile fireface800Profile{};
    if (vendorId == DeviceProfiles::Audio::kRMEVendorId &&
        modelId == DeviceProfiles::Audio::kFireface800ModelId) {
        return &fireface800Profile;
    }

    // Map identity to the DICE family structures
    DICE::DiceDeviceIdentity identity{
        .guid = guid,
        .vendorId = vendorId,
        .modelId = modelId
    };

    // Instantiate/access the DICE registry singleton.
    // The constructor of DiceProfileRegistry pre-populates all known DICE profiles.
    static DICE::DiceProfileRegistry diceRegistry{};

    if (const auto* profile = diceRegistry.FindProfile(identity)) {
        return profile;
    }

    // The generic profile is a DICE fallback only. Exact AV/C adapters above
    // must never inherit its name or wire geometry.
    return diceRegistry.GenericProfile();
}

} // namespace ASFW::Isoch::Audio
