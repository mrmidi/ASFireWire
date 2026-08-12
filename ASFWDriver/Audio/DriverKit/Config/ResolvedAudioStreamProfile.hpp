// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AudioStreamProfile.hpp"
#include "../../Devices/ResolvedAudioEndpointProfile.hpp"

namespace ASFW::Audio::DriverKit {

// AudioDriverKit-local value adapter over the serialized resolved endpoint
// profile. It performs no catalog or identity lookup and owns its copy.
class ResolvedAudioStreamProfile final
    : public ASFW::Isoch::Audio::IAudioStreamProfile {
public:
    void Load(Devices::ResolvedAudioEndpointProfile profile) noexcept;
    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] const Devices::ResolvedAudioEndpointProfile& Value() const noexcept;
    [[nodiscard]] Devices::ResolvedAudioEndpointProfile& MutableValue() noexcept;

    [[nodiscard]] const char* Name() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override;
    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        ASFW::Isoch::Audio::AudioStreamConfig& out) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        ASFW::Isoch::Audio::AudioStreamConfig& out) const noexcept override;
    [[nodiscard]] uint32_t TxStreamCount() const noexcept override;
    [[nodiscard]] uint32_t RxStreamCount() const noexcept override;
    [[nodiscard]] ASFW::Isoch::Audio::AudioStreamTxPolicy
    TxStreamPolicy() const noexcept override;
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override;
    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double rate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double rate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double rate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double rate) const noexcept override;
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;
    [[nodiscard]] uint32_t RxTransferDelayTicks(double rate) const noexcept override;
    [[nodiscard]] uint32_t TxTransferDelayTicks(double rate) const noexcept override;

private:
    [[nodiscard]] const Devices::RateTimingPolicy* Timing(double rate) const noexcept;
    [[nodiscard]] bool BuildStream(bool playback,
        ASFW::Isoch::Audio::AudioStreamConfig& out) const noexcept;

    Devices::ResolvedAudioEndpointProfile profile_{};
};

} // namespace ASFW::Audio::DriverKit
