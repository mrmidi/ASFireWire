#pragma once

#include "../Ports/IDiagSink.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Lab {

// Fixed-size sticky counter store behind Ports::IDiagSink. Relaxed atomics
// so the same object can be incremented from a real-time IO callback and
// read from test code or a StopIO dump; counters only ever grow (sticky)
// until Reset(), which is a non-RT operation.
class StickyCounterSink final : public Ports::IDiagSink {
public:
    static constexpr uint32_t kCapacity = 64;

    StickyCounterSink() noexcept = default;

    void Increment(uint32_t counterId, uint64_t delta) noexcept override {
        if (counterId >= kCapacity) {
            overflowedIds_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        counters_[counterId].fetch_add(delta, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t Value(uint32_t counterId) const noexcept {
        if (counterId >= kCapacity) {
            return 0;
        }
        return counters_[counterId].load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t OverflowedIds() const noexcept {
        return overflowedIds_.load(std::memory_order_relaxed);
    }

    void Reset() noexcept {
        for (auto& counter : counters_) {
            counter.store(0, std::memory_order_relaxed);
        }
        overflowedIds_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> counters_[kCapacity]{};
    std::atomic<uint64_t> overflowedIds_{0};
};

} // namespace ASFW::Lab
