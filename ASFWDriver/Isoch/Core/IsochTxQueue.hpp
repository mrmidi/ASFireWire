// IsochTxQueue.hpp
// ASFW - Payload-opaque shared queue contract for OHCI isochronous transmit.
//
// This is the only shared ABI between an IT packet producer and the OHCI
// transport consumer.  It deliberately contains no content-format, audio-clock,
// or producer-policy concepts: the immediate header and payload bytes are
// opaque to transport.  Bump kTxQueueAbiVersion for every layout change.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ASFW::Isoch {

inline constexpr uint32_t kTxQueueAbiVersion = 5;

/// Producer fills the plain fields, then release-stores commitGeneration.
/// Consumer acquire-loads it and accepts only ExpectedTxCommitGeneration().
struct alignas(64) IsochTxPacketMeta final {
    uint32_t immediateHeader[2];  ///< Opaque OUTPUT_MORE_IMMEDIATE quadlets.
    uint32_t payloadLength;       ///< Opaque payload byte count.
    uint32_t reserved0;
    uint64_t packetIndex;         ///< Absolute packet index.
    std::atomic<uint64_t> commitGeneration{0};
    uint8_t reserved1[64 - 32];
};

static_assert(sizeof(IsochTxPacketMeta) == 64);
static_assert(alignof(IsochTxPacketMeta) == 64);
static_assert(offsetof(IsochTxPacketMeta, immediateHeader) == 0);
static_assert(offsetof(IsochTxPacketMeta, payloadLength) == 8);
static_assert(offsetof(IsochTxPacketMeta, packetIndex) == 16);
static_assert(offsetof(IsochTxPacketMeta, commitGeneration) == 24);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

[[nodiscard]] constexpr uint32_t TxQueueSlotIndexFor(
    uint64_t packetIndex, uint32_t numSlots) noexcept {
    return static_cast<uint32_t>(packetIndex % numSlots);
}

[[nodiscard]] constexpr uint64_t ExpectedTxCommitGeneration(
    uint64_t packetIndex, uint32_t numSlots) noexcept {
    return packetIndex / numSlots + 1;
}

/// Raw host/cycle anchor sampled by the IT consumer.  Interpretation belongs
/// to the packet producer's content/timing domain.
struct IsochTxClockPairSample final {
    uint64_t hostTimeMid{0};
    uint32_t cycleTimer32{0};
};

struct IsochTxClockPairSeqlock final {
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint64_t> hostTimeMid{0};
    std::atomic<uint32_t> cycleTimer32{0};

    void Publish(const IsochTxClockPairSample& sample) noexcept {
        const uint32_t sequenceBefore = sequence.load(std::memory_order_relaxed);
        sequence.store(sequenceBefore + 1, std::memory_order_release);
        hostTimeMid.store(sample.hostTimeMid, std::memory_order_relaxed);
        cycleTimer32.store(sample.cycleTimer32, std::memory_order_relaxed);
        sequence.store(sequenceBefore + 2, std::memory_order_release);
    }

    [[nodiscard]] bool TryRead(IsochTxClockPairSample& out,
                               uint32_t maxAttempts = 4) const noexcept {
        for (uint32_t attempt = 0; attempt < maxAttempts; ++attempt) {
            const uint32_t before = sequence.load(std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            const IsochTxClockPairSample sample{
                .hostTimeMid = hostTimeMid.load(std::memory_order_relaxed),
                .cycleTimer32 = cycleTimer32.load(std::memory_order_relaxed),
            };
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence.load(std::memory_order_relaxed) == before) {
                out = sample;
                return true;
            }
        }
        return false;
    }
};

inline constexpr uint32_t kIsochTxCompletionStampSlots = 32;

struct IsochTxCompletionStamp final {
    std::atomic<uint64_t> packetIndex{0};
    std::atomic<uint32_t> cycleTimestamp{0};
    uint32_t reserved{0};
};
static_assert(sizeof(IsochTxCompletionStamp) == 16);

enum class IsochTxQueueStatus : uint32_t {
    kStopped = 0,
    kRunning = 1,
    kProducerFault = 2,
    kDeadContext = 3,
};

