//
// MidiNubPublisher.hpp
// ASFWDriver
//
// Creation and teardown of ASFWMidiNub instances, one per endpoint whose
// projected capability can actually carry MIDI.
//
// Mirrors AudioNubPublisher, with one deliberate difference: an endpoint with
// no usable MIDI publishes no nub at all. An empty MIDI device in CoreMIDI is
// worse than none -- the user sees ports and blames their cable.
//

#pragma once

#include <cstdint>

#include "../Capabilities/MidiEndpointCapabilities.hpp"

class IOService;

namespace ASFW::Midi {

class MidiNubPublisher {
public:
    explicit MidiNubPublisher(IOService* driver) noexcept : driver_(driver) {}

    MidiNubPublisher(const MidiNubPublisher&) = delete;
    MidiNubPublisher& operator=(const MidiNubPublisher&) = delete;

    /// Publish a MIDI nub for `caps` unless one already exists for its GUID.
    ///
    /// Returns false only on a real failure; an endpoint with no usable MIDI
    /// returns true having published nothing, because that is a normal
    /// outcome, not an error.
    [[nodiscard]] bool EnsureNub(const MidiEndpointCapabilities& caps,
                                 uint64_t endpointId,
                                 const char* deviceName,
                                 const char* model,
                                 const char* manufacturer) noexcept;

    /// Terminate the nub for `endpointId` if one is published.
    void TerminateNub(uint64_t endpointId) noexcept;

    /// The published nub for `endpointId`, or nullptr. Not retained: valid only
    /// while the nub is published.
    [[nodiscard]] IOService* GetNub(uint64_t endpointId) const noexcept;

private:
    static constexpr uint32_t kMaxNubs = 8;

    struct Entry {
        uint64_t endpointId{0};
        IOService* nub{nullptr};
        bool used{false};
    };

    IOService* driver_{nullptr};
    Entry entries_[kMaxNubs]{};
};

} // namespace ASFW::Midi
