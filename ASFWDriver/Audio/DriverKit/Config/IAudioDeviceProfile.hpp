// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IAudioDeviceProfile.hpp
// Protocol-agnostic device profile interface for ADK Dext configuration.

#pragma once

#include "../../Wire/AMDTP/AmdtpTypes.hpp"
#include <cstdint>
#include <vector>

namespace ASFW::Isoch::Audio {

/// Protocol-agnostic device profile interface for ADK Dext configuration.
/// Since the driver is compiled with -fno-rtti, all polymorphism is statically
/// declared as virtual interface methods.
class IAudioDeviceProfile {
public:
    virtual ~IAudioDeviceProfile() = default;

    /// Returns the human-readable product name for the device.
    [[nodiscard]] virtual const char* Name() const noexcept = 0;

    /// Returns the host-to-device (transmit) wire format encoding (e.g. kAM824 vs kRawPcm24In32).
    [[nodiscard]] virtual Encoding::AudioWireFormat TxWireFormat() const noexcept = 0;

    /// Returns the device-to-host (receive) wire format encoding.
    [[nodiscard]] virtual Encoding::AudioWireFormat RxWireFormat() const noexcept = 0;

    /// Returns the number of PCM audio output channels from the host.
    [[nodiscard]] virtual uint32_t TxChannelCount() const noexcept = 0;

    /// Returns the number of PCM audio input channels to the host.
    [[nodiscard]] virtual uint32_t RxChannelCount() const noexcept = 0;

    /// Returns the number of transmit MIDI slots embedded in the isochronous streams.
    [[nodiscard]] virtual uint32_t TxMidiSlots() const noexcept = 0;

    /// Returns the number of receive MIDI slots embedded in the isochronous streams.
    [[nodiscard]] virtual uint32_t RxMidiSlots() const noexcept = 0;

    /// Returns the transmit data block size (DBS) in quadlets.
    [[nodiscard]] virtual uint32_t TxDbs() const noexcept = 0;

    /// Returns the receive data block size (DBS) in quadlets.
    [[nodiscard]] virtual uint32_t RxDbs() const noexcept = 0;

    /// Returns the transmit safety offset in frames for a given sample rate.
    /// This defines the minimum distance between the host's read/write pointers and the hardware DMA head.
    [[nodiscard]] virtual uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept = 0;

    /// Returns the receive safety offset in frames for a given sample rate.
    [[nodiscard]] virtual uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept = 0;

    /// Returns the reported transmit latency in frames for a given sample rate.
    /// This is published to the HAL/CoreAudio to allow proper alignment of streams.
    [[nodiscard]] virtual uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept = 0;

    /// Returns the reported receive latency in frames for a given sample rate.
    [[nodiscard]] virtual uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept = 0;

    /// Sample rates advertised to CoreAudio. Default is a single 48 kHz; profiles
    /// override to expose the device's supported set (DICE decodes CLOCKCAPABILITIES).
    [[nodiscard]] virtual std::vector<uint32_t> SupportedSampleRates() const {
        return {48000u};
    }

    /// IEC 61883-6 presentation delay removed from received SYT before replay.
    [[nodiscard]] virtual uint32_t RxTransferDelayTicks(double sampleRate) const noexcept {
        (void)sampleRate;
        return 12800;
    }

    /// IEC 61883-6 presentation delay added when rebuilding transmit SYT.
    [[nodiscard]] virtual uint32_t TxTransferDelayTicks(double sampleRate) const noexcept {
        (void)sampleRate;
        return 12800;
    }
};

} // namespace ASFW::Isoch::Audio
