// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieOnyx400FProfile.hpp - ADK isoch geometry for the Mackie Onyx 400F
// (Echo Fireworks platform; Linux snd-fireworks VENDOR_LOUD/MODEL_MACKIE_400F).
//
// Static 10-in/10-out (+1 MIDI slot per direction, DBS 11) at 44.1 kHz from the
// product spec — NOT yet captured
// from hardware. The runtime protocol (Audio/Protocols/Fireworks) reads the
// device's HWINFO on first contact and refuses to stream if the counts differ;
// update this profile, the nub publisher in AVCDiscovery and kOnyx400FGeometry
// together from that capture.

#pragma once

#include "../DICE/DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

class MackieOnyx400FProfile final : public DICE::IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;

    [[nodiscard]] bool Matches(const DICE::DiceDeviceIdentity& identity) const noexcept override;

    [[nodiscard]] DICE::DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        DICE::DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        DICE::DiceStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
