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

/// Why a lookup could not produce an interval. The two failures are different
/// facts and must not share a counter: `Pending` says the span had not closed
/// yet -- for J4, that the reader asked for a frame the writer had not decoded,
/// which is the reader running ahead and not a measurement failure at all --
/// while `AgedOut` says the endpoint existed and the ring lost it.
enum class LedgerLookup : uint8_t { Resolved, Pending, AgedOut };

/// One ledger interval's observed distribution.
///
/// `unresolved` is part of the measurement, not an error channel: it counts
/// samples whose first endpoint could not be recovered, so a thin histogram
/// can never be mistaken for a well-behaved interval that simply occurred
/// rarely.
struct LedgerIntervalStats final {
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> unresolved{0};
    /// Candidates whose endpoints were both recovered but did not order: the
    /// span's end preceded its start. These are not measurement gaps, they are
    /// evidence that one of the two endpoints is stamped at the wrong event,
    /// and they must be counted rather than dropped -- `samples + pending +
    /// unresolved + invalid` is the number of candidates the site considered.
    std::atomic<uint64_t> invalid{0};
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

    /// Samples whose span had not closed when they were looked up. Kept apart
    /// from `unresolved`, which means the endpoint was lost.
    std::atomic<uint64_t> pending{0};

    void CountUnresolved() noexcept {
        unresolved.fetch_add(1, std::memory_order_relaxed);
    }
    void CountPending() noexcept {
        pending.fetch_add(1, std::memory_order_relaxed);
    }
    /// The endpoints were found but did not order. See `invalid`.
    void CountInvalid() noexcept {
        invalid.fetch_add(1, std::memory_order_relaxed);
    }
    /// Total over the three non-sample outcomes. `Resolved` reaching here means
    /// the lookup succeeded and the caller rejected the pair on its own terms,
    /// which is the reversed-interval case -- it must land in a counter, not
    /// fall through. Leaving it unhandled is what let reversed I1/I2 candidates
    /// disappear from every counter at once.
    void Count(LedgerLookup outcome) noexcept {
        switch (outcome) {
            case LedgerLookup::Pending:  CountPending();   break;
            case LedgerLookup::AgedOut:  CountUnresolved(); break;
            case LedgerLookup::Resolved: CountInvalid();   break;
        }
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
        invalid.store(0, std::memory_order_relaxed);
        pending.store(0, std::memory_order_relaxed);
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
    /// One record. `sequence` is the writing record's index plus one -- zero
    /// means never written or currently being overwritten -- and it is stored
    /// last with release so a reader that observes it also observes the pair.
    struct Entry final {
        std::atomic<uint64_t> sequence{0};
        std::atomic<uint64_t> cursor{0};
        std::atomic<uint64_t> ticks{0};
    };

    std::atomic<uint64_t> count{0};
    Entry entries[kLedgerStampSlots]{};

    /// Single producer. The slot is invalidated before it is rewritten so a
    /// concurrent reader can tell a half-written pair from a whole one: without
    /// that, on the first wrap (count == kLedgerStampSlots, oldest == 0) the
    /// aged-out guard below does not fire -- the contended slot *is* index 0 --
    /// and a lookup could return the incoming cursor beside the outgoing ticks.
    void Record(uint64_t cursor, uint64_t ticks) noexcept {
        const uint64_t n = count.load(std::memory_order_relaxed);
        auto& entry = entries[n % kLedgerStampSlots];
        entry.sequence.store(0, std::memory_order_relaxed);
        entry.cursor.store(cursor, std::memory_order_relaxed);
        entry.ticks.store(ticks, std::memory_order_relaxed);
        entry.sequence.store(n + 1, std::memory_order_release);
        count.store(n + 1, std::memory_order_release);
    }

    /// Read one record, rejecting it unless it still belongs to `index` both
    /// before and after the pair is taken. A rejection means the writer is
    /// recycling that slot, so the record it held is already gone.
    [[nodiscard]] static bool ReadEntry(const Entry& entry,
                                        uint64_t index,
                                        uint64_t& outCursor,
                                        uint64_t& outTicks) noexcept {
        const uint64_t expected = index + 1;
        if (entry.sequence.load(std::memory_order_acquire) != expected) {
            return false;
        }
        outCursor = entry.cursor.load(std::memory_order_relaxed);
        outTicks = entry.ticks.load(std::memory_order_relaxed);
        return entry.sequence.load(std::memory_order_acquire) == expected;
    }

    /// When `value` first became covered. False when nothing retained covers
    /// it: either no record has passed it yet, or the record that did has been
    /// overwritten.
    /// As CoveredAt, but says which of the two failures occurred.
    [[nodiscard]] LedgerLookup Lookup(uint64_t value,
                                      uint64_t& outTicks) const noexcept {
        const uint64_t n = count.load(std::memory_order_acquire);
        if (n == 0) return LedgerLookup::Pending;
        const uint64_t oldest = n > kLedgerStampSlots ? n - kLedgerStampSlots : 0;
        bool found = false;
        uint64_t ticks = 0;
        uint64_t foundIndex = 0;
        for (uint64_t index = oldest; index < n; ++index) {
            const auto& entry = entries[index % kLedgerStampSlots];
            uint64_t entryCursor = 0;
            uint64_t entryTicks = 0;
            if (!ReadEntry(entry, index, entryCursor, entryTicks)) {
                // The only slot a writer can be recycling is the one holding
                // the oldest retained record, so this is that record leaving.
                return LedgerLookup::AgedOut;
            }
            if (entryCursor <= value) continue;
            ticks = entryTicks;
            foundIndex = index;
            found = true;
            break;
        }
        // Nothing retained reaches past the value: the span has not closed yet.
        if (!found) return LedgerLookup::Pending;
        // The answer would be the oldest survivor of a lapped ring, so a
        // discarded earlier record may have covered it first.
        if (foundIndex == oldest && oldest > 0) return LedgerLookup::AgedOut;
        if (count.load(std::memory_order_acquire) > oldest + kLedgerStampSlots) {
            return LedgerLookup::AgedOut;
        }
        outTicks = ticks;
        return LedgerLookup::Resolved;
    }

    [[nodiscard]] bool CoveredAt(uint64_t value,
                                 uint64_t& outTicks) const noexcept {
        return Lookup(value, outTicks) == LedgerLookup::Resolved;
    }

    void Reset() noexcept {
        count.store(0, std::memory_order_relaxed);
        for (auto& entry : entries) {
            entry.sequence.store(0, std::memory_order_relaxed);
            entry.cursor.store(0, std::memory_order_relaxed);
            entry.ticks.store(0, std::memory_order_relaxed);
        }
    }
};

} // namespace ASFW::Audio::Runtime
