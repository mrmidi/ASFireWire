// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV2Profile.hpp - ADK isoch geometry for MOTU protocol-v2 devices.
//
// This is the driver-side (allocator) profile, distinct from the DeviceProfiles identity
// table. ASFWAudioDevice::StartIO resolves one of these and calls
// BuildDefaultTxStreamConfig to size the isochronous resources; without a MOTU entry the
// registry falls through to the generic DICE profile, which describes an AM824 stream and
// makes StartIO fail with kAudioHardwareUnspecifiedError ('what').
//
// Geometry is the v2 fixed-chunk layout: 14 PCM chunks per direction at 44.1/48 kHz
// (motu-protocol-v2.c:274-282, snd_motu_spec_828mk2 / snd_motu_spec_ultralite). dbs is
// the data block size in QUADLETS -- for MOTU that is one SPH quadlet plus the message
// and PCM chunks padded to quadlet alignment, which is 13 for 14 chunks, NOT the channel
// count. Getting that wrong sizes every packet incorrectly.

#pragma once

#include "../AudioStreamProfile.hpp"

namespace ASFW::Isoch::Audio::MOTU::Profiles {

class MotuV2Profile final : public IAudioStreamProfile {
public:
    /// `unitSwVersion` selects the model name. The geometry below is the
    /// {14,14,0} layout shared by the 828mk2 and UltraLite — it is NOT shared by
    /// every v2 model, so this profile must not be handed a version it was not
    /// written for. The 896HD and Traveler are {14,14,8}, which coincides only
    /// below 176.4 kHz, and the 8pre is tx {10,10,0} / rx {6,6,0} — asymmetric
    /// between directions, which a single chunk count cannot express
    /// (Linux motu-protocol-v2.c:274-320). Enabling another model means giving it
    /// its own geometry, not adding a case to the name switch.
    explicit MotuV2Profile(uint32_t unitSwVersion = 0) noexcept
        : unitSwVersion_(unitSwVersion) {}

    [[nodiscard]] const char* Name() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override;
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        AudioStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        AudioStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override;

    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;

    [[nodiscard]] uint32_t RxChannelCount() const noexcept override;
    [[nodiscard]] uint32_t TxChannelCount() const noexcept override;
    [[nodiscard]] uint32_t TxMidiSlots() const noexcept override;
    [[nodiscard]] uint32_t RxMidiSlots() const noexcept override;
    [[nodiscard]] uint32_t TxDbs() const noexcept override;
    [[nodiscard]] uint32_t RxDbs() const noexcept override;
    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;

    // MOTU is duplex-always: the device only produces a usable clock once the host is
    // replaying its cadence, so the anchor can take longer to establish than a DICE
    // device's. Matches the Linux READY_TIMEOUT_MS budget (motu-stream.c:11).
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override {
        return 2000;
    }

private:
    uint32_t unitSwVersion_{0};
};

} // namespace ASFW::Isoch::Audio::MOTU::Profiles
