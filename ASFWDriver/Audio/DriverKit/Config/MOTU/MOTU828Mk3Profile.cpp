// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MOTU828Mk3Profile.hpp"

#include "../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../Protocols/MOTU/MOTU828Mk3Geometry.hpp"

namespace ASFW::Isoch::Audio::MOTU::Profiles {
namespace {

constexpr uint32_t kEnabledSampleRateHz = 48000;

bool FillStreamConfig(AudioStreamConfig& outConfig, AudioStreamDirection direction) noexcept {
    const auto geometry = ASFW::Audio::MOTU::Build828Mk3Geometry(kEnabledSampleRateHz);
    if (!geometry.has_value()) {
        return false;
    }

    outConfig = AudioStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = geometry->sampleRateHz;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = direction == AudioStreamDirection::HostToDevice ? 0 : 1;
    outConfig.pcmChannels = static_cast<uint8_t>(direction == AudioStreamDirection::HostToDevice
                                                     ? geometry->hostToDevicePcm
                                                     : geometry->deviceToHostPcm);
    outConfig.dbs = static_cast<uint8_t>(direction == AudioStreamDirection::HostToDevice
                                             ? geometry->hostToDeviceDbs
                                             : geometry->deviceToHostDbs);
    outConfig.midiSlots = 0;
    outConfig.framesPerDataPacket = static_cast<uint8_t>(geometry->framesPerDataPacket);
    outConfig.fdf = 0x22;
    // FMT is the captured V3 wire marker, not the generic IEC 61883-6 AM824
    // identifier (0x10). The official driver stores the second CIP quadlet as
    // a literal whose wire form is 0x8222ffff -> FMT=0x02, and the Sequoia bus
    // capture shows the same value in both steady state and the seed window.
    // Same reasoning as MotuV3Wire::BuildCipQ0: keep the observed marker rather
    // than the value the standard would imply for this proprietary layout.
    outConfig.fmt = 0x02;
    // The captured header carries 0x04 in the FN/QPC/SPH octet, i.e. SPH set, and the
    // FDF 0x22 is a MOTU marker, not an AM824 SFC to re-derive per rate. With both, the
    // generic CIP builder emits MotuV3Wire::BuildCipQ0/Q1 byte for byte.
    outConfig.cipSph = true;
    outConfig.fdfIsFixed = true;
    return true;
}

} // namespace

const char* MOTU828Mk3Profile::Name() const noexcept {
    return DeviceProfiles::Audio::kMotu828Mk3ModelName;
}

Encoding::AudioWireFormat MOTU828Mk3Profile::TxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kMotuV3Packed;
}

Encoding::AudioWireFormat MOTU828Mk3Profile::RxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kMotuV3Packed;
}

bool MOTU828Mk3Profile::BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    return FillStreamConfig(outConfig, AudioStreamDirection::HostToDevice);
}

bool MOTU828Mk3Profile::BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    return FillStreamConfig(outConfig, AudioStreamDirection::DeviceToHost);
}

AudioStreamTxPolicy MOTU828Mk3Profile::TxStreamPolicy() const noexcept {
    return AudioStreamTxPolicy{
        .hostToDevicePcmEncoding = Encoding::AudioWireFormat::kMotuV3Packed,
        .variableDbs = false,
        .defaultNonAudioSlotWord = 0,
        .initializeNonAudioSlots = false,
        .preserveFdfInNoDataPackets = true,
        .emptyPacketsDuringIdle = false,
        // Captured CoreAudio-to-wire order: Main L/R on host channels 1-2.
        .motuPlaybackPorts = Encoding::Motu::kMk3Playback,
    };
}

std::vector<uint32_t> MOTU828Mk3Profile::SupportedSampleRates() const {
    // The pure geometry model covers all six rates. Runtime activation remains
    // deliberately limited to the captured 48 kHz path until AudioStreamConfig
    // can consume the protocol's live per-rate DBS instead of a static default.
    return {kEnabledSampleRateHz};
}

uint32_t MOTU828Mk3Profile::TxSafetyOffsetFrames(double) const noexcept { return 64; }
uint32_t MOTU828Mk3Profile::RxSafetyOffsetFrames(double) const noexcept { return 64; }
uint32_t MOTU828Mk3Profile::TxReportedLatencyFrames(double) const noexcept { return 128; }
uint32_t MOTU828Mk3Profile::RxReportedLatencyFrames(double) const noexcept { return 128; }

} // namespace ASFW::Isoch::Audio::MOTU::Profiles
