//
// MidiRingByteSink.hpp
// ASFWDriver
//
// Adapts the content layer's byte sink onto the device -> host rings.
//
// Runs on the core driver's receive queue, once per received packet, so it
// allocates nothing and takes no lock. It copies each run into the ring
// immediately and never retains the pointer it was handed: that points into a
// receive packet whose buffer transport recycles as soon as the call returns.
//
// Pure logic over the shared block: no DriverKit dependency, host-testable.
//

#pragma once

#include <cstdint>
#include <span>

#include "../../Audio/Ports/IMidiByteSink.hpp"
#include "MidiTransportBlock.hpp"

namespace ASFW::Midi {

class MidiRingByteSink final : public ASFW::Audio::Ports::IMidiByteSink {
public:
    MidiRingByteSink() noexcept = default;

    /// Point at a block for one stream epoch. Passing nullptr detaches.
    void Bind(MidiTransportBlock* block, uint64_t streamEpoch) noexcept {
        block_ = block;
        streamEpoch_ = streamEpoch;
    }

    void Unbind() noexcept { block_ = nullptr; }

    [[nodiscard]] bool Bound() const noexcept { return block_ != nullptr; }

    void DeliverMidiBytes(uint8_t port, const uint8_t* bytes,
                          uint8_t count, uint64_t hostTicks) noexcept override {
        if (block_ == nullptr || bytes == nullptr || count == 0) return;
        if (port >= kMidiPortsPerDirection) return;
        // A stream that restarted under us must not have its predecessor's
        // bytes appended to the new epoch's ring.
        if (!block_->Usable(streamEpoch_)) return;

        if (!block_->deviceToHost[port].TryWrite({bytes, count}, hostTicks)) {
            // TryWrite already counted the drop and marked the gap. Nothing to
            // do here but not pretend it succeeded.
            ++overflowRuns_;
        }
    }

    void MarkMidiDiscontinuity(uint8_t port) noexcept override {
        if (block_ == nullptr || port >= kMidiPortsPerDirection) return;
        if (!block_->Usable(streamEpoch_)) return;
        block_->deviceToHost[port].MarkDiscontinuity();
    }

    [[nodiscard]] uint64_t OverflowRuns() const noexcept { return overflowRuns_; }

private:
    MidiTransportBlock* block_{nullptr};
    uint64_t streamEpoch_{0};
    uint64_t overflowRuns_{0};
};

} // namespace ASFW::Midi
