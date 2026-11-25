// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidiWireFormat.hpp - how a family lays MIDI bytes into its MIDI data
// channel. The value travels with the MIDI geometry from the family adapter's
// MidiEndpointCapabilities to the receive packet processor and the transmit
// composer, which select the codec with it (MidiWireCodec.hpp).
//
// Adding a format: a value here, its codec, and one case in each function of
// MidiWireCodec.hpp.

#pragma once

#include <cstdint>

namespace ASFW::Encoding {

enum class MidiWireFormat : uint8_t {
    /// IEC 61883-6 MPX-MIDI: AM824 labels 0x80..0x83, port by (DBC + block) % 8.
    kAm824Mpx = 0,
    /// Digi 002 / 003: byte 3 carries the port code and byte count
    /// (Audio/Wire/Digi00x/Digi00xMidi.hpp).
    kDigi00x = 1,
};

inline constexpr MidiWireFormat kLastMidiWireFormat = MidiWireFormat::kDigi00x;

/// For a value that crossed a boundary as an integer.
[[nodiscard]] constexpr bool IsMidiWireFormat(uint32_t raw) noexcept {
    return raw <= static_cast<uint32_t>(kLastMidiWireFormat);
}

} // namespace ASFW::Encoding
