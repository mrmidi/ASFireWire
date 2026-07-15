// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProfile.cpp - ADK isoch geometry for Apogee Duet AV/C.

#include "ApogeeDuetProfile.hpp"

#include "../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

namespace {

void FillStreamConfig(DICE::DiceStreamConfig& outConfig,
                      DICE::DiceStreamDirection direction) noexcept {
    outConfig = DICE::DiceStreamConfig{};
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

const char* ApogeeDuetProfile::Name() const noexcept {
    return DeviceProfiles::Audio::kApogeeDuetModelName;
}

bool ApogeeDuetProfile::Matches(const DICE::DiceDeviceIdentity& identity) const noexcept {
    return identity.vendorId == DeviceProfiles::Audio::kApogeeVendorId &&
           identity.modelId == DeviceProfiles::Audio::kApogeeDuetModelId;
}

DICE::DiceDeviceQuirks ApogeeDuetProfile::Quirks() const noexcept {
    return DICE::DiceDeviceQuirks{};
}

bool ApogeeDuetProfile::BuildDefaultTxStreamConfig(
    DICE::DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DICE::DiceStreamDirection::HostToDevice);
    return true;
}

bool ApogeeDuetProfile::BuildDefaultRxStreamConfig(
    DICE::DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DICE::DiceStreamDirection::DeviceToHost);
    return true;
}

std::vector<uint32_t> ApogeeDuetProfile::SupportedSampleRates() const {
    return {44100u, 48000u};
}

uint32_t ApogeeDuetProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 64;
}

uint32_t ApogeeDuetProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

uint32_t ApogeeDuetProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

uint32_t ApogeeDuetProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
