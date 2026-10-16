//
// IMidiReceiveWake.hpp
// ASFWDriver
//
// "Bytes arrived for the MIDI service": the one thing the receive path asks of
// the MIDI side besides the byte rings themselves.
//
// The receive queue must never wait on a CoreMIDI client, so the call is a
// one-way hand-off that returns immediately; the MIDI service converts and
// sends on its own queue. An implementation must tolerate a call at any time,
// including after its endpoint has been unpublished (then it does nothing).
//

#pragma once

#include <cstdint>

namespace ASFW::Audio::Ports {

class IMidiReceiveWake {
public:
    virtual ~IMidiReceiveWake() = default;

    /// Device -> host bytes were queued for `endpointId`. Non-blocking.
    virtual void MidiReceived(uint64_t endpointId) noexcept = 0;
};

} // namespace ASFW::Audio::Ports
