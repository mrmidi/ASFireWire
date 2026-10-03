// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// Exact cumulative RX clock observation in the MOTU 24.576 MHz SPH domain.
// `streamGeneration` is the AudioTransportControlBlock start generation, not
// a publication counter. A consumer must rebase when it changes.
struct MotuRxSphClockSample final {
    uint64_t streamGeneration{0};
    uint64_t rxFrames{0};
    int64_t rxTicks{0};
};

// Single-writer (master MOTU RX consumer), single-reader (future TX servo),
// latest-value bridge. The sequence protects the relationship between frames,
// ticks and stream generation; readers may miss intermediate publications but
// never accept fields from different observations.
//
// ResetForStart runs while the isoch contexts are quiesced, so Reset() and
// Publish() do not have concurrent writers. All fields remain atomic because
// the TX reader is on a different queue.
class MotuRxSphClockLatest final {
  public:
    void Reset(uint64_t streamGeneration) noexcept {
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        streamGeneration_.store(streamGeneration, std::memory_order_relaxed);
        rxFrames_.store(0, std::memory_order_relaxed);
        rxTicks_.store(0, std::memory_order_relaxed);
        updates_.store(0, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] bool Publish(const MotuRxSphClockSample& sample) noexcept {
        if (sample.streamGeneration == 0 || sample.rxFrames == 0 || sample.rxTicks <= 0 ||
            streamGeneration_.load(std::memory_order_acquire) != sample.streamGeneration) {
            return false;
        }

        const uint64_t updates = updates_.load(std::memory_order_relaxed);
        if (updates != 0 && (sample.rxFrames <= rxFrames_.load(std::memory_order_relaxed) ||
                             sample.rxTicks <= rxTicks_.load(std::memory_order_relaxed))) {
            return false;
        }

        sequence_.fetch_add(1, std::memory_order_acq_rel);
        streamGeneration_.store(sample.streamGeneration, std::memory_order_relaxed);
        rxFrames_.store(sample.rxFrames, std::memory_order_relaxed);
        rxTicks_.store(sample.rxTicks, std::memory_order_relaxed);
        updates_.store(updates + 1, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool ReadLatest(MotuRxSphClockSample& out, uint64_t& outUpdates) const noexcept {
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {
                continue;
            }

            const MotuRxSphClockSample sample{
                .streamGeneration = streamGeneration_.load(std::memory_order_relaxed),
                .rxFrames = rxFrames_.load(std::memory_order_relaxed),
                .rxTicks = rxTicks_.load(std::memory_order_relaxed),
            };
            const uint64_t updates = updates_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                if (updates == 0) {
                    return false;
                }
                out = sample;
                outUpdates = updates;
                return true;
            }
        }
        return false;
    }

  private:
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint64_t> streamGeneration_{0};
    std::atomic<uint64_t> rxFrames_{0};
    std::atomic<int64_t> rxTicks_{0};
    std::atomic<uint64_t> updates_{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the RX-to-TX SPH bridge must stay lock-free");
static_assert(std::atomic<int64_t>::is_always_lock_free,
              "the RX-to-TX SPH bridge must stay lock-free");

} // namespace ASFW::Audio::Runtime
