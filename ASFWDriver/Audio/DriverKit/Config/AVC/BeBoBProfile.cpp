// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBProfile.cpp — Per-GUID ADK stream geometry for generic BeBoB devices.
//
// Fresh implementation. Wire geometry cross-validated with Linux
// sound/firewire/bebob/bebob_stream.c; no reference source is copied.

#include "BeBoBProfile.hpp"

#include <algorithm>
#include <limits>

namespace ASFW::Isoch::Audio::AVC::Profiles {

BeBoBProfile::BeBoBProfile(const ::ASFW::Audio::BeBoB::DeviceModel& discoveryModel) {
    supportedRates_ = discoveryModel.SupportedRatesHz();
    const auto selectedRate = discoveryModel.SelectDuplexRateHz();
    if (!selectedRate) {
        return;
    }
    const auto playback = discoveryModel.InputFormationAtRate(*selectedRate);
    const auto capture = discoveryModel.OutputFormationAtRate(*selectedRate);
    if (!playback || !capture) return;

    sampleRateHz_ = *selectedRate;
    txPcmChannels_ = playback->pcmChannels;
    txMidiSlots_ = playback->midiSlots;
    rxPcmChannels_ = capture->pcmChannels;
    rxMidiSlots_ = capture->midiSlots;
}

Encoding::AudioWireFormat BeBoBProfile::TxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

Encoding::AudioWireFormat BeBoBProfile::RxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kAM824;
}

bool BeBoBProfile::BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    if (sampleRateHz_ == 0 || txPcmChannels_ == 0 ||
        txPcmChannels_ > std::numeric_limits<uint8_t>::max() ||
        txMidiSlots_ > std::numeric_limits<uint8_t>::max() ||
        txPcmChannels_ + txMidiSlots_ > std::numeric_limits<uint8_t>::max()) {
        return false;
    }
    outConfig.direction = AudioStreamDirection::HostToDevice;
    outConfig.sampleRate = sampleRateHz_;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.pcmChannels = static_cast<uint8_t>(txPcmChannels_);
    outConfig.dbs = static_cast<uint8_t>(txPcmChannels_ + txMidiSlots_);
    outConfig.midiSlots = static_cast<uint8_t>(txMidiSlots_);
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
    return true;
}

bool BeBoBProfile::BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    if (sampleRateHz_ == 0 || rxPcmChannels_ == 0 ||
        rxPcmChannels_ > std::numeric_limits<uint8_t>::max() ||
        rxMidiSlots_ > std::numeric_limits<uint8_t>::max() ||
        rxPcmChannels_ + rxMidiSlots_ > std::numeric_limits<uint8_t>::max()) {
        return false;
    }
    outConfig.direction = AudioStreamDirection::DeviceToHost;
    outConfig.sampleRate = sampleRateHz_;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.pcmChannels = static_cast<uint8_t>(rxPcmChannels_);
    outConfig.dbs = static_cast<uint8_t>(rxPcmChannels_ + rxMidiSlots_);
    outConfig.midiSlots = static_cast<uint8_t>(rxMidiSlots_);
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
