// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieOnyx820iProfile.cpp - ADK isoch geometry for the Mackie Onyx 820i (Oxford run).

#include "MackieOnyx820iProfile.hpp"

#include "../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

namespace {

// Asymmetric duplex, from the live 820i capture: capture 8ch MBLA, playback 2ch
// MBLA (dbs == pcm channels: MBLA-only streams, no MIDI slots). Blocking mode is
// the vendor-wide Loud rule (Linux snd-oxfw oxfw.c:189-196), so
// framesPerDataPacket is SYT_INTERVAL: 8 at 44.1 kHz. fdf 0x01 is the AM824 SFC
// code for 44.1 kHz (the device's captured current rate); fmt 0x10 is AM824.
void FillStreamConfig(DICE::DiceStreamConfig& outConfig,
                      DICE::DiceStreamDirection direction,
                      uint32_t pcmChannels) noexcept {
    outConfig = DICE::DiceStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = 44100;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = 0;
    outConfig.pcmChannels = pcmChannels;
    outConfig.dbs = pcmChannels;
    outConfig.midiSlots = 0;
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x01;
    outConfig.fmt = 0x10;
}

} // namespace

const char* MackieOnyx820iProfile::Name() const noexcept {
    return DeviceProfiles::Audio::kOnyxIOxfwModelName;
}

bool MackieOnyx820iProfile::Matches(const DICE::DiceDeviceIdentity& identity) const noexcept {
    return identity.vendorId == DeviceProfiles::Audio::kMackieVendorId &&
           identity.modelId == DeviceProfiles::Audio::kOnyxIOxfwModelId;
}

DICE::DiceDeviceQuirks MackieOnyx820iProfile::Quirks() const noexcept {
    return DICE::DiceDeviceQuirks{};
}

bool MackieOnyx820iProfile::BuildDefaultTxStreamConfig(
    DICE::DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DICE::DiceStreamDirection::HostToDevice, 2);
    return true;
}

bool MackieOnyx820iProfile::BuildDefaultRxStreamConfig(
    DICE::DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DICE::DiceStreamDirection::DeviceToHost, 8);
    return true;
}

std::vector<uint32_t> MackieOnyx820iProfile::SupportedSampleRates() const {
    // The device advertises 44.1/48/88.2/96 kHz, but until the AV/C SIGNAL FORMAT
    // rate transition is wired for this device (M4), only the captured current
    // rate is offered — same policy as the Duet profile: do not let CoreAudio
    // construct a device the bring-up path must then move underneath it.
    return {44100u};
}

// Safety-offset and latency figures mirror the Duet profile's validated values as
// placeholders; they are unvalidated for the Onyx until real streaming (M2/M3)
// and must be re-measured then.
uint32_t MackieOnyx820iProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 64;
}

uint32_t MackieOnyx820iProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

uint32_t MackieOnyx820iProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

uint32_t MackieOnyx820iProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
