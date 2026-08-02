// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProfile.hpp - ADK isoch geometry for Apogee Duet AV/C.

#pragma once

#include "../DICE/DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

// The ADK direct-audio allocation path consumes the shared isoch profile
// interface even for AV/C devices. The protocol/control path remains AV/C.
class ApogeeDuetProfile final : public DICE::IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;

    [[nodiscard]] bool Matches(const DICE::DiceDeviceIdentity& identity) const noexcept override;

    [[nodiscard]] DICE::DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        DICE::DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        DICE::DiceStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
