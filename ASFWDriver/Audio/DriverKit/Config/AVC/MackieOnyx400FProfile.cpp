// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieOnyx400FProfile.cpp - ADK isoch geometry for the Mackie Onyx 400F (Fireworks).

#include "MackieOnyx400FProfile.hpp"

#include "../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

namespace {

// Symmetric 10 x 10 MBLA at 1x (dbs == pcm channels, no MIDI slots). Blocking
// mode is what Linux snd-fireworks uses unconditionally (fireworks_stream.c:
// CIP_BLOCKING | CIP_UNAWARE_SYT), so framesPerDataPacket is SYT_INTERVAL: 8 at
// 44.1 kHz. fdf 0x01 is the AM824 SFC code for 44.1 kHz; fmt 0x10 is AM824.
constexpr uint32_t kPcmChannels = 10;
constexpr uint32_t kSampleRateHz = 44100;

void FillStreamConfig(DICE::DiceStreamConfig& outConfig,
                      DICE::DiceStreamDirection direction) noexcept {
    outConfig = DICE::DiceStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = kSampleRateHz;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = 0;
    outConfig.pcmChannels = kPcmChannels;
    outConfig.dbs = kPcmChannels;
    outConfig.midiSlots = 0;
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x01;
    outConfig.fmt = 0x10;
}

} // namespace

const char* MackieOnyx400FProfile::Name() const noexcept {
    return DeviceProfiles::Audio::kOnyx400FModelName;
}

bool MackieOnyx400FProfile::Matches(const DICE::DiceDeviceIdentity& identity) const noexcept {
    return identity.vendorId == DeviceProfiles::Audio::kMackieVendorId &&
           identity.modelId == DeviceProfiles::Audio::kOnyx400FModelId;
}

DICE::DiceDeviceQuirks MackieOnyx400FProfile::Quirks() const noexcept {
    return DICE::DiceDeviceQuirks{};
}

bool MackieOnyx400FProfile::BuildDefaultTxStreamConfig(
    DICE::DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DICE::DiceStreamDirection::HostToDevice);
    return true;
}

bool MackieOnyx400FProfile::BuildDefaultRxStreamConfig(
    DICE::DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DICE::DiceStreamDirection::DeviceToHost);
    return true;
}

std::vector<uint32_t> MackieOnyx400FProfile::SupportedSampleRates() const {
    // Single rate until the ADK transport reconfiguration supports rate changes
    // for AV/C static-profile devices (see MackieOnyx820iProfile for the
    // field-verified failure chain). Must stay in lockstep with
    // FireworksProtocol::SupportedRates and the published nub config.
    return {kSampleRateHz};
}

AudioStreamTxPolicy MackieOnyx400FProfile::TxStreamPolicy() const noexcept {
    // Fireworks expects a continuous IEC 61883 stream once the connection is
    // up: keep CIP NO-DATA packets flowing while output is idle, the same
    // policy the Onyx-i needed (record-only sessions collapsed without it).
    return AudioStreamTxPolicy{
        .hostToDevicePcmEncoding = Encoding::AudioWireFormat::kAM824,
        .variableDbs = false,
        .defaultNonAudioSlotWord = 0x80000000,
        .initializeNonAudioSlots = true,
        .preserveFdfInNoDataPackets = false,
        .emptyPacketsDuringIdle = true
    };
}

// Safety offsets and latencies start from the Onyx-i field-tuned values
// (192/256/256/256), which trade ~3 ms of reported latency for host-jitter
// headroom on an M3. Re-tune after the first 400F soak.
uint32_t MackieOnyx400FProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 192;
}

uint32_t MackieOnyx400FProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 256;
}

uint32_t MackieOnyx400FProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 256;
}

uint32_t MackieOnyx400FProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 256;
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
