// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBProfile.cpp — Per-GUID ADK stream geometry for generic BeBoB devices.
//
// Fresh implementation. Wire geometry cross-validated with Linux
// sound/firewire/bebob/bebob_stream.c; no reference source is copied.

#include "BeBoBProfile.hpp"

#include <algorithm>

namespace ASFW::Isoch::Audio::AVC::Profiles {
namespace {

uint32_t RateCodeToHz(uint8_t code) noexcept {
    switch (code) {
        case 0x00: return 32000U;
        case 0x01: return 44100U;
        case 0x02: return 48000U;
        case 0x03: return 88200U;
        case 0x04: return 96000U;
        case 0x05: return 176400U;
        case 0x06: return 192000U;
        case 0x0A: return 88200U;
        default: return 0U;
    }
}

} // namespace

BeBoBProfile::BeBoBProfile(const BeBoB::DeviceModel& discoveryModel) {
    // Derive geometry from the first duplex formation.
    if (!discoveryModel.input.supportedFormations.empty()) {
        const auto& formation = discoveryModel.input.supportedFormations[0];
        pcmChannels_ = formation.pcmChannels;
        midiSlots_ = formation.midiSlots;
        sampleRateHz_ = RateCodeToHz(formation.rateCode);
    }

    // Collect all supported rates from both directions.
    bool seen[128] = {};
    for (const auto& formation : discoveryModel.input.supportedFormations) {
        const uint32_t hz = RateCodeToHz(formation.rateCode);
        if (hz > 0 && !seen[formation.rateCode]) {
            seen[formation.rateCode] = true;
            supportedRates_.push_back(hz);
        }
    }
    for (const auto& formation : discoveryModel.output.supportedFormations) {
        const uint32_t hz = RateCodeToHz(formation.rateCode);
        if (hz > 0 && !seen[formation.rateCode]) {
            seen[formation.rateCode] = true;
            supportedRates_.push_back(hz);
        }
    }
    if (sampleRateHz_ == 0 && !supportedRates_.empty()) {
        sampleRateHz_ = supportedRates_[0];
    }
}

Encoding::AudioWireFormat BeBoBProfile::TxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

Encoding::AudioWireFormat BeBoBProfile::RxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

bool BeBoBProfile::BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    outConfig.direction = AudioStreamDirection::HostToDevice;
    outConfig.sampleRate = sampleRateHz_;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.pcmChannels = static_cast<uint8_t>(pcmChannels_);
    outConfig.dbs = static_cast<uint8_t>(pcmChannels_ + midiSlots_);
    outConfig.midiSlots = static_cast<uint8_t>(midiSlots_);
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
    return true;
}

bool BeBoBProfile::BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    outConfig.direction = AudioStreamDirection::DeviceToHost;
    outConfig.sampleRate = sampleRateHz_;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.pcmChannels = static_cast<uint8_t>(pcmChannels_);
    outConfig.dbs = static_cast<uint8_t>(pcmChannels_ + midiSlots_);
    outConfig.midiSlots = static_cast<uint8_t>(midiSlots_);
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
    return true;
}

std::vector<uint32_t> BeBoBProfile::SupportedSampleRates() const {
    return supportedRates_;
}

uint32_t BeBoBProfile::TxSafetyOffsetFrames(double /*sampleRate*/) const noexcept {
    return 64;
}

uint32_t BeBoBProfile::RxSafetyOffsetFrames(double /*sampleRate*/) const noexcept {
    return 64;
}

uint32_t BeBoBProfile::TxReportedLatencyFrames(double /*sampleRate*/) const noexcept {
    return 128;
}

uint32_t BeBoBProfile::RxReportedLatencyFrames(double /*sampleRate*/) const noexcept {
    return 128;
}

AudioStreamTxPolicy BeBoBProfile::TxStreamPolicy() const noexcept {
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
