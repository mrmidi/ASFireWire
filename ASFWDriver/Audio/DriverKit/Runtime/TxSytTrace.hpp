#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// One observed/produced SYT decision on the live replay TX path.
struct TxSytTraceSample final {
    uint64_t packetIndex{0};        // transmit packet index this decision is for
    uint32_t sourceCycle{0};        // device source packet cycle (decoded)
    uint32_t outCycle{0};           // our transmit cycle the SYT is re-anchored to
    uint32_t sytOffsetDelayFree{0}; // replayed delay-free offset (rx - rxDelay)
    uint32_t txDelayTicks{0};       // transfer delay re-added on transmit
    uint16_t observedRxSyt{0};      // device's original SYT, reconstructed
    uint16_t txSyt{0};              // SYT we are sending
};

// Single-writer (TX preparation, audio queue), single-reader (watchdog),
// latest-value trace of the live replay SYT decision. `sequence` is odd while
// the writer is replacing the fields and even when they form a consistent
// snapshot. Diagnostics only: intermediate decisions may be overwritten before
// the watchdog samples; that is intended (we only want "the last decision").
class TxSytTraceLatest final {
public:
    void Reset() noexcept {
        sequence_.store(0, std::memory_order_relaxed);
        updates_.store(0, std::memory_order_relaxed);
    }

    void Publish(const TxSytTraceSample& s) noexcept {
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        packetIndex_.store(s.packetIndex, std::memory_order_relaxed);
        sourceCycle_.store(s.sourceCycle, std::memory_order_relaxed);
        outCycle_.store(s.outCycle, std::memory_order_relaxed);
        sytOffsetDelayFree_.store(
            s.sytOffsetDelayFree, std::memory_order_relaxed);
        txDelayTicks_.store(s.txDelayTicks, std::memory_order_relaxed);
        observedRxSyt_.store(s.observedRxSyt, std::memory_order_relaxed);
        txSyt_.store(s.txSyt, std::memory_order_relaxed);
        updates_.fetch_add(1, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    // Returns false until at least one decision has been published. `outUpdates`
    // reports the total decisions so far so the reader can show how many were
    // collapsed since it last sampled.
    [[nodiscard]] bool ReadLatest(TxSytTraceSample& out,
                                  uint64_t& outUpdates) const noexcept {
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {
                continue;
            }
            TxSytTraceSample s{};
            s.packetIndex = packetIndex_.load(std::memory_order_relaxed);
            s.sourceCycle = sourceCycle_.load(std::memory_order_relaxed);
            s.outCycle = outCycle_.load(std::memory_order_relaxed);
            s.sytOffsetDelayFree =
                sytOffsetDelayFree_.load(std::memory_order_relaxed);
            s.txDelayTicks = txDelayTicks_.load(std::memory_order_relaxed);
            s.observedRxSyt = observedRxSyt_.load(std::memory_order_relaxed);
            s.txSyt = txSyt_.load(std::memory_order_relaxed);
            const uint64_t u = updates_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                if (u == 0) {
                    return false;
                }
                out = s;
                outUpdates = u;
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint64_t> packetIndex_{0};
    std::atomic<uint32_t> sourceCycle_{0};
    std::atomic<uint32_t> outCycle_{0};
    std::atomic<uint32_t> sytOffsetDelayFree_{0};
    std::atomic<uint32_t> txDelayTicks_{0};
    std::atomic<uint16_t> observedRxSyt_{0};
    std::atomic<uint16_t> txSyt_{0};
    std::atomic<uint64_t> updates_{0};
};

} // namespace ASFW::Audio::Runtime
