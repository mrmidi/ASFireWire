//
// IMidiByteSink.hpp
// ASFWDriver
//
// Where MPX-MIDI bytes lifted off the wire go.
//
// The seam is port-indexed bytes and nothing else. The content layer that
// demultiplexes AM824 slots must not know that the other side is a ring, a
// CoreMIDI endpoint, or a test double; the sink must not know about CIP, DBC
// or data blocks.
//
// The sink copies what it is given into storage it owns. It must never retain
// the pointer: it points into a receive packet whose buffer transport recycles
// as soon as this call returns.
//

#pragma once

#include <cstdint>

namespace ASFW::Audio::Ports {

class IMidiByteSink {
public:
    virtual ~IMidiByteSink() = default;

    /// Deliver 1..3 bytes for one port, copied out before returning.
    ///
    /// `port` is already reduced modulo the 8 ports one MPX slot multiplexes.
    virtual void DeliverMidiBytes(uint8_t port, const uint8_t* bytes,
                                  uint8_t count) noexcept = 0;

    /// The byte stream for `port` is broken here.
    ///
    /// Raised when a packet is dropped or a stream restarts. The consumer must
    /// reset its parser at this point so bytes from either side of the gap
    /// cannot combine into a message nobody sent.
    virtual void MarkMidiDiscontinuity(uint8_t port) noexcept = 0;
};

} // namespace ASFW::Audio::Ports
