// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FocusriteSaffireProfile.hpp
// Focusrite Saffire specific profile.

#pragma once

#include "../../DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

class FocusriteSaffireProfile final : public IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;

    [[nodiscard]] bool Matches(const DiceDeviceIdentity& identity) const noexcept override;

    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;

    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
