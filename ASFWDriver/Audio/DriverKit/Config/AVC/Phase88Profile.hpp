// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Phase88Profile.hpp - ADK isoch geometry for TerraTec PHASE 88 Rack FW BeBoB.

#pragma once

#include "../AudioStreamProfile.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

// This is ADK stream geometry only. PHASE 88 identity and control remain
// AV/C + CMP; no DICE identity contract applies to this profile.
class Phase88Profile final : public IAudioStreamProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;

    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        AudioStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;

    // Wire-verified 2026-07-16: with only an oPCR connection the PHASE 88
    // transmits 8-byte CIP NO-DATA indefinitely (7.5 s observed); it starts
    // multiplexing data only after receiving host packets for about a second.
    // A 500 ms budget therefore always tears the stream down just before the
    // device comes up. Match Linux READY_TIMEOUT_MS (bebob_stream.c:10).
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override {
        return 4000;
    }

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
