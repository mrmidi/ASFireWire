// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Seqlock.hpp - the one sequence-lock protocol used for published telemetry
// intervals (TX preparation heartbeat, RX capture occupancy).
//
// A single writer brackets a group of relaxed atomic stores with the sequence
// counter (odd while writing). Readers copy the group with relaxed loads and
// accept the copy only if the counter was even and unchanged across it. Readers
// never block the writer, which may be on a real-time path.
//
// The fences are what make this correct under the C++ memory model; relaxed
// loads on both sides of the copy are not enough (Boehm, "Can seqlocks get
// along with programming language memory models?", MSPC 2012):
//
//   writer: seq.fetch_add(1, relaxed); fence(release); stores...;
//           seq.fetch_add(1, release)
//   reader: s0 = seq.load(acquire); loads...; fence(acquire);
//           s1 = seq.load(relaxed); valid iff s0 == s1 and s0 is even
//
// Writers must be serialised by the caller. The data fields themselves must be
// std::atomic (relaxed) so a racing copy is not a data race; the protocol only
// decides whether the copy is consistent.

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace ASFW::Audio::Runtime {

inline void SeqlockWriteBegin(std::atomic<uint64_t>& sequence) noexcept {
    sequence.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

inline void SeqlockWriteEnd(std::atomic<uint64_t>& sequence) noexcept {
    sequence.fetch_add(1, std::memory_order_release);
}

/// Runs `copy` until it observes a consistent snapshot or `attempts` run out.
/// Returns the (even) sequence of the accepted copy, or nullopt. On nullopt the
/// caller must discard whatever `copy` wrote: it may be torn.
template <typename CopyFn>
[[nodiscard]] std::optional<uint64_t> SeqlockTryRead(
    const std::atomic<uint64_t>& sequence, CopyFn&& copy,
    uint32_t attempts = 4) noexcept {
    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        const uint64_t before = sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        copy();
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t after = sequence.load(std::memory_order_relaxed);
        if (before == after) {
            return before;
        }
    }
    return std::nullopt;
}

/// Serialises writers that may race (e.g. two queues both able to close an
/// interval). Non-blocking: a writer that loses simply skips its turn.
class SeqlockWriterGate final {
public:
    [[nodiscard]] bool TryEnter() noexcept {
        return !busy_.exchange(true, std::memory_order_acquire);
    }
    void Leave() noexcept { busy_.store(false, std::memory_order_release); }
    void Reset() noexcept { busy_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> busy_{false};
};

} // namespace ASFW::Audio::Runtime
