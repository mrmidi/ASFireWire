// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// Backend calls the native generation-checked IRM API. No persistent kernel
// allocation object: see CRASH-2026-09-12.md. A failed operation is not retried.
template<class Backend> class GenerationReservation {
    Backend *backend_ = nullptr;
    uint32_t generation_ = 0;
    uint8_t channel_ = 0;
    bool channelHeld_ = false, bandwidthHeld_ = false, released_ = false;
public:
    GenerationReservation() = default;
    GenerationReservation(const GenerationReservation &) = delete;
    GenerationReservation &operator=(const GenerationReservation &) = delete;
    ~GenerationReservation() { (void)release(); }
    int32_t acquire(Backend &backend, uint32_t generation, uint8_t channel) {
        backend_ = &backend; generation_ = generation; channel_ = channel;
        auto status = backend.channel(true, channel_, generation_);
        if (status) return status;
        channelHeld_ = true;
        status = backend.bandwidth(true, 512, generation_);
        bandwidthHeld_ = !status;
        return status;
    }
    int32_t release() {
        if (released_ || !backend_) return 0;
        released_ = true;
        int32_t status = 0;
        if (bandwidthHeld_) status = backend_->bandwidth(false, 512, generation_);
        if (channelHeld_) {
            const auto channelStatus = backend_->channel(false, channel_, generation_);
            if (!status) status = channelStatus;
        }
        return status;
    }
};
