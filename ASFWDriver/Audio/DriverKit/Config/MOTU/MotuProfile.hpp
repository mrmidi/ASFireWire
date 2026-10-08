// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuProfile.hpp - model-specific ADK geometry for MOTU packed PCM.
//
// This is the driver-side (allocator) profile, distinct from the DeviceProfiles identity
// table. ASFWAudioDevice::StartIO resolves one of these and calls
// BuildDefaultTxStreamConfig to size the isochronous resources; without a MOTU entry the
// registry falls through to the generic DICE profile, which describes an AM824 stream and
// makes StartIO fail with kAudioHardwareUnspecifiedError ('what').
//
// Defaults come from the model table; live rate formations include optical banks.
// For example, 828mk2 has 14 fixed PCM chunks at 44.1/48 kHz
// (motu-protocol-v2.c:274-282, snd_motu_spec_828mk2). dbs is
// the data block size in QUADLETS -- for MOTU that is one SPH quadlet plus the message
// and PCM chunks padded to quadlet alignment, which is 13 for 14 chunks, NOT the channel
// count. Getting that wrong sizes every packet incorrectly.

#pragma once

#include "../AudioStreamProfile.hpp"
#include "../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../Wire/MOTU/MotuPortLayout.hpp"

namespace ASFW::Isoch::Audio::MOTU::Profiles {

/// Host-to-device port map for a model, by its unit directory software version.
[[nodiscard]] constexpr ::ASFW::Encoding::Motu::MotuPortMap PlaybackPortsForSwVersion(uint32_t swVersion) noexcept {
    switch (swVersion) {
    case DeviceProfiles::Audio::kMotu828mk2SwVersion:
    case DeviceProfiles::Audio::kMotuUltraliteSwVersion:
        return ::ASFW::Encoding::Motu::kV2Playback;
    case 0x15: return ::ASFW::Encoding::Motu::k828mk3Playback;
    default:
        return {};
    }
}

/// Device-to-host port map for a model, by its unit directory software version.
[[nodiscard]] constexpr ::ASFW::Encoding::Motu::MotuPortMap CapturePortsForSwVersion(uint32_t swVersion) noexcept {
    switch (swVersion) {
    case DeviceProfiles::Audio::kMotu828mk2SwVersion:
        return ::ASFW::Encoding::Motu::k828mk2Capture;
    case DeviceProfiles::Audio::kMotuUltraliteSwVersion:
        return ::ASFW::Encoding::Motu::kUltraLiteCapture;
    default:
        return {};
    }
}

class MotuProfile final : public IAudioStreamProfile {
public:
    /// Model-specific fallback geometry. Live rate formations supersede these
    /// defaults before the graph starts; a model is never silently 828mk2.
    explicit MotuProfile(uint32_t unitSwVersion = 3) noexcept
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

    // Tx/RxChannelCount() are deliberately NOT overridden: one stream per
    // direction carrying kPcmChunks channels is exactly what the base computes
    // by summing BuildTx/RxStreamConfig, so the overrides restated their own
    // stream config and could only ever drift from it.
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
