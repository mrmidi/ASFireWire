// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// GenericDiceProfile.cpp
// Generic fallback profile for DICE devices.

#include "GenericDiceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {

void FillDefaultStreamConfig(DiceStreamConfig& outConfig,
                             DiceStreamDirection direction) noexcept {
    outConfig = DiceStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = 48000;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = 0;
    outConfig.pcmChannels = 2;
    outConfig.dbs = 2;
    outConfig.midiSlots = 0;
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
}

} // namespace

const char* GenericDiceProfile::Name() const noexcept {
    return "Generic DICE";
}

bool GenericDiceProfile::Matches(const DiceDeviceIdentity& identity) const noexcept {
    (void)identity;
    return true; // Catch-all fallback
}

DiceDeviceQuirks GenericDiceProfile::Quirks() const noexcept {
    return DiceDeviceQuirks{};
}

bool GenericDiceProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool GenericDiceProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

uint32_t GenericDiceProfile::SafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    // Standard safe defaults
    return 64; 
}

uint32_t GenericDiceProfile::ReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
