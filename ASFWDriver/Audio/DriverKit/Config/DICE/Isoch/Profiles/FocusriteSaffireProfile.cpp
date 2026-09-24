// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FocusriteSaffireProfile.cpp
// Focusrite Saffire specific profile.

#include "FocusriteSaffireProfile.hpp"
#include "../../../TimingLadder.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {

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

// Captured from Focusrite GUID 0x00130E0401405B54 (TCD2220) at 48 kHz:
// playback 12 PCM + MIDI, then 8 PCM; capture 10 PCM + MIDI, then 10 PCM.
// Cross-checked with FFADO src/dice/focusrite/saffire_pro40.cpp:50-97.
const char* FocusriteSaffirePro40Profile::Name() const noexcept {
    return "Focusrite Saffire Pro 40";
}

DiceDeviceQuirks FocusriteSaffirePro40Profile::Quirks() const noexcept {
    auto quirks = FocusriteSaffireProfile::Quirks();
    quirks.tx.hostToDevicePcmEncoding = Encoding::AudioWireFormat::kAM824;
    return quirks;
}

bool FocusriteSaffirePro40Profile::BuildDefaultTxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    return BuildTxStreamConfig(0, outConfig);
}

bool FocusriteSaffirePro40Profile::BuildTxStreamConfig(
    uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept {
    if (streamIndex >= TxStreamCount()) {
        return false;
    }
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    outConfig.pcmChannels = streamIndex == 0 ? 12 : 8;
    outConfig.midiSlots = streamIndex == 0 ? 1 : 0;
    outConfig.dbs = outConfig.pcmChannels + outConfig.midiSlots;
    outConfig.sourceChannelOffset = streamIndex == 0 ? 0 : 12;
    return true;
}

bool FocusriteSaffirePro40Profile::BuildDefaultRxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    return BuildRxStreamConfig(0, outConfig);
}

// Capture is 10 PCM + 1 MIDI, then 10 PCM with no MIDI (see the dump note
// above). Both streams are the same width, so only the MIDI slot -- and
// therefore the data-block size -- distinguishes them.
bool FocusriteSaffirePro40Profile::BuildRxStreamConfig(
    uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept {
    if (streamIndex >= RxStreamCount()) {
        return false;
    }
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    outConfig.pcmChannels = 10;
    outConfig.midiSlots = streamIndex == 0 ? 1 : 0;
    outConfig.dbs = outConfig.pcmChannels + outConfig.midiSlots;
    outConfig.sourceChannelOffset = streamIndex * 10;
    return true;
}

// Transmit (playback) safety offset is configured to be smaller (6 packets) to optimize
// latency while preventing audio dropouts under normal CPU/thread scheduling.
uint32_t FocusriteSaffireProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kTxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

// Receive (capture) safety offset is configured to be larger (16 packets) to handle
// FireWire packet reception jitter and asynchronous processing overhead.
uint32_t FocusriteSaffireProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kRxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

// The reported latencies in frames matching the focusrite saffire kext model.
uint32_t FocusriteSaffireProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

uint32_t FocusriteSaffireProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
