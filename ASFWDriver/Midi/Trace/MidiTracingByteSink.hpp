//
// MidiTracingByteSink.hpp
// ASFWDriver
//
// An IMidiByteSink decorator that logs each completed device -> host message
// and forwards every byte unchanged.
//
// A decorator rather than logging inside the demultiplexer or the ring sink:
// both of those are pure-logic headers with an explicit no-logging contract and
// both run per data block. This sits between them, sees exactly the bytes the
// real sink will receive, and can be left out entirely when tracing is off.
//

#pragma once

#include <cstdint>

#include "MidiMessageTrace.hpp"
#include "MidiTraceLog.hpp"
#include "../../Audio/Ports/IMidiByteSink.hpp"
#include "../../Audio/Wire/AM824/MpxMidiDemux.hpp"

#include <DriverKit/IOLib.h>

namespace ASFW::Midi::Trace {

class MidiTracingByteSink final : public ASFW::Audio::Ports::IMidiByteSink {
public:
    /// `downstream` receives every byte whether or not tracing is enabled;
    /// `counters` is the same struct the demultiplexer writes, and carries the
    /// wire position of the run being delivered.
    void Bind(ASFW::Audio::Ports::IMidiByteSink* downstream,
              const ASFW::Encoding::MpxMidiDemuxCounters* counters) noexcept {
        downstream_ = downstream;
        counters_ = counters;
        ResetAllPorts();
    }

    void Unbind() noexcept {
        downstream_ = nullptr;
        counters_ = nullptr;
        ResetAllPorts();
    }

    [[nodiscard]] bool Bound() const noexcept { return downstream_ != nullptr; }

    void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool Enabled() const noexcept { return enabled_; }

    void DeliverMidiBytes(uint8_t port, const uint8_t* bytes,
                          uint8_t count, uint64_t hostTicks) noexcept override {
        // Forward first and unconditionally. Tracing must never be able to
        // change what CoreMIDI receives, including when it faults on a byte it
        // cannot parse.
        if (downstream_ != nullptr) {
            downstream_->DeliverMidiBytes(port, bytes, count, hostTicks);
        }
        if (!enabled_ || bytes == nullptr || count == 0 ||
            port >= ASFW::Encoding::kMpxMidiPorts) {
            return;
        }

        const uint8_t dbc = counters_ ? counters_->lastDeliveredDbc : 0;
        const uint32_t block = counters_ ? counters_->lastDeliveredBlockIndex : 0;
        // The packet's instant, not now: tracing must report the same time
        // the data path recorded, or the log and the ring disagree.
        const uint64_t now = hostTicks;

        for (uint8_t i = 0; i < count; ++i) {
            DecodedMidiMessage message{};
            if (parsers_[port].Feed(bytes[i], message)) {
                LogDecodedMidi(MidiTraceDirection::kIn, port, message, "dbc", dbc,
                               static_cast<uint8_t>(block), now);
            }
            // A status byte that truncated a System Exclusive is reported after
            // the SysEx it ended; drain it so the message it starts is not lost.
            DecodedMidiMessage deferred{};
            if (parsers_[port].HasDeferred() &&
                parsers_[port].TakeDeferred(deferred)) {
                LogDecodedMidi(MidiTraceDirection::kIn, port, deferred, "dbc", dbc,
                               static_cast<uint8_t>(block), now);
            }
        }
    }

    void MarkMidiDiscontinuity(uint8_t port) noexcept override {
        if (downstream_ != nullptr) {
            downstream_->MarkMidiDiscontinuity(port);
        }
        if (port < ASFW::Encoding::kMpxMidiPorts) {
            // Bytes from either side of a gap must not combine into a message
            // nobody sent -- the same reason the real converter resets here.
            parsers_[port].Reset();
        }
    }

private:
    void ResetAllPorts() noexcept {
        for (auto& parser : parsers_) {
            parser.Reset();
        }
    }

    ASFW::Audio::Ports::IMidiByteSink* downstream_{nullptr};
    const ASFW::Encoding::MpxMidiDemuxCounters* counters_{nullptr};
    bool enabled_{true};
    MidiMessageTrace parsers_[ASFW::Encoding::kMpxMidiPorts]{};
};

} // namespace ASFW::Midi::Trace
