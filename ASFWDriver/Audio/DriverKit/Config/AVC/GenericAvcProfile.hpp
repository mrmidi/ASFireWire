// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GenericAvcProfile.hpp - The one audio profile for AV/C units published from
// generic discovery (BeBoB and Oxford alike).
//
// Stream geometry, rates and channel names come from discovery
// (AvcAudioConfig::BuildGraphAudioConfig) and cross the nub as resolved
// streams, which StartIO frames from (BuildResolvedTxStreamConfig). Rate
// policy is the catalog row's (ApplyRatePolicy), and the device name is the
// nub's. So this profile holds only the host side: AM824 framing constants,
// the transmit policy and the host timing. It names no device and states no
// channel counts or rates.

#pragma once

#include "../AudioStreamProfile.hpp"

#include <cstdint>
#include <vector>

namespace ASFW::Isoch::Audio::AVC::Profiles {

class GenericAvcProfile final : public IAudioStreamProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override { return "Generic AV/C"; }

    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kAM824;
    }
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kAM824;
    }

    // Framing constants only: stream shape comes from discovery.
    [[nodiscard]] bool BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept override;

    // Rates come from discovery and the catalog row, never from here.
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override { return {}; }

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;

    // BeBoB devices send NO-DATA until they have received host packets for
    // about a second (wire-verified on the PHASE 88, 2026-07-16; Linux
    // bebob_stream.c:10 waits 4 s). Oxford answers sooner (oxfw-stream.c:12
    // waits 600 ms); the longer budget only delays a real failure.
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override { return 4000; }

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
