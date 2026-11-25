// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidiWireCodec.hpp - the MIDI data-channel codec for a geometry's format.
// The receive packet processor and the transmit composer call these and never
// name a format themselves.

#pragma once

#include "MidiWireFormat.hpp"
#include "../AM824/MpxMidiDemux.hpp"
#include "../AM824/MpxMidiMux.hpp"
#include "../AM824/MpxMidiRateLimiter.hpp"
#include "../Digi00x/Digi00xMidi.hpp"

#include <cstdint>

namespace ASFW::Encoding {

/// Device -> host: deliver every MIDI byte in one packet's data blocks.
inline void DemuxMidiSlot(const uint8_t* dataBlocks, uint32_t eventCount, uint8_t dbc,
                          const MpxMidiGeometry& geometry, uint64_t packetHostTicks,
                          ASFW::Audio::Ports::IMidiByteSink& sink,
                          MpxMidiDemuxCounters& counters) noexcept {
    switch (geometry.format) {
        case MidiWireFormat::kAm824Mpx:
            DemuxMpxMidi(dataBlocks, eventCount, dbc, geometry, packetHostTicks, sink, counters);
            return;
        case MidiWireFormat::kDigi00x:
            Digi00x::DemuxMidi(dataBlocks, eventCount, dbc, geometry, packetHostTicks, sink,
                               counters);
            return;
    }
}

/// Host -> device: write the selected bytes into one packet's MIDI slots.
inline void WriteMidiSlot(uint8_t* payload, uint32_t blocksInPacket, uint8_t dbc,
                          const MpxMidiGeometry& geometry,
                          const MpxMidiPacketBytes& bytes) noexcept {
    switch (geometry.format) {
        case MidiWireFormat::kAm824Mpx:
            WriteMpxMidiSlot(payload, blocksInPacket, dbc, geometry, bytes);
            return;
        case MidiWireFormat::kDigi00x:
            Digi00x::WriteMidiSlot(payload, blocksInPacket, dbc, geometry, bytes);
            return;
    }
}

/// The ports (bit n = port n) a packet of `blocksInPacket` blocks can carry.
[[nodiscard]] constexpr uint32_t MidiPortsInPacket(uint32_t blocksInPacket, uint8_t dbc,
                                                   const MpxMidiGeometry& geometry) noexcept {
    switch (geometry.format) {
        case MidiWireFormat::kAm824Mpx:
            return MpxMidiPortsInPacket(blocksInPacket, dbc, geometry);
        case MidiWireFormat::kDigi00x:
            return Digi00x::PortsInPacket(blocksInPacket, dbc, geometry);
    }
    return 0;
}

/// How fast the device may be fed.
[[nodiscard]] constexpr MidiTxPacing MidiTxPacingFor(MidiWireFormat format) noexcept {
    switch (format) {
        case MidiWireFormat::kAm824Mpx: return kAm824MidiPacing;
        case MidiWireFormat::kDigi00x: return Digi00x::kMidiTxPacing;
    }
    return kAm824MidiPacing;
}

} // namespace ASFW::Encoding
