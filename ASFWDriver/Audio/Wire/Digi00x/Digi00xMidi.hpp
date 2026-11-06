// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xMidi.hpp - the Digi 002 / 003 MIDI data channel (issue #249).
//
// Data channel 0 of every data block carries MIDI, in a format of the device's
// own, not IEC 61883-6 MPX-MIDI:
//
//   byte 0  0x80 in every block, with or without MIDI
//   byte 1  first MIDI byte   (0 when unused)
//   byte 2  second MIDI byte  (0 when unused)
//   byte 3  port code in the high nibble, MIDI byte count (0..2) in the low
//
// Port codes: 0x0 = MIDI port 1, 0x2 = MIDI port 2, 0xE = the console's
// control port. Host to device, a block may carry bytes only for the port its
// data block counter selects, (DBC + block) % 8: 0, 1 and 2 map to the three
// ports and 3..7 stay idle. Device to host, the port comes from the code in
// byte 3, not from the counter.
//
// Behavioural reference: Linux sound/firewire/digi00x/amdtp-dot.c
// (write_midi_messages, read_midi_messages). Fresh implementation; nothing is
// copied. Two deliberate differences: receive accepts only the codes the
// reference documents (0x0 and 0xE) and counts anything else as invalid,
// where the reference sends every nonzero code to the console port, so an
// unexpected code shows up in the counters instead of on a port; and transmit
// writes one byte per quadlet, as the shared composer reserves one byte per
// port per packet.
//
// Same content/transport split and inputs as MpxMidiDemux / MpxMidiMux, so the
// receive packet processor and the transmit composer can select either by
// wire format. Pure logic: no DriverKit dependency, no allocation, no locking.

#pragma once

#include "../AM824/MpxMidiMux.hpp"   // MpxMidiGeometry, MpxMidiPacketBytes, counters

#include <cstddef>
#include <cstdint>

namespace ASFW::Encoding::Digi00x {

/// Host-side port indices, the same in both directions.
inline constexpr uint8_t kMidiPort1 = 0;
inline constexpr uint8_t kMidiPort2 = 1;     ///< host to device only
inline constexpr uint8_t kControlPort = 2;   ///< console models only
inline constexpr uint8_t kMidiPortSlots = 3;

/// Byte 0 of the MIDI data channel in every block.
inline constexpr uint8_t kMidiLabel = 0x80;
inline constexpr uint8_t kMaxMidiBytesPerQuadlet = 2;

/// The port code in the high nibble of byte 3, or 0xFF for no port.
[[nodiscard]] constexpr uint8_t PortCode(uint8_t port) noexcept {
    switch (port) {
        case kMidiPort1: return 0x0;
        case kMidiPort2: return 0x2;
        case kControlPort: return 0xE;
        default: return 0xFF;
    }
}

/// The host port a received port code belongs to, or 0xFF if the code is not
/// one the device is documented to send.
[[nodiscard]] constexpr uint8_t PortForReceivedCode(uint8_t code) noexcept {
    switch (code) {
        case 0x0: return kMidiPort1;
        case 0xE: return kControlPort;
        default: return 0xFF;
    }
}

/// The port block `f` of a packet with this DBC may carry, or 0xFF for none.
[[nodiscard]] constexpr uint8_t PortForBlock(uint8_t dbc, uint32_t f) noexcept {
    const uint32_t slot = (static_cast<uint32_t>(dbc) + f) % 8u;
    return slot < kMidiPortSlots ? static_cast<uint8_t>(slot) : 0xFF;
}

/// Lift MIDI bytes out of one received packet's data blocks.
///
/// Inputs as DemuxMpxMidi: `dataBlocks` is the first data block, past the CIP
/// header, and holds `eventCount * dbs * 4` bytes; the caller establishes every
/// bound. `dbc` only feeds the trace counters: the port is in the quadlet.
/// `geometry.portCount` bounds the host ports: bytes for a port at
/// or above it are dropped and counted.
inline void DemuxMidi(const uint8_t* dataBlocks, uint32_t eventCount,
                      uint8_t dbc, const MpxMidiGeometry& geometry,
                      uint64_t packetHostTicks,
                      ASFW::Audio::Ports::IMidiByteSink& sink,
                      MpxMidiDemuxCounters& counters) noexcept {
    if (dataBlocks == nullptr || eventCount == 0 || !geometry.Valid()) return;

    for (uint32_t f = 0; f < eventCount; ++f) {
        const uint8_t* quadlet =
            dataBlocks + (static_cast<size_t>(f) * geometry.dbs +
                          geometry.midiSlotIndex) * 4u;
        ++counters.blocksInspected;

        const uint8_t count = quadlet[3] & 0x0Fu;
        if (quadlet[0] != kMidiLabel || count > kMaxMidiBytesPerQuadlet) {
            ++counters.invalidLabels;
            continue;
        }
        if (count == 0) {
            ++counters.emptyQuadlets;
            continue;
        }
        const uint8_t port = PortForReceivedCode(static_cast<uint8_t>(quadlet[3] >> 4));
        if (port == 0xFF) {
            ++counters.invalidLabels;
            continue;
        }
        if (port >= geometry.portCount) {
            counters.droppedForUnknownPort += count;
            continue;
        }
        counters.lastDeliveredDbc = dbc;
        counters.lastDeliveredBlockIndex = f;
        counters.lastDeliveredPort = port;
        sink.DeliverMidiBytes(port, quadlet + 1, count, packetHostTicks);
        counters.bytesDelivered += count;
    }
}

/// Write the MIDI data channel across one outgoing packet's data blocks.
///
/// Inputs and ordering as WriteMpxMidiSlot: run after the PCM is in place.
/// The port rotation always counts from the DBC (`geometry.dbcAligned` is not
/// consulted), as the reference does.
/// Every block's MIDI quadlet is written: the selected port's byte where there
/// is one, the idle word (0x80000000) otherwise. Unlike MPX there is no
/// eight-block cap; the reference has none, and 1x rates never exceed eight
/// blocks per packet.
inline void WriteMidiSlot(uint8_t* payload, uint32_t blocksInPacket, uint8_t dbc,
                          const MpxMidiGeometry& geometry,
                          const MpxMidiPacketBytes& bytes) noexcept {
    if (payload == nullptr || !geometry.Valid()) return;

    for (uint32_t f = 0; f < blocksInPacket; ++f) {
        uint8_t* slot =
            payload + (static_cast<size_t>(f) * geometry.dbs +
                       geometry.midiSlotIndex) * 4u;
        const uint8_t port = PortForBlock(dbc, f);
        const bool send = port < geometry.portCount && bytes.hasByteForPort[port];
        slot[0] = kMidiLabel;
        slot[1] = send ? bytes.byteForPort[port] : 0;
        slot[2] = 0;
        slot[3] = send ? static_cast<uint8_t>((PortCode(port) << 4) | 1u) : 0;
    }
}

/// The ports (bit n = port n) WriteMidiSlot can carry in a packet of
/// `blocksInPacket` blocks with this DBC.
[[nodiscard]] constexpr uint32_t PortsInPacket(uint32_t blocksInPacket, uint8_t dbc,
                                               const MpxMidiGeometry& geometry) noexcept {
    uint32_t mask = 0;
    for (uint32_t f = 0; f < blocksInPacket && f < 8u; ++f) {
        const uint8_t port = PortForBlock(dbc, f);
        if (port < geometry.portCount) mask |= 1u << port;
    }
    return mask;
}

} // namespace ASFW::Encoding::Digi00x
