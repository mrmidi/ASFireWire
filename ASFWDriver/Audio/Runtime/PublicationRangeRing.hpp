// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Audio-owned publication receipt history and exact coverage verification.
// Retains actual committed suffixes [copyStart, incomingEnd) with conservative
// visibility host-time bounds.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

inline constexpr uint32_t kPublicationRangeSlots = 256;

enum class PublicationCoverageResult : uint8_t {
    Resolved = 0,
    Pending,
    Gap,
    EpochMismatch,
    AgedOut,
    ReadCollision,
};

struct PublicationRangeEntry final {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint64_t> firstFrame{0};
    std::atomic<uint64_t> endFrame{0};
    std::atomic<uint64_t> hostTicksEarliest{0};
    std::atomic<uint64_t> hostTicksLatest{0};
};

class PublicationRangeRing final {
public:
    PublicationRangeRing() noexcept = default;

    // Single serialized publisher. Reset requires publisher/reader quiescence.
    // All entry operations are SC: a reader accepting the same sequence before
    // and after its scalar loads cannot accept fields from a concurrent rewrite.
    void Record(uint64_t epoch,
                uint64_t firstFrame,
                uint64_t endFrame,
                uint64_t hostTicksEarliest,
                uint64_t hostTicksLatest) noexcept {
        if (endFrame <= firstFrame) return;

        const uint64_t n = count_.load(std::memory_order_seq_cst);
        auto& entry = entries_[n % kPublicationRangeSlots];

        // Sequence 0 signals write-in-progress.
        entry.sequence.store(0, std::memory_order_seq_cst);
        entry.epoch.store(epoch, std::memory_order_seq_cst);
        entry.firstFrame.store(firstFrame, std::memory_order_seq_cst);
        entry.endFrame.store(endFrame, std::memory_order_seq_cst);
        entry.hostTicksEarliest.store(hostTicksEarliest, std::memory_order_seq_cst);
        entry.hostTicksLatest.store(hostTicksLatest, std::memory_order_seq_cst);
        entry.sequence.store(n + 1, std::memory_order_seq_cst);
        count_.store(n + 1, std::memory_order_seq_cst);
    }

    struct SnapshotEntry final {
        uint64_t epoch{0};
        uint64_t firstFrame{0};
        uint64_t endFrame{0};
        uint64_t hostTicksEarliest{0};
        uint64_t hostTicksLatest{0};
    };

    [[nodiscard]] static bool ReadEntry(const PublicationRangeEntry& entry,
                                        uint64_t index,
                                        SnapshotEntry& out) noexcept {
        const uint64_t expected = index + 1;
        if (entry.sequence.load(std::memory_order_seq_cst) != expected) {
            return false;
        }
        out.epoch = entry.epoch.load(std::memory_order_seq_cst);
        out.firstFrame = entry.firstFrame.load(std::memory_order_seq_cst);
        out.endFrame = entry.endFrame.load(std::memory_order_seq_cst);
        out.hostTicksEarliest = entry.hostTicksEarliest.load(std::memory_order_seq_cst);
        out.hostTicksLatest = entry.hostTicksLatest.load(std::memory_order_seq_cst);
        return entry.sequence.load(std::memory_order_seq_cst) == expected;
    }

    /// Prove whether the requested frame range [firstFrame, firstFrame + frameCount)
    /// is completely covered without holes by retained publications in the specified epoch.
    ///
    /// E0 complete-payload availability is derived as max(hostTicks) over the
    /// contributing publications.
    [[nodiscard]] PublicationCoverageResult LookupPacketCoverage(
        uint64_t targetEpoch,
        uint64_t firstFrame,
        uint32_t frameCount,
        uint64_t& outEarliest,
        uint64_t& outLatest) const noexcept {
        outEarliest = 0;
        outLatest = 0;
        if (frameCount == 0) return PublicationCoverageResult::Resolved;
        // Work is bounded even under a continuously advancing publisher.
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto result = LookupOnce(targetEpoch, firstFrame, frameCount,
                                           outEarliest, outLatest);
            if (result != PublicationCoverageResult::ReadCollision) return result;
        }
        return PublicationCoverageResult::ReadCollision;
    }

