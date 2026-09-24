// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// WeissIntProfile.cpp - Weiss INT202/INT203 DICE stream profile.

#include "WeissIntProfile.hpp"
#include "../../../TimingLadder.hpp"

#include "../../../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {
namespace {

constexpr uint32_t kPcmChannels = 2;

void FillStreamConfig(DiceStreamConfig& out, DiceStreamDirection direction) noexcept {
    out = DiceStreamConfig{};
    out.direction = direction;
    out.sampleRate = 48000;
    out.streamMode = Encoding::StreamMode::kBlocking;
    out.sid = 0;
    out.pcmChannels = kPcmChannels;
    out.dbs = kPcmChannels;
    out.framesPerDataPacket = 8;
    out.fdf = 0x02;
    out.fmt = 0x10;
}

} // namespace

const char* WeissIntProfile::Name() const noexcept {
    return "Weiss INT (DICE)";
}

DiceDeviceQuirks WeissIntProfile::Quirks() const noexcept {
    // The Linux Weiss DICE driver uses the normal DICE AMDTP PCM path. Keep
    // ASFW's standards-conformant AM824 default; no Weiss-specific framing
    // quirk has been established from hardware or the original driver.
    return DiceDeviceQuirks{};
}

bool WeissIntProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool WeissIntProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    // Linux dice-weiss.c:10-35 declares two PCM channels in *both* DICE
    // directions for INT202 and INT203. This is the physical/control-plane
    // stream shape, independent of the CoreAudio input visibility above.
    FillStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

uint32_t WeissIntProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kTxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kNone);
}

uint32_t WeissIntProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kRxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kNone);
}

uint32_t WeissIntProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

uint32_t WeissIntProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
