#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class ZtsMirrorPublishResult : uint8_t {
    Published = 0,
    NoNewGeneration,
    AlreadyPublished,
    NotReady,
    InvalidTimeline,
};

inline const char* ToString(ZtsMirrorPublishResult result) noexcept {
    switch (result) {
        case ZtsMirrorPublishResult::Published:        return "Published";
        case ZtsMirrorPublishResult::NoNewGeneration:   return "NoNewGeneration";
        case ZtsMirrorPublishResult::AlreadyPublished: return "AlreadyPublished";
        case ZtsMirrorPublishResult::NotReady:         return "NotReady";
        case ZtsMirrorPublishResult::InvalidTimeline:  return "InvalidTimeline";
        default:                                       return "Unknown";
    }
}

struct HostClockAnchorSample final {
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
    static constexpr uint64_t kQueueCapacity = 16;

    struct Slot final {
        std::atomic<uint64_t> sequence{0};
        std::atomic<uint64_t> sampleFrame{0};
        std::atomic<uint64_t> hostTicks{0};
        std::atomic<uint32_t> hostNanosPerSampleQ8{0};
    };

    Slot queue[kQueueCapacity]{};
    std::atomic<uint64_t> producerCursor{0};
    std::atomic<uint64_t> consumerCursor{0};
    std::atomic<bool> notificationPending{false};

    // Latest accepted real anchor, retained for diagnostics.
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> sampleFrame{0};
    std::atomic<uint64_t> hostTicks{0};
    std::atomic<uint32_t> hostNanosPerSampleQ8{0};

    std::atomic<uint64_t> anchorUpdates{0};
    std::atomic<uint64_t> mirrorPublications{0};
    std::atomic<uint64_t> staleUpdates{0};
    std::atomic<uint64_t> queueOverflows{0};
    std::atomic<uint64_t> notificationDispatches{0};
    std::atomic<uint64_t> notificationCoalesced{0};

    void Reset() noexcept {
        for (auto& slot : queue) {
            slot.sequence.store(0, std::memory_order_relaxed);
            slot.sampleFrame.store(0, std::memory_order_relaxed);
            slot.hostTicks.store(0, std::memory_order_relaxed);
            slot.hostNanosPerSampleQ8.store(0, std::memory_order_relaxed);
        }
        producerCursor.store(0, std::memory_order_relaxed);
        consumerCursor.store(0, std::memory_order_relaxed);
        notificationPending.store(false, std::memory_order_relaxed);

        generation.store(0, std::memory_order_relaxed);
        sampleFrame.store(0, std::memory_order_relaxed);
        hostTicks.store(0, std::memory_order_relaxed);
        hostNanosPerSampleQ8.store(0, std::memory_order_relaxed);

        anchorUpdates.store(0, std::memory_order_relaxed);
        mirrorPublications.store(0, std::memory_order_relaxed);
        staleUpdates.store(0, std::memory_order_relaxed);
        queueOverflows.store(0, std::memory_order_relaxed);
        notificationDispatches.store(0, std::memory_order_relaxed);
        notificationCoalesced.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] HostClockAnchorPublishResult Publish(
        uint64_t nextSampleFrame,
        uint64_t nextHostTicks,
        uint32_t nextHostNanosPerSampleQ8,
        uint64_t periodFrames) noexcept {
        if (nextHostTicks == 0 || nextHostNanosPerSampleQ8 == 0 ||
            periodFrames == 0 || (nextSampleFrame % periodFrames) != 0) {
            staleUpdates.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const uint64_t previousGeneration =
            generation.load(std::memory_order_acquire);
        const uint64_t previousSample =
            sampleFrame.load(std::memory_order_relaxed);
        const uint64_t previousHost =
            hostTicks.load(std::memory_order_relaxed);
        if (previousGeneration != 0 &&
            (nextSampleFrame != previousSample + periodFrames ||
             nextHostTicks <= previousHost)) {
            staleUpdates.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const uint64_t write =
            producerCursor.load(std::memory_order_relaxed);
        const uint64_t read =
            consumerCursor.load(std::memory_order_acquire);
        if (write - read >= kQueueCapacity) {
            queueOverflows.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        auto& slot = queue[write % kQueueCapacity];
        slot.sampleFrame.store(nextSampleFrame, std::memory_order_relaxed);
        slot.hostTicks.store(nextHostTicks, std::memory_order_relaxed);
        slot.hostNanosPerSampleQ8.store(
            nextHostNanosPerSampleQ8, std::memory_order_relaxed);
        slot.sequence.store(write + 1, std::memory_order_release);

        sampleFrame.store(nextSampleFrame, std::memory_order_relaxed);
        hostTicks.store(nextHostTicks, std::memory_order_relaxed);
        hostNanosPerSampleQ8.store(
            nextHostNanosPerSampleQ8, std::memory_order_relaxed);
        anchorUpdates.fetch_add(1, std::memory_order_relaxed);
        generation.store(write + 1, std::memory_order_release);
        producerCursor.store(write + 1, std::memory_order_release);

        const bool notify =
            !notificationPending.exchange(true, std::memory_order_acq_rel);
        if (notify) {
            notificationDispatches.fetch_add(1, std::memory_order_relaxed);
        } else {
            notificationCoalesced.fetch_add(1, std::memory_order_relaxed);
        }
        return {
            .accepted = true,
            .notifyConsumer = notify,
            .notificationGeneration = write + 1,
        };
    }

    [[nodiscard]] bool TryPop(HostClockAnchorSample& out) noexcept {
        const uint64_t read =
            consumerCursor.load(std::memory_order_relaxed);
        const uint64_t write =
            producerCursor.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }

        const auto& slot = queue[read % kQueueCapacity];
        if (slot.sequence.load(std::memory_order_acquire) != read + 1) {
            return false;
        }

        out.sampleFrame =
            slot.sampleFrame.load(std::memory_order_relaxed);
        out.hostTicks =
            slot.hostTicks.load(std::memory_order_relaxed);
        out.hostNanosPerSampleQ8 =
            slot.hostNanosPerSampleQ8.load(std::memory_order_relaxed);
        consumerCursor.store(read + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool FinishDrainAndNeedsAnotherPass() noexcept {
        notificationPending.store(false, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        const bool hasQueuedAnchors =
            consumerCursor.load(std::memory_order_acquire) !=
            producerCursor.load(std::memory_order_acquire);
        if (!hasQueuedAnchors) {
            return false;
        }

        return !notificationPending.exchange(
            true, std::memory_order_acq_rel);
    }
};

static_assert((HostClockAnchorState::kQueueCapacity &
               (HostClockAnchorState::kQueueCapacity - 1)) == 0,
              "Host anchor queue capacity must be a power of two");
static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace ASFW::Audio::Runtime
