//
// MidiNubPublisher.hpp
// ASFWDriver
//
// Creation and teardown of ASFWMidiNub instances, one per endpoint whose
// projected capability can actually carry MIDI.
//
// Mirrors AudioNubPublisher, including the one rule that matters most: the nub
// is configured EXPLICITLY after Create. IOService::Create runs the nub's Start
// before the publisher can set anything, so the endpoint id is handed over with
// SetEndpointId (as AudioNubPublisher hands over SetGuid), the properties are
// set, and only then does Publish register the nub for matching. Nothing the
// nub or the MIDI service needs is ever inferred from a property read that
// races the publisher.
//
// An endpoint with no usable MIDI publishes no nub at all: an empty MIDI device
// in CoreMIDI is worse than none -- the user sees ports and blames their cable.
//
// The publisher is also the receive path's wake target (IMidiReceiveWake): it
// finds the live nub for an endpoint under its lock and fires the one-way
// action, and does nothing for an endpoint that was unpublished meanwhile.
//

#pragma once

#include <cstdint>

#include <DriverKit/IOLib.h>

#include "../Capabilities/MidiEndpointCapabilities.hpp"
#include "../../Audio/Ports/IMidiReceiveWake.hpp"
#include "IMidiNubPublishing.hpp"

class IOService;

namespace ASFW::Midi {

class MidiNubPublisher final : public ASFW::Audio::Ports::IMidiReceiveWake,
                               public IMidiNubPublishing {
public:
    explicit MidiNubPublisher(IOService* driver) noexcept;
    ~MidiNubPublisher() noexcept override;

    MidiNubPublisher(const MidiNubPublisher&) = delete;
    MidiNubPublisher& operator=(const MidiNubPublisher&) = delete;

    /// Publish a MIDI nub for `caps` unless one already exists for the endpoint.
    ///
    /// Returns false only on a real failure; an endpoint with no usable MIDI
    /// returns true having published nothing, because that is a normal outcome,
    /// not an error.
    [[nodiscard]] bool EnsureNub(const MidiEndpointCapabilities& caps,
                                 uint64_t endpointId,
                                 const char* deviceName,
                                 const char* model,
                                 const char* manufacturer) noexcept override;

    /// Terminate the nub for `endpointId` if one is published.
    void TerminateNub(uint64_t endpointId) noexcept override;

    /// Terminate every published nub (service teardown).
    void TerminateAll() noexcept;

    [[nodiscard]] bool HasNub(uint64_t endpointId) const noexcept;

    // IMidiReceiveWake
    void MidiReceived(uint64_t endpointId) noexcept override;

private:
    static constexpr uint32_t kMaxNubs = 8;

    struct Entry {
        uint64_t endpointId{0};
        IOService* nub{nullptr};
        bool used{false};
    };

    IOService* driver_{nullptr};
    mutable IOLock* lock_{nullptr};
    Entry entries_[kMaxNubs]{};
};

} // namespace ASFW::Midi
