// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Phase88Profile.cpp - ADK isoch geometry for TerraTec PHASE 88 Rack FW BeBoB.

#include "Phase88Profile.hpp"

#include "../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {
namespace {

constexpr uint32_t kPhase88SampleRateHz = 48000;
constexpr uint32_t kPhase88PcmChannels = 10;
constexpr uint32_t kPhase88MidiSlots = 1;
constexpr uint32_t kPhase88Dbs = kPhase88PcmChannels + kPhase88MidiSlots;
constexpr uint32_t kPhase88FramesPerPacket = 8;

void FillStreamConfig(AudioStreamConfig& outConfig, AudioStreamDirection direction) noexcept {
    outConfig = AudioStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = kPhase88SampleRateHz;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = 0;
    outConfig.pcmChannels = kPhase88PcmChannels;
    outConfig.midiSlots = kPhase88MidiSlots;
    outConfig.dbs = kPhase88Dbs;
    outConfig.framesPerDataPacket = kPhase88FramesPerPacket;
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
}

} // namespace

const char* Phase88Profile::Name() const noexcept {
    return DeviceProfiles::Audio::kPhase88RackFwModelName;
}

Encoding::AudioWireFormat Phase88Profile::TxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

Encoding::AudioWireFormat Phase88Profile::RxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

bool Phase88Profile::BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    // The live BridgeCo formation reports ten PCM blocks plus one AM824 MIDI
    // block in each direction (DBS 11). This is an ADK allocator profile only;
    // FCP and CMP remain owned by the BeBoB protocol adapter.
    FillStreamConfig(outConfig, AudioStreamDirection::HostToDevice);
    return true;
}

bool Phase88Profile::BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, AudioStreamDirection::DeviceToHost);
    return true;
}

std::vector<uint32_t> Phase88Profile::SupportedSampleRates() const {
    return {kPhase88SampleRateHz};
}

uint32_t Phase88Profile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 64;
}

uint32_t Phase88Profile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 64;
}

uint32_t Phase88Profile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

uint32_t Phase88Profile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
