// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioSpecialProfile.hpp"

#include "../../../../Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"

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

uint32_t SafetyFrames(double rate) noexcept {
    // The shared vendor safety table is one millisecond of frames at each rate
    // (tmp/midi-branch/documentation/1814.md:426-433).
    return static_cast<uint32_t>((rate + 500.0) / 1000.0);
}

uint32_t RoundTripFrames(bool projectMix, double rate) noexcept {
    // Values are transcribed from the vendor tables
    // (tmp/midi-branch/documentation/1814.md:426-433; the 1814 table is also
    // recorded in docs/MAUDIO_1814_KEXT_RE.md:391). Keep FDF bands distinct:
    // collapsing 2x into the 48 kHz value under-reports CoreAudio latency.
    if (rate >= 95000.0) return projectMix ? 366U : 378U;
    if (rate >= 87500.0) return projectMix ? 342U : 358U;
    if (rate >= 47000.0) return projectMix ? 208U : 225U;
    return projectMix ? 196U : 210U;
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
    // ADK keeps channel geometry fixed for the lifetime of the published
    // streams. S/PDIF has the same 10-in/6-out formation at 44.1/48 and
    // 88.2/96 kHz, so these rates are safe to advertise. The 1814's
    // 176.4/192 kHz formation is 2-in/4-out and needs atomic HAL geometry
    // rebuilding before it can be exposed. ProjectMix does not support those
    // rates in its reference rate list.
    return {44100U, 48000U, 88200U, 96000U};
}

bool MAudioSpecialProfile::ConfigureStreamRate(
    AudioStreamConfig& config, uint32_t sampleRateHz) noexcept {
    if (sampleRateHz != 44100U && sampleRateHz != 48000U &&
        sampleRateHz != 88200U && sampleRateHz != 96000U) {
        return false;
    }
    const auto geometry = ASFW::Encoding::AmdtpRateGeometryForSampleRate(sampleRateHz);
    if (!geometry) {
        return false;
    }
    config.sampleRate = sampleRateHz;
    config.fdf = geometry->fdf;
    config.framesPerDataPacket = static_cast<uint8_t>(geometry->sytIntervalFrames);
    return true;
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