/// Fixed, neutral third shared descriptor of the three-descriptor TX RPC.
/// Geometry is initialized by transport. Producer-owned and consumer-owned
/// progress are reset independently to prevent one side from erasing the
/// other's prefill or completion state.
struct IsochTxQueueControl final {
    uint32_t abiVersion{0};
    uint32_t numSlots{0};
    uint32_t slotStrideBytes{0};
    uint32_t maxPacketBytes{0};
    uint32_t interruptInterval{0};
    uint32_t reserved0{0};

    // Consumer → producer: raw execution progress/anchors.
    std::atomic<uint32_t> streamGeneration{0};
    std::atomic<IsochTxQueueStatus> statusWord{IsochTxQueueStatus::kStopped};
    IsochTxClockPairSeqlock clockPair{};
    std::atomic<uint32_t> startCycleMatch{0};
    std::atomic<uint64_t> startFirstPacketIndex{0};
    std::atomic<uint64_t> completionCursor{0};
    std::atomic<uint64_t> completionStampCount{0};
    IsochTxCompletionStamp completionStamps[kIsochTxCompletionStampSlots]{};
    std::atomic<uint64_t> refillRequestGeneration{0};
    std::atomic<uint64_t> refillHandledGeneration{0};
    std::atomic<uint64_t> refillRequestHostTicks{0};
    std::atomic<uint64_t> refillRequestCount{0};
    std::atomic<uint64_t> refillCoalescedCount{0};
    std::atomic<uint32_t> maxCompletionDelta{0};
    std::atomic<uint64_t> maxCompletionDeltaEvents{0};

    // Producer → consumer: end-exclusive committed packet cursor.
    std::atomic<uint64_t> committedEnd{0};

    /// Runs in the producer before prefill. It never resets consumer state.
    void ResetProducerForStart() noexcept {
        committedEnd.store(0, std::memory_order_release);
    }

    /// Runs in the transport immediately before arm. It never resets prefill.
    void ResetConsumerForArm() noexcept {
        streamGeneration.store(0, std::memory_order_relaxed);
        statusWord.store(IsochTxQueueStatus::kStopped, std::memory_order_relaxed);
        startCycleMatch.store(0, std::memory_order_relaxed);
        startFirstPacketIndex.store(0, std::memory_order_relaxed);
        completionCursor.store(0, std::memory_order_relaxed);
        completionStampCount.store(0, std::memory_order_relaxed);
        refillRequestGeneration.store(0, std::memory_order_relaxed);
        refillHandledGeneration.store(0, std::memory_order_relaxed);
        refillRequestHostTicks.store(0, std::memory_order_relaxed);
        refillRequestCount.store(0, std::memory_order_relaxed);
        refillCoalescedCount.store(0, std::memory_order_relaxed);
        maxCompletionDelta.store(0, std::memory_order_relaxed);
        maxCompletionDeltaEvents.store(0, std::memory_order_relaxed);
    }

    void MarkRefillHandled(uint64_t generation) noexcept {
        uint64_t handled = refillHandledGeneration.load(std::memory_order_relaxed);
        while (handled < generation &&
               !refillHandledGeneration.compare_exchange_weak(
                   handled, generation, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    void PushCompletionStamp(uint64_t packetIndex, uint32_t cycleTimestamp) noexcept {
        const uint64_t count = completionStampCount.load(std::memory_order_relaxed);
        auto& slot = completionStamps[count % kIsochTxCompletionStampSlots];
        slot.packetIndex.store(packetIndex, std::memory_order_relaxed);
        slot.cycleTimestamp.store(cycleTimestamp, std::memory_order_relaxed);
        completionStampCount.store(count + 1, std::memory_order_release);
    }

    [[nodiscard]] bool ReadCompletionStamp(uint64_t stampIndex,
                                           uint64_t& outPacketIndex,
                                           uint32_t& outCycleTimestamp) const noexcept {
        const auto& slot = completionStamps[stampIndex % kIsochTxCompletionStampSlots];
        outPacketIndex = slot.packetIndex.load(std::memory_order_relaxed);
        outCycleTimestamp = slot.cycleTimestamp.load(std::memory_order_relaxed);
        const uint64_t countAfter = completionStampCount.load(std::memory_order_acquire);
        return stampIndex < countAfter &&
               countAfter - stampIndex <= kIsochTxCompletionStampSlots;
    }
};

static_assert(std::atomic<IsochTxQueueStatus>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace ASFW::Isoch
