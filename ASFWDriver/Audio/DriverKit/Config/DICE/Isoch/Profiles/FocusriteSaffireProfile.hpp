// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FocusriteSaffireProfile.hpp
// Focusrite Saffire specific profile.

#pragma once

#include "../../DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

class FocusriteSaffireProfile : public IDiceDeviceProfile {
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

class FocusriteSaffirePro40Profile final : public FocusriteSaffireProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;
    [[nodiscard]] bool Matches(const DiceDeviceIdentity& identity) const noexcept override;
    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;
    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildTxStreamConfig(
        uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] uint32_t TxStreamCount() const noexcept override { return 2; }
    [[nodiscard]] uint32_t RxStreamCount() const noexcept override { return 2; }
    [[nodiscard]] uint32_t TxChannelCount() const noexcept override { return 20; }
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
