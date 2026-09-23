// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioSpecialProfile.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {
namespace {

constexpr uint32_t kInitialRateHz = 48000;

void Fill(AudioStreamConfig& out, AudioStreamDirection direction,
          uint8_t pcmChannels) noexcept {
    out = {};
    out.direction = direction;
    out.sampleRate = kInitialRateHz;
    out.streamMode = Encoding::StreamMode::kBlocking;
    out.pcmChannels = pcmChannels;
    out.midiSlots = 1;
    out.dbs = static_cast<uint8_t>(pcmChannels + 1);
    out.framesPerDataPacket = 8;
    out.fdf = 0x02;
    out.fmt = 0x10;
}

uint32_t SafetyFrames(double) noexcept {
    return 48U;
}

uint32_t RoundTripFrames(bool projectMix, double) noexcept {
    // 48 kHz vendor values: midi-branch/documentation/1814.md:426-433.
    return projectMix ? 208U : 225U;
}

} // namespace

const char* MAudioSpecialProfile::Name() const noexcept {
    return projectMix_ ? "M-Audio ProjectMix I/O" : "M-Audio FireWire 1814";
}

Encoding::AudioWireFormat MAudioSpecialProfile::TxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

Encoding::AudioWireFormat MAudioSpecialProfile::RxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

bool MAudioSpecialProfile::BuildDefaultTxStreamConfig(AudioStreamConfig& out) const noexcept {
    Fill(out, AudioStreamDirection::HostToDevice, 6);
    return true;
}

bool MAudioSpecialProfile::BuildDefaultRxStreamConfig(AudioStreamConfig& out) const noexcept {
    Fill(out, AudioStreamDirection::DeviceToHost, 10);
    return true;
}

std::vector<uint32_t> MAudioSpecialProfile::SupportedSampleRates() const {
    // FW-255 validates the special-firmware path at its fixed 48 kHz rate.
    return {48000U};
}

uint32_t MAudioSpecialProfile::TxSafetyOffsetFrames(double rate) const noexcept {
    return SafetyFrames(rate);
}

uint32_t MAudioSpecialProfile::RxSafetyOffsetFrames(double rate) const noexcept {
    return SafetyFrames(rate);
}

uint32_t MAudioSpecialProfile::TxReportedLatencyFrames(double rate) const noexcept {
    return RoundTripFrames(projectMix_, rate) / 2U;
}

uint32_t MAudioSpecialProfile::RxReportedLatencyFrames(double rate) const noexcept {
    return (RoundTripFrames(projectMix_, rate) + 1U) / 2U;
}

AudioStreamTxPolicy MAudioSpecialProfile::TxStreamPolicy() const noexcept {
    return AudioStreamTxPolicy{
        .hostToDevicePcmEncoding = Encoding::AudioWireFormat::kAM824,
        .variableDbs = false,
        .defaultNonAudioSlotWord = 0x80000000,
        .initializeNonAudioSlots = true,
        .preserveFdfInNoDataPackets = false,
        .emptyPacketsDuringIdle = false,
    };
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
