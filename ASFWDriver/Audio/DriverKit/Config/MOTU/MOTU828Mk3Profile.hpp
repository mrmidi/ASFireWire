// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Protocol-neutral ADK stream profile for the MOTU 828 Mk3 V3 wire format.

#pragma once

#include "../AudioStreamProfile.hpp"

namespace ASFW::Isoch::Audio::MOTU::Profiles {

class MOTU828Mk3Profile final : public IAudioStreamProfile {
  public:
    [[nodiscard]] const char* Name() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override;

    [[nodiscard]] bool
    BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool
    BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;
    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;

    // Observed during bring-up: after the ISOC_COMM_CONTROL
    // deactivate->activate transition, MOTU's PLL takes up to ~3 s to lock before
    // it starts sending IR packets. The 500 ms base-class default always tears
    // the stream down before the device comes up.
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override {
        return 3000;
    }
};

} // namespace ASFW::Isoch::Audio::MOTU::Profiles
