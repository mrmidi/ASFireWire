//
// MpxMidiMux.hpp
// ASFWDriver
//
// MPX-MIDI multiplexing: write one AM824 slot per data block, eight ports
// rotating across successive blocks.
//
// The inverse of MpxMidiDemux, and the same content/transport split: it fills a
// slot in a packet image transport will carry opaquely.
//
// Cross-checked against Linux write_midi_messages (amdtp-am824.c:295-320) and
// Focusrite's FillFirewireBuffers MIDI loop (Saffire!0xf449). Fresh code; the
// references are behavioural evidence only.
//
// Pure logic: no DriverKit dependency, no allocation, no locking.
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "MpxMidiDemux.hpp"   // kMpxMidiPorts, kMpxMidiLabelBase

namespace ASFW::Encoding {

/// Data blocks per packet that may carry MIDI.
///
/// Linux caps MIDI to the first eight blocks (MAX_MIDI_RX_BLOCKS,
/// amdtp-am824.c:28,306) and says some receivers inspect only those.
/// Focusrite writes every block, which proves its own devices accept more --
/// not that others do. At 48 kHz the two are identical; above it they diverge,
/// and the cap costs nothing because MIDI 1.0 cannot use the extra bandwidth.
inline constexpr uint32_t kMpxMidiMaxBlocksPerPacket = 8;

/// Empty MIDI quadlet: "MIDI conformant data, zero valid bytes".
/// Byte-identical to what PrepareDataPacket already lays into non-PCM slots.
inline constexpr uint32_t kMpxMidiEmptyQuadlet = 0x80000000u;

/// What to write into one packet's MIDI slot: at most one byte per port, as
/// both references emit (label 0x81).
struct MpxMidiPacketBytes final {
    uint8_t byteForPort[kMpxMidiPorts]{};
    bool hasByteForPort[kMpxMidiPorts]{};
};

/// Write the MIDI slot across this packet's data blocks.
///
/// `payload` points at the first data block -- past the CIP header -- and must
/// hold `blocksInPacket * dbs * 4` bytes. Must run AFTER the PCM snapshot: the
/// packetizer lays defaults into every slot including the PCM ones and then
/// overwrites PCM, so composing MIDI earlier would be erased.
///
/// Blocks beyond the cap, and blocks whose port has no byte, keep the empty
/// quadlet the packet was armed with -- which is already correct on the wire.
inline void WriteMpxMidiSlot(uint8_t* payload, uint32_t blocksInPacket,
                             uint8_t dbc, const MpxMidiGeometry& geometry,
                             const MpxMidiPacketBytes& bytes) noexcept {
    if (payload == nullptr || !geometry.Valid()) return;

    const uint32_t blocks = blocksInPacket < kMpxMidiMaxBlocksPerPacket
                                ? blocksInPacket
                                : kMpxMidiMaxBlocksPerPacket;
    for (uint32_t f = 0; f < blocks; ++f) {
        const uint8_t port = static_cast<uint8_t>(
            (geometry.dbcAligned ? (static_cast<uint32_t>(dbc) + f) : f) %
            kMpxMidiPorts);

        uint32_t word = kMpxMidiEmptyQuadlet;
        if (port < geometry.portCount && bytes.hasByteForPort[port]) {
            // label 0x81 = one valid byte, then the byte, then zero fill.
            word = (static_cast<uint32_t>(kMpxMidiLabelBase + 1u) << 24) |
                   (static_cast<uint32_t>(bytes.byteForPort[port]) << 16);
        }

        uint8_t* slot =
            payload + (static_cast<size_t>(f) * geometry.dbs +
                       geometry.midiSlotIndex) * 4u;
        // Big-endian on the wire, like all IEEE 1394 payload.
        slot[0] = static_cast<uint8_t>(word >> 24);
        slot[1] = static_cast<uint8_t>(word >> 16);
        slot[2] = static_cast<uint8_t>(word >> 8);
        slot[3] = static_cast<uint8_t>(word);
    }
}

/// Which port block `f` of a packet with this DBC belongs to.
[[nodiscard]] constexpr uint8_t MpxMidiPortForBlock(uint8_t dbc, uint32_t f,
                                                    bool dbcAligned) noexcept {
    return static_cast<uint8_t>(
        (dbcAligned ? (static_cast<uint32_t>(dbc) + f) : f) % kMpxMidiPorts);
}

} // namespace ASFW::Encoding
