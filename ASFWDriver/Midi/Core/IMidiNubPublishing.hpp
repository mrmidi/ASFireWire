//
// IMidiNubPublishing.hpp
// ASFWDriver
//
// What endpoint publication needs from the thing that creates MIDI nubs. A seam
// so the decision "which endpoints get a MIDI service" is host-tested without a
// DriverKit nub behind it.
//

#pragma once

#include <cstdint>

#include "../Capabilities/MidiEndpointCapabilities.hpp"

namespace ASFW::Midi {

class IMidiNubPublishing {
public:
    virtual ~IMidiNubPublishing() = default;

    /// Publish a MIDI service for the endpoint. True for success and for the
    /// ordinary "nothing to publish" outcome; false only on a real failure.
    [[nodiscard]] virtual bool EnsureNub(const MidiEndpointCapabilities& caps,
                                         uint64_t endpointId,
                                         const char* deviceName,
                                         const char* model,
                                         const char* manufacturer) noexcept = 0;

    virtual void TerminateNub(uint64_t endpointId) noexcept = 0;
};

} // namespace ASFW::Midi
