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
    // 44.1 kHz only until the ADK transport reconfiguration supports rate
    // changes for AV/C static-profile devices — offering 48 kHz lets CoreAudio
    // attempt a change that fails at reconfig and leaves a stale
    // session.pendingClock behind (see MackieOnyxProtocol::SupportedRates for
    // the field-verified failure chain). Must stay in lockstep with the
    // runtime protocol's rate set and the published nub config.
    return {44100u};
}

AudioStreamTxPolicy MackieOnyx820iProfile::TxStreamPolicy() const noexcept {
    // Field-verified 2026-08-17: with the default policy (no empty packets), a
    // record-only session leaves the output ring unfed and the TX prep deficit
    // grows monotonically (~3.3k -> 5.7k frames) until the session collapses —
    // five start/stop cycles in a row. Phase88 (the working BeBoB precedent on
    // this same AV/C+CMP base) sends CIP NO-DATA packets while output is idle;
    // the Onyx needs the identical policy.
    return AudioStreamTxPolicy{
        .hostToDevicePcmEncoding = Encoding::AudioWireFormat::kAM824,
        .variableDbs = false,
        .defaultNonAudioSlotWord = 0x80000000,
        .initializeNonAudioSlots = true,
        .preserveFdfInNoDataPackets = false,
        .emptyPacketsDuringIdle = true
    };
}

// Safety offsets and latencies, field-tuned 2026-08-17. The Duet placeholder
// values (64/128/128/128) produced audible in-and-out playback on an 8 GB M3:
// sessions stayed up (200 TxPrepFrame starvation markers, no faults, no
// restarts) but host callback latency spiked to ~922 us against a 64-frame
// (~1.45 ms) TX safety offset, with prep margin dipping to 120 frames. The
// widened values trade ~3 ms of reported latency for jitter headroom.
uint32_t MackieOnyx820iProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 192;
}

uint32_t MackieOnyx820iProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 256;
}

uint32_t MackieOnyx820iProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 256;
}

uint32_t MackieOnyx820iProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 256;
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
