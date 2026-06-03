#pragma once

#include "AudioClientCursor.hpp"
#include "AudioRtCounters.hpp"
#include "DeviceTimeline.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct AudioTransportControlBlock final {
    std::atomic<uint64_t> generation{0};

    AudioClientCursor client{};
    DeviceTimeline device{};
    AudioRtCounters counters{};

    std::atomic<uint64_t> inputProducedEndFrame{0};
    std::atomic<uint64_t> outputConsumedEndFrame{0};

    std::atomic<uint64_t> inputOverruns{0};
    std::atomic<uint64_t> outputUnderruns{0};
    std::atomic<uint64_t> discontinuities{0};

    void ResetForStart() noexcept {
        generation.fetch_add(1, std::memory_order_acq_rel);

        inputProducedEndFrame.store(0, std::memory_order_release);
        outputConsumedEndFrame.store(0, std::memory_order_release);

        inputOverruns.store(0, std::memory_order_release);
        outputUnderruns.store(0, std::memory_order_release);
        discontinuities.store(0, std::memory_order_release);
    }
};

} // namespace ASFW::Audio::Runtime
