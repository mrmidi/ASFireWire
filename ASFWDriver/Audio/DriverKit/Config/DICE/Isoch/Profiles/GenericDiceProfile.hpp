// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GenericDiceProfile.hpp
// Generic fallback profile for DICE devices.

#pragma once

#include "../../DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

class GenericDiceProfile final : public IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;

    [[nodiscard]] bool Matches(const DiceDeviceIdentity& identity) const noexcept override;

    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] uint32_t SafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t ReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
