// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FocusriteSaffireProfile.cpp
// Focusrite Saffire specific profile.

#include "FocusriteSaffireProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {

constexpr uint32_t kFocusriteVendorId = 0x00130E;

void FillDefaultStreamConfig(DiceStreamConfig& outConfig,
                             DiceStreamDirection direction) noexcept {
    outConfig = DiceStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = 48000;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = 0;
    if (direction == DiceStreamDirection::HostToDevice) {
        outConfig.pcmChannels = 8;
        outConfig.midiSlots = 1;
        outConfig.dbs = 9;
    } else {
        outConfig.pcmChannels = 16;
        outConfig.midiSlots = 1;
        outConfig.dbs = 17;
    }
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
}

} // namespace

const char* FocusriteSaffireProfile::Name() const noexcept {
    return "Focusrite Saffire (DICE)";
}

bool FocusriteSaffireProfile::Matches(const DiceDeviceIdentity& identity) const noexcept {
    return identity.vendorId == kFocusriteVendorId;
}

DiceDeviceQuirks FocusriteSaffireProfile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    quirks.tx.hostToDevicePcmEncoding = Encoding::AudioWireFormat::kRawPcm24In32;
    quirks.tx.dbsPolicy = DbsPolicy::Constant;
    quirks.tx.defaultNonAudioSlotWord = 0x80000000;
    quirks.tx.initializeNonAudioSlots = true;
    quirks.tx.preserveFdfInNoDataPackets = true;
    quirks.rx.deviceToHostPcmEncoding = Encoding::AudioWireFormat::kAM824;
    quirks.rx.dbsPolicy = DbsPolicy::Constant;
    return quirks;
}

bool FocusriteSaffireProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool FocusriteSaffireProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

// Transmit (playback) safety offset is configured to be smaller (6 packets) to optimize
// latency while preventing audio dropouts under normal CPU/thread scheduling.
uint32_t FocusriteSaffireProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    uint32_t framesPerPacket = 8;
    uint32_t rateAddend = 0;
    
    // Safety offset scales with sample rate. Frames per packet is 8 for <= 48kHz,
    // 16 for 96kHz, and 32 for 192kHz.
    if (sampleRate > 96000.0) {
        framesPerPacket = 32;
        rateAddend = 4;
    } else if (sampleRate > 48000.0) {
        framesPerPacket = 16;
        rateAddend = 2;
    }

    // Default to latencyMode = 1 (safe medium latency mode).
    // Mode 1: Output delay is 6 packets + rate-based offset.
    const uint32_t delayPackets = 6 + rateAddend;
    return delayPackets * framesPerPacket;
}

// Receive (capture) safety offset is configured to be larger (16 packets) to handle
// FireWire packet reception jitter and asynchronous processing overhead.
uint32_t FocusriteSaffireProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    uint32_t framesPerPacket = 8;
    uint32_t rateAddend = 0;
    
    if (sampleRate > 96000.0) {
        framesPerPacket = 32;
        rateAddend = 4;
    } else if (sampleRate > 48000.0) {
        framesPerPacket = 16;
        rateAddend = 2;
    }

    // Input (Rx) uses 16 packets + rate-based offset.
    const uint32_t delayPackets = 16 + rateAddend;
    return delayPackets * framesPerPacket;
}

// The reported latencies in frames matching the focusrite saffire kext model.
uint32_t FocusriteSaffireProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    uint32_t ladder = 29;
    if (sampleRate > 96000.0) {
        ladder = 119;
    } else if (sampleRate > 48000.0) {
        ladder = 59;
    }
    return ladder;
}

uint32_t FocusriteSaffireProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    uint32_t ladder = 29;
    if (sampleRate > 96000.0) {
        ladder = 119;
    } else if (sampleRate > 48000.0) {
        ladder = 59;
    }
    return ladder;
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
