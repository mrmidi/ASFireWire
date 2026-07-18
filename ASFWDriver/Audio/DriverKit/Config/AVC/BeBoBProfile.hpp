// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBProfile.hpp — Per-GUID ADK stream geometry for generic BeBoB devices.
//
// Unlike Phase88Profile (static, known geometry), BeBoBProfile is constructed
// from discovery data and owned per-GUID. This allows two different BeBoB
// devices to report different stream geometries. For verified devices with
// static geometry (Phase88), use the dedicated profile instead.

#pragma once

#include "../AudioStreamProfile.hpp"
#include "../../../../Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"

#include <vector>

namespace ASFW::Isoch::Audio::AVC::Profiles {

class BeBoBProfile final : public IAudioStreamProfile {
public:
    explicit BeBoBProfile(const ::ASFW::Audio::BeBoB::DeviceModel& discoveryModel);

    [[nodiscard]] const char* Name() const noexcept override { return "BeBoB Device"; }

    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;

    // BeBoB devices transmit CIP NO-DATA until receiving host packets.
    // Match Linux READY_TIMEOUT_MS (bebob_stream.c:10).
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override {
        return 4000;
    }

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;

private:
    uint32_t pcmChannels_{0};
    uint32_t midiSlots_{0};
    uint32_t sampleRateHz_{48000};
    std::vector<uint32_t> supportedRates_;
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
