#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class ZtsMirrorPublishResult : uint8_t {
    Published = 0,
    NoNewGeneration,
    NotReady,
};

inline const char* ToString(ZtsMirrorPublishResult result) noexcept {
    switch (result) {
        case ZtsMirrorPublishResult::Published:       return "Published";
        case ZtsMirrorPublishResult::NoNewGeneration: return "NoNewGeneration";
        case ZtsMirrorPublishResult::NotReady:        return "NotReady";
        default:                                      return "Unknown";
    }
}

struct HostClockAnchorSample final {
    uint64_t generation{0};
    uint64_t sampleFrame{0};
    uint64_t hostTicks{0};
    uint32_t hostNanosPerSampleQ8{0};
};

struct HostClockAnchorPublishResult final {
    bool accepted{false};
    bool notifyConsumer{false};
    uint64_t notificationGeneration{0};
};

struct HostClockAnchorState {
    // Single-writer, cross-process latest-value mailbox. `sequence` is odd
    // while the producer replaces the sample and even when the fields form a
    // consistent snapshot. Intermediate anchors may be overwritten before the
    // AudioDriverKit action runs; CoreAudio's configured clock algorithm owns
    // timestamp history and smoothing.
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> sampleFrame{0};
    std::atomic<uint64_t> hostTicks{0};
    std::atomic<uint32_t> hostNanosPerSampleQ8{0};

    std::atomic<uint64_t> anchorUpdates{0};
    std::atomic<uint64_t> mirrorPublications{0};
    std::atomic<uint64_t> invalidUpdates{0};

    void Reset() noexcept {
        sequence.store(0, std::memory_order_relaxed);
        generation.store(0, std::memory_order_relaxed);
        sampleFrame.store(0, std::memory_order_relaxed);
        hostTicks.store(0, std::memory_order_relaxed);
        hostNanosPerSampleQ8.store(0, std::memory_order_relaxed);

        anchorUpdates.store(0, std::memory_order_relaxed);
        mirrorPublications.store(0, std::memory_order_relaxed);
        invalidUpdates.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] HostClockAnchorPublishResult Publish(
        uint64_t nextSampleFrame,
        uint64_t nextHostTicks,
        uint32_t nextHostNanosPerSampleQ8) noexcept {
        if (nextHostTicks == 0 || nextHostNanosPerSampleQ8 == 0) {
            invalidUpdates.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const uint64_t nextGeneration =
            generation.load(std::memory_order_relaxed) + 1;
        sequence.fetch_add(1, std::memory_order_acq_rel);
        sampleFrame.store(nextSampleFrame, std::memory_order_relaxed);
        hostTicks.store(nextHostTicks, std::memory_order_relaxed);
        hostNanosPerSampleQ8.store(
            nextHostNanosPerSampleQ8, std::memory_order_relaxed);
        generation.store(nextGeneration, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
        anchorUpdates.fetch_add(1, std::memory_order_relaxed);
        return {
            .accepted = true,
            .notifyConsumer = true,
            .notificationGeneration = nextGeneration,
        };
    }

    [[nodiscard]] bool TryReadLatest(
        uint64_t afterGeneration,
        HostClockAnchorSample& out) const noexcept {
        for (;;) {
            const uint64_t sequenceBefore =
                sequence.load(std::memory_order_acquire);
            if ((sequenceBefore & 1u) != 0) {
                continue;
            }

            HostClockAnchorSample snapshot{};
            snapshot.generation =
                generation.load(std::memory_order_relaxed);
            snapshot.sampleFrame =
                sampleFrame.load(std::memory_order_relaxed);
            snapshot.hostTicks =
                hostTicks.load(std::memory_order_relaxed);
            snapshot.hostNanosPerSampleQ8 =
                hostNanosPerSampleQ8.load(std::memory_order_relaxed);

            const uint64_t sequenceAfter =
                sequence.load(std::memory_order_acquire);
            if (sequenceBefore == sequenceAfter &&
                (sequenceAfter & 1u) == 0) {
                if (snapshot.generation <= afterGeneration) {
                    return false;
                }
                out = snapshot;
                return true;
            }
        }
    }
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace ASFW::Audio::Runtime
