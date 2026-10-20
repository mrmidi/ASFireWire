// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProfile.cpp - see ApogeeDuetProfile.hpp for the sources.

#include "ApogeeDuetProfile.hpp"
#include "../TimingLadder.hpp"

#include <optional>

namespace ASFW::Isoch::Audio::AVC::Profiles {

namespace {

struct DuetLatency {
    uint32_t rate;
    uint32_t input;
    uint32_t output;
};

// DuetFWOverideDriver.kext Info.plist, Latency table (InputLatency/OutputLatency).
constexpr DuetLatency kDuetLatency[] = {
    {44100, 46, 55},
    {48000, 40, 67},
    {88200, 108, 57},
    {96000, 119, 59},
};

std::optional<DuetLatency> LatencyAt(double sampleRate) noexcept {
    for (const auto& row : kDuetLatency) {
        if (static_cast<double>(row.rate) == sampleRate) return row;
    }
    return std::nullopt;
}

// The DICE ladder on the same TX/RX engine: Saffire's 6 output delay packets,
// and the 10 input packets the Pro 24 DSP measured fit (S_in 80 at 48 kHz).
constexpr uint32_t kTxDelayPackets = TimingLadder::kTxDelayPackets;
constexpr uint32_t kRxDelayPackets = 10;
static_assert(TimingLadder::SafetyOffsetFrames(kTxDelayPackets, 48000.0,
                                               TimingLadder::RateAddend::kPerTier) == 48);
static_assert(TimingLadder::SafetyOffsetFrames(kRxDelayPackets, 48000.0,
                                               TimingLadder::RateAddend::kPerTier) == 80);

} // namespace

uint32_t ApogeeDuetProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(kTxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

uint32_t ApogeeDuetProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(kRxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

// A rate outside Apogee's table keeps the generic AV/C declaration.
uint32_t ApogeeDuetProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    const auto row = LatencyAt(sampleRate);
    return row ? row->output : GenericAvcProfile::TxReportedLatencyFrames(sampleRate);
}

uint32_t ApogeeDuetProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    const auto row = LatencyAt(sampleRate);
    return row ? row->input : GenericAvcProfile::RxReportedLatencyFrames(sampleRate);
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
