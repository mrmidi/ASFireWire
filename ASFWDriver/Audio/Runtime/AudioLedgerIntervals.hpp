// AudioLedgerIntervals.hpp
// ASFW - Measuring the path intervals the latency ledger only asserts.
//
// Four spans in the ledger are written down as geometry rather than as
// observations: I1 (client write -> payload finality) and J4 (capture publish
// -> client read) are variable waits with no recorded spread, and I2 (finality
// -> transmission) and J3 (packet received -> frame decoded) are configured
// nominals whose real distribution has never been measured. Dispatch delay sits
// inside I2 and J3 and is bounded by nothing in the geometry, which is exactly
// why the nominal cannot stand in for the measurement.
//
// Two primitives serve all four. A span whose endpoints are observed at the
// same site needs only the ladder; a span whose endpoints are observed at
// different sites needs the ring to carry the first endpoint forward.

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

inline constexpr uint32_t kLedgerIntervalBuckets = 8;

/// Elapsed-time ladder, in microseconds. The bottom bucket is one isoch cycle
/// because that is the quantum every one of these intervals is built from; the
/// ladder then doubles to 8 ms, beyond which a physical interval has stopped
/// being a distribution question and become a fault.
[[nodiscard]] constexpr uint32_t LedgerIntervalBucket(uint64_t micros) noexcept {
    if (micros <=  125) return 0;
    if (micros <=  250) return 1;
    if (micros <=  500) return 2;
    if (micros <= 1000) return 3;
    if (micros <= 2000) return 4;
    if (micros <= 4000) return 5;
    if (micros <= 8000) return 6;
    return kLedgerIntervalBuckets - 1;
}

/// One ledger interval's observed distribution.
///
/// `unresolved` is part of the measurement, not an error channel: it counts
/// samples whose first endpoint could not be recovered, so a thin histogram
/// can never be mistaken for a well-behaved interval that simply occurred
/// rarely.
struct LedgerIntervalStats final {
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> unresolved{0};
    std::atomic<uint64_t> sumMicros{0};
    std::atomic<uint64_t> maxMicros{0};
    std::atomic<uint64_t> minMicros{~uint64_t{0}};
    std::atomic<uint64_t> histogram[kLedgerIntervalBuckets]{};

    void Record(uint64_t micros) noexcept {
        samples.fetch_add(1, std::memory_order_relaxed);
        sumMicros.fetch_add(micros, std::memory_order_relaxed);
        histogram[LedgerIntervalBucket(micros)].fetch_add(
            1, std::memory_order_relaxed);
        uint64_t seen = maxMicros.load(std::memory_order_relaxed);
        while (micros > seen &&
               !maxMicros.compare_exchange_weak(seen, micros,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
        }
        seen = minMicros.load(std::memory_order_relaxed);
        while (micros < seen &&
               !minMicros.compare_exchange_weak(seen, micros,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
        }
    }

    void CountUnresolved() noexcept {
        unresolved.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t MeanMicros() const noexcept {
        const uint64_t n = samples.load(std::memory_order_relaxed);
        return n == 0 ? 0 : sumMicros.load(std::memory_order_relaxed) / n;
    }

    /// True once anything landed in the top bucket, which is the only part of
    /// this distribution worth waking a log line for.
    [[nodiscard]] bool SawOverflow() const noexcept {
        return histogram[kLedgerIntervalBuckets - 1].load(
                   std::memory_order_relaxed) != 0;
    }

    void Reset() noexcept {
        samples.store(0, std::memory_order_relaxed);
        unresolved.store(0, std::memory_order_relaxed);
        sumMicros.store(0, std::memory_order_relaxed);
        maxMicros.store(0, std::memory_order_relaxed);
        minMicros.store(~uint64_t{0}, std::memory_order_relaxed);
        for (auto& bucket : histogram) {
            bucket.store(0, std::memory_order_relaxed);
        }
    }
};

inline constexpr uint32_t kLedgerStampSlots = 64;

/// A bounded record of when a monotonically increasing cursor passed each of
/// its recent values, so a later site can ask when an earlier event happened.
///
/// The cursors these hold -- a published frame end, a finality frontier -- only
/// ever move forward, so the entries are ordered and the first record whose
/// cursor exceeds a value is the record that covered it. A value older than
/// everything retained is reported as unrecoverable rather than answered from
/// the oldest surviving entry, because that answer would be a plausible
/// understatement of exactly the long intervals worth measuring.
struct LedgerStampRing final {
    struct Entry final {
        std::atomic<uint64_t> cursor{0};
        std::atomic<uint64_t> ticks{0};
    };

    std::atomic<uint64_t> count{0};
    Entry entries[kLedgerStampSlots]{};

    void Record(uint64_t cursor, uint64_t ticks) noexcept {
        const uint64_t n = count.load(std::memory_order_relaxed);
        auto& entry = entries[n % kLedgerStampSlots];
        entry.cursor.store(cursor, std::memory_order_relaxed);
        entry.ticks.store(ticks, std::memory_order_relaxed);
        count.store(n + 1, std::memory_order_release);
    }

    /// When `value` first became covered. False when nothing retained covers
    /// it: either no record has passed it yet, or the record that did has been
    /// overwritten.
    [[nodiscard]] bool CoveredAt(uint64_t value,
                                 uint64_t& outTicks) const noexcept {
        const uint64_t n = count.load(std::memory_order_acquire);
        if (n == 0) return false;
        const uint64_t oldest = n > kLedgerStampSlots ? n - kLedgerStampSlots : 0;
        bool found = false;
        uint64_t ticks = 0;
        uint64_t foundIndex = 0;
        for (uint64_t index = oldest; index < n; ++index) {
            const auto& entry = entries[index % kLedgerStampSlots];
            if (entry.cursor.load(std::memory_order_relaxed) <= value) continue;
            ticks = entry.ticks.load(std::memory_order_relaxed);
            foundIndex = index;
            found = true;
            break;
        }
        if (!found) return false;
        // An answer of exactly the oldest retained record is not an answer once
        // the ring has lapped: cursors only increase, so a discarded earlier
        // record may have covered this value first, and taking the surviving
        // one would understate precisely the long intervals worth measuring.
        if (foundIndex == oldest && oldest > 0) return false;
        // A writer that lapped during the scan may have replaced what was read.
        if (count.load(std::memory_order_acquire) > oldest + kLedgerStampSlots) {
            return false;
        }
        outTicks = ticks;
        return true;
    }

    void Reset() noexcept {
        count.store(0, std::memory_order_relaxed);
        for (auto& entry : entries) {
            entry.cursor.store(0, std::memory_order_relaxed);
            entry.ticks.store(0, std::memory_order_relaxed);
        }
    }
};

} // namespace ASFW::Audio::Runtime
