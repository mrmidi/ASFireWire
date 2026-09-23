// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../AudioStreamProfile.hpp"
#include "../../../../Audio/Protocols/BeBoB/MAudioSpecialStartPolicy.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

// HAL allocation for the two M-Audio special-firmware personas. The initial
// S/PDIF formation is asymmetric: ten capture PCM slots, six playback PCM
// slots, and one AM824 MIDI slot in each direction. Device commands and dynamic
// formation changes belong to MAudioSpecialProtocol, not this profile.
class MAudioSpecialProfile final : public IAudioStreamProfile {
public:
    explicit MAudioSpecialProfile(bool projectMix) noexcept : projectMix_(projectMix) {}

    [[nodiscard]] const char* Name() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override;
    [[nodiscard]] bool BuildDefaultTxStreamConfig(AudioStreamConfig& out) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(AudioStreamConfig& out) const noexcept override;
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;
    /// Apply one of the profile's fixed-geometry S/PDIF rates to a live stream
    /// config, including the maximum AMDTP data packet frame count.
    [[nodiscard]] static bool ConfigureStreamRate(
        AudioStreamConfig& config, uint32_t sampleRateHz) noexcept;
    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double rate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double rate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double rate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double rate) const noexcept override;
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override {
        return ::ASFW::Audio::BeBoB::kMAudioStreamReadyTimeoutMs;
    }
    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;

private:
    bool projectMix_{false};
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
