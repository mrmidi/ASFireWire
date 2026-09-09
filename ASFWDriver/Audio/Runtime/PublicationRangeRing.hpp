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

    void Record(uint64_t epoch,
                uint64_t firstFrame,
                uint64_t endFrame,
                uint64_t hostTicksEarliest,
                uint64_t hostTicksLatest) noexcept {
        if (endFrame <= firstFrame) return;

        const uint64_t n = count_.load(std::memory_order_relaxed);
        auto& entry = entries_[n % kPublicationRangeSlots];

        // Sequence 0 signals write-in-progress.
        entry.sequence.store(0, std::memory_order_relaxed);
        entry.epoch.store(epoch, std::memory_order_relaxed);
        entry.firstFrame.store(firstFrame, std::memory_order_relaxed);
        entry.endFrame.store(endFrame, std::memory_order_relaxed);
        entry.hostTicksEarliest.store(hostTicksEarliest, std::memory_order_relaxed);
        entry.hostTicksLatest.store(hostTicksLatest, std::memory_order_relaxed);
        entry.sequence.store(n + 1, std::memory_order_release);
        count_.store(n + 1, std::memory_order_release);
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
        if (entry.sequence.load(std::memory_order_acquire) != expected) {
            return false;
        }
        out.epoch = entry.epoch.load(std::memory_order_relaxed);
        out.firstFrame = entry.firstFrame.load(std::memory_order_relaxed);
        out.endFrame = entry.endFrame.load(std::memory_order_relaxed);
        out.hostTicksEarliest = entry.hostTicksEarliest.load(std::memory_order_relaxed);
        out.hostTicksLatest = entry.hostTicksLatest.load(std::memory_order_relaxed);
        return entry.sequence.load(std::memory_order_acquire) == expected;
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
        if (frameCount == 0) return PublicationCoverageResult::Resolved;
        const uint64_t requiredEnd = firstFrame + frameCount;

        const uint64_t n = count_.load(std::memory_order_acquire);
        if (n == 0) return PublicationCoverageResult::Pending;

        const uint64_t oldest = n > kPublicationRangeSlots ? n - kPublicationRangeSlots : 0;

        SnapshotEntry oldestSnap{};
        if (!ReadEntry(entries_[oldest % kPublicationRangeSlots], oldest, oldestSnap)) {
            return PublicationCoverageResult::AgedOut;
        }

        // If the packet begins before the oldest retained entry, it has aged out.
        if (firstFrame < oldestSnap.firstFrame) {
            return PublicationCoverageResult::AgedOut;
        }

        uint64_t maxEarliest = 0;
        uint64_t maxLatest = 0;
        uint64_t cursor = firstFrame;
        bool sawEpochMismatch = false;

        // Scan retained entries from oldest to newest to find covering segments.
        for (uint64_t index = oldest; index < n; ++index) {
            const auto& entry = entries_[index % kPublicationRangeSlots];
            SnapshotEntry snap{};
            if (!ReadEntry(entry, index, snap)) {
                return PublicationCoverageResult::AgedOut;
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
                sawEpochMismatch = true;
                return PublicationCoverageResult::EpochMismatch;
            }

            maxEarliest = std::max(maxEarliest, snap.hostTicksEarliest);
            maxLatest = std::max(maxLatest, snap.hostTicksLatest);
            cursor = std::min(snap.endFrame, requiredEnd);

            if (cursor >= requiredEnd) {
                break;
            }
        }

        // Check if entries were overwritten during our walk.
        if (count_.load(std::memory_order_acquire) > oldest + kPublicationRangeSlots) {
            return PublicationCoverageResult::AgedOut;
        }

        if (cursor < requiredEnd) {
            if (sawEpochMismatch) return PublicationCoverageResult::EpochMismatch;
            return PublicationCoverageResult::Pending;
        }

        outEarliest = maxEarliest;
        outLatest = maxLatest;
        return PublicationCoverageResult::Resolved;
    }

    void Reset() noexcept {
        count_.store(0, std::memory_order_relaxed);
        for (auto& entry : entries_) {
            entry.sequence.store(0, std::memory_order_relaxed);
            entry.epoch.store(0, std::memory_order_relaxed);
            entry.firstFrame.store(0, std::memory_order_relaxed);
            entry.endFrame.store(0, std::memory_order_relaxed);
            entry.hostTicksEarliest.store(0, std::memory_order_relaxed);
            entry.hostTicksLatest.store(0, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] uint64_t Count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> count_{0};
    PublicationRangeEntry entries_[kPublicationRangeSlots]{};
};

} // namespace ASFW::Audio::Runtime
