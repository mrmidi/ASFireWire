// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

#ifndef ASFW_TX_FLIGHT_RECORDER
#define ASFW_TX_FLIGHT_RECORDER 1
#endif

namespace ASFW::Isoch::Tx {
// Value-only observations. Cycles are not proof of descriptor execution;
// eventTicks is callback/refill entry, not physical interrupt assertion time.
struct TxRefillRecord {
    uint64_t epoch{}, eventTicks{}, readBeforeTicks{}, readAfterTicks{};
    uint64_t completionBefore{}, mappedBefore{}, committedBefore{};
    uint64_t failedPacket{}, expectedSeal{}, observedSeal{};
    uint32_t cycle{}, previousCycle{}, command{}, control{};
    uint32_t previousSlot{}, slot{}, inferredDelta{}, filled{};
    uint32_t source{}, failure{}, flags{}, reserved{};
};
static_assert(sizeof(TxRefillRecord) == 128);

// Single writer: the existing refillInProgress gate, including Start/Stop.
// Consumer cannot inspect mutable history. The first anomaly publishes an
// immutable history with release; one reader claims it with acquire. Neither
// stream restart nor export rearms capture. Object destruction still requires
// the enclosing context's ordinary reader/writer lifetime quiescence.
class TxRefillFlightRecorder {
public:
    static constexpr uint32_t kCapacity = 64;
    void Record(const TxRefillRecord& record, bool freeze) noexcept {
        if constexpr (!ASFW_TX_FLIGHT_RECORDER) return;
        if (state_.load(std::memory_order_relaxed) != 0) return;
        records_[next_] = record;
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
        if (freeze) state_.store(1, std::memory_order_release);
    }
    template<typename Visitor>
    bool ExportOnce(Visitor visitor) const {
        uint32_t expected = 1;
        if (!state_.compare_exchange_strong(expected, 2, std::memory_order_acquire))
            return false;
        for (uint32_t i = 0; i < count_; ++i)
            visitor(i, count_, records_[(next_ + kCapacity - count_ + i) % kCapacity]);
        return true;
    }
private:
    std::array<TxRefillRecord, kCapacity> records_{};
    uint32_t next_{}, count_{};
    mutable std::atomic<uint32_t> state_{0}; // recording, frozen, claimed
};
}