#if defined(ASFW_HOST_TEST)
    // Deterministic interleaving after the count snapshot; absent in production.
    void SetLookupSnapshotHook(void (*hook)(void*), void* context) noexcept {
        lookupSnapshotHook_ = hook;
        lookupSnapshotContext_ = context;
    }
#endif

private:
    [[nodiscard]] PublicationCoverageResult LookupOnce(
        uint64_t targetEpoch, uint64_t firstFrame, uint32_t frameCount,
        uint64_t& outEarliest, uint64_t& outLatest) const noexcept {
        const uint64_t requiredEnd = firstFrame + frameCount;

        const uint64_t n = count_.load(std::memory_order_seq_cst);
        if (n == 0) return PublicationCoverageResult::Pending;

#if defined(ASFW_HOST_TEST)
        if (lookupSnapshotHook_) lookupSnapshotHook_(lookupSnapshotContext_);
#endif
        const uint64_t oldest = n > kPublicationRangeSlots ? n - kPublicationRangeSlots : 0;

        SnapshotEntry oldestSnap{};
        if (!ReadEntry(entries_[oldest % kPublicationRangeSlots], oldest, oldestSnap)) {
            return PublicationCoverageResult::ReadCollision;
        }

        if (oldestSnap.epoch != targetEpoch) {
            return PublicationCoverageResult::EpochMismatch;
        }
        // Only a successfully read, same-epoch retention boundary proves eviction.
        if (firstFrame < oldestSnap.firstFrame) {
            return oldest > 0 ? PublicationCoverageResult::AgedOut
                              : PublicationCoverageResult::Gap;
        }

        uint64_t maxEarliest = 0;
        uint64_t maxLatest = 0;
        uint64_t cursor = firstFrame;

        // Scan retained entries from oldest to newest to find covering segments.
        for (uint64_t index = oldest; index < n; ++index) {
            const auto& entry = entries_[index % kPublicationRangeSlots];
            SnapshotEntry snap{};
            if (!ReadEntry(entry, index, snap)) {
                return PublicationCoverageResult::ReadCollision;
            }

            if (snap.endFrame <= cursor) {
                // Precedes the unverified cursor; continue advancing.
                continue;
            }

            if (snap.firstFrame > cursor) {
                // Uncovered gap encountered in publication history.
                return PublicationCoverageResult::Gap;
            }

            // Entry covers [cursor, min(snap.endFrame, requiredEnd)).
            if (snap.epoch != targetEpoch) {
                return PublicationCoverageResult::EpochMismatch;
            }

            maxEarliest = std::max(maxEarliest, snap.hostTicksEarliest);
            maxLatest = std::max(maxLatest, snap.hostTicksLatest);
            cursor = std::min(snap.endFrame, requiredEnd);

            if (cursor >= requiredEnd) {
                break;
            }
        }

        // Each contributing receipt was copied coherently. Later eviction of an
        // already copied receipt does not invalidate its immutable publication bounds.
        if (cursor < requiredEnd) {
            return PublicationCoverageResult::Pending;
        }

        outEarliest = maxEarliest;
        outLatest = maxLatest;
        return PublicationCoverageResult::Resolved;
    }

public:
    void Reset() noexcept {
        count_.store(0, std::memory_order_seq_cst);
        for (auto& entry : entries_) {
            entry.sequence.store(0, std::memory_order_seq_cst);
            entry.epoch.store(0, std::memory_order_seq_cst);
            entry.firstFrame.store(0, std::memory_order_seq_cst);
            entry.endFrame.store(0, std::memory_order_seq_cst);
            entry.hostTicksEarliest.store(0, std::memory_order_seq_cst);
            entry.hostTicksLatest.store(0, std::memory_order_seq_cst);
        }
    }

    [[nodiscard]] uint64_t Count() const noexcept {
        return count_.load(std::memory_order_seq_cst);
    }

private:
#if defined(ASFW_HOST_TEST)
    void (*lookupSnapshotHook_)(void*){nullptr};
    void* lookupSnapshotContext_{nullptr};
#endif
    std::atomic<uint64_t> count_{0};
    PublicationRangeEntry entries_[kPublicationRangeSlots]{};
};

} // namespace ASFW::Audio::Runtime
