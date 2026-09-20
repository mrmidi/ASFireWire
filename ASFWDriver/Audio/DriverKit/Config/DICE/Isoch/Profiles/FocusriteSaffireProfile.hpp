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
    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;
    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildTxStreamConfig(
        uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept override;
    // Capture's counterpart. Without it the base repeated stream 0's shape,
    // which put a MIDI slot on stream 1 that the device does not carry: the
    // recorded dump is 10 PCM + MIDI, then 10 PCM alone. The PCM total came out
    // right by luck (both streams are 10 wide), the data-block size did not.
    [[nodiscard]] bool BuildRxStreamConfig(
        uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] uint32_t TxStreamCount() const noexcept override { return 2; }
    [[nodiscard]] uint32_t RxStreamCount() const noexcept override { return 2; }
    // TxChannelCount() is deliberately NOT overridden: the base sums
    // BuildTxStreamConfig across the streams, which is 12 + 8 = 20 -- the value
    // this override stated by hand. A profile that states a total separately
    // from its streams can disagree with itself; this one no longer can.
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
