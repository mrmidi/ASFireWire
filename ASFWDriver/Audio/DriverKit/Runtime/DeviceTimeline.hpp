#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct DeviceTimeline final {
    std::atomic<uint64_t> sampleFrame{0};
    std::atomic<uint64_t> hostTicks{0};
    std::atomic<uint32_t> hostNanosPerSampleQ8{0};
    std::atomic<uint64_t> generation{0};

    void Publish(uint64_t inSampleFrame,
                 uint64_t inHostTicks,
                 uint32_t inHostNanosPerSampleQ8) noexcept {
        sampleFrame.store(inSampleFrame, std::memory_order_relaxed);
        hostTicks.store(inHostTicks, std::memory_order_relaxed);
        hostNanosPerSampleQ8.store(inHostNanosPerSampleQ8, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_release);
    }
};

} // namespace ASFW::Audio::Runtime
