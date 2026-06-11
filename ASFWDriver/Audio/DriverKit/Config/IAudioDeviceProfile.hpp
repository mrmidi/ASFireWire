// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// IAudioDeviceProfile.hpp
// Protocol-agnostic device profile interface for ADK Dext configuration.

#pragma once

#include "../../Wire/AMDTP/PacketAssembler.hpp"
#include <cstdint>

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
};

} // namespace ASFW::Isoch::Audio
