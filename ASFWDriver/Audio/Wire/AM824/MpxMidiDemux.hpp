//
// MpxMidiDemux.hpp
// ASFWDriver
//
// MPX-MIDI demultiplexing: one AM824 slot per data block, eight ports
// time-multiplexed across successive blocks.
//
// Content knowledge, not transport. It reads data blocks that transport
// delivered opaquely and hands port-indexed bytes to a sink; it never touches
// DMA, descriptors or MMIO.
//
// Wire behaviour cross-checked against Linux read_midi_messages
// (amdtp-am824.c:323-344) and Focusrite readMIDIQuadlet (Saffire!0xcdfc).
// Fresh implementation; the references are behavioural evidence only.
//
// Pure logic: no DriverKit dependency, no allocation, no logging.
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../Ports/IMidiByteSink.hpp"

namespace ASFW::Encoding {

/// Ports multiplexed into one MPX-MIDI slot.
inline constexpr uint8_t kMpxMidiPorts = 8;

/// IEC 61883-6 MIDI conformant data label. The low two bits are the count of
/// valid bytes in the quadlet, so 0x80 means "no MIDI byte this block".
inline constexpr uint8_t kMpxMidiLabelBase = 0x80;
inline constexpr uint8_t kMpxMidiMaxBytesPerQuadlet = 3;

/// Geometry needed to find and interpret the MIDI slot.
struct MpxMidiGeometry final {
    /// AM824 slots per data block (the CIP DBS).
    uint8_t dbs{0};
    /// Index of the MPX-MIDI slot within each data block.
    uint8_t midiSlotIndex{0};
    /// Physical ports actually present. Bytes for a higher port index are
    /// dropped rather than delivered to an endpoint that does not exist.
    uint8_t portCount{0};
    /// Whether the port rotation counts from the packet's DBC.
    ///
    /// Default true: Focusrite implements no unaligned path, and Linux applies
    /// its CIP_UNALIGHED_DBC quirk only to named device families.
    bool dbcAligned{true};

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return dbs > 0 && midiSlotIndex < dbs && portCount > 0 &&
               portCount <= kMpxMidiPorts;
    }
};

struct MpxMidiDemuxCounters final {
    uint64_t blocksInspected{0};
    uint64_t bytesDelivered{0};
    /// Quadlets whose label was 0x80: no byte this block. The idle filler, and
    /// by far the common case -- not an error.
    uint64_t emptyQuadlets{0};
    /// Quadlets whose label was not in 0x80..0x83 at all. A nonzero value means
    /// the slot index is wrong or the formation is not what we think.
    uint64_t invalidLabels{0};
    /// Bytes addressed to a port beyond portCount.
    uint64_t droppedForUnknownPort{0};
};

/// Lift MIDI bytes out of one packet's data blocks.
///
/// `dataBlocks` points at the first data block -- past the receive prefix and
/// the CIP header -- and must be at least `eventCount * dbs * 4` bytes. Every
/// bound is the caller's to establish: this function trusts the geometry it is
/// given, because it runs once per received packet.
///
/// Deliberately takes the event count from the wire payload, never from the
/// number of PCM samples successfully written: in MIDI-only operation there is
/// no PCM writer bound at all, and the MIDI must still come through.
inline void DemuxMpxMidi(const uint8_t* dataBlocks, uint32_t eventCount,
                         uint8_t dbc, const MpxMidiGeometry& geometry,
                         ASFW::Audio::Ports::IMidiByteSink& sink,
                         MpxMidiDemuxCounters& counters) noexcept {
    if (dataBlocks == nullptr || eventCount == 0 || !geometry.Valid()) return;

    const uint32_t blockQuadlets = geometry.dbs;
    for (uint32_t f = 0; f < eventCount; ++f) {
        const uint8_t* quadlet =
            dataBlocks + (static_cast<size_t>(f) * blockQuadlets +
                          geometry.midiSlotIndex) * 4u;
        ++counters.blocksInspected;

        // Big-endian on the wire, like all IEEE 1394 payload: the label is the
        // first byte, then up to three MIDI bytes.
        const uint8_t label = quadlet[0];
        if (label == kMpxMidiLabelBase) {
            ++counters.emptyQuadlets;
            continue;
        }
        // Range-check the label rather than masking it. Focusrite masks
        // label & 3, which agrees for 0x80..0x83 but would also accept a
        // different AM824 label as MIDI; Linux's explicit check does not.
        if (label < kMpxMidiLabelBase ||
            label > kMpxMidiLabelBase + kMpxMidiMaxBytesPerQuadlet) {
            ++counters.invalidLabels;
            continue;
        }
        const uint8_t count = static_cast<uint8_t>(label - kMpxMidiLabelBase);

        const uint8_t port = static_cast<uint8_t>(
            (geometry.dbcAligned ? (static_cast<uint32_t>(dbc) + f) : f) %
            kMpxMidiPorts);
        if (port >= geometry.portCount) {
            counters.droppedForUnknownPort += count;
            continue;
        }

        // A single quadlet can carry a whole three-byte Note On. Both
        // references only ever *send* one byte per quadlet, which is not a
        // reason to assume that on receive.
        sink.DeliverMidiBytes(port, quadlet + 1, count);
        counters.bytesDelivered += count;
    }
}

} // namespace ASFW::Encoding
