// IsochTxQueue.hpp
// ASFW - Payload-opaque shared queue contract for OHCI isochronous transmit.
//
// This is the only shared ABI between an IT packet producer and the OHCI
// transport consumer. It deliberately contains no content-format or content-clock
// or producer-policy concepts: the immediate header and payload bytes are
// opaque to transport.  Bump kTxQueueAbiVersion for every layout change.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ASFW::Isoch {

inline constexpr uint32_t kTxQueueAbiVersion = 10;

/// Payload images per producer slot.
///
/// A planned packet is armed with a complete, valid image (image 0) so the
/// consumer always has something to transmit for the slot. A producer that
/// obtains better content before the slot is mapped into a descriptor writes it
/// as a second complete image (image 1) and marks it ready; the consumer picks
/// whichever is ready at map time. The consumer is the only writer of the
/// descriptor, so it never observes a partially written image, and a late
/// second image simply loses -- it is never blended with the first.
///
/// This is a neutral producer/consumer facility. It carries no notion of what
/// makes one image better than another.
inline constexpr uint32_t kTxPayloadImagesPerSlot = 2;

[[nodiscard]] constexpr uint64_t TxPayloadImageOffset(
    uint32_t slotIndex, uint32_t imageIndex,
    uint32_t slotStrideBytes) noexcept {
    return (static_cast<uint64_t>(slotIndex) * kTxPayloadImagesPerSlot +
            imageIndex) *
           slotStrideBytes;
}

/// Producer fills the plain fields, then release-stores commitGeneration.
/// Consumer acquire-loads it and accepts only ExpectedTxCommitGeneration().
struct alignas(64) IsochTxPacketMeta final {
    uint32_t immediateHeader[2];  ///< Opaque OUTPUT_MORE_IMMEDIATE quadlets.
    uint32_t payloadLength;       ///< Opaque payload byte count.
    /// Which payload image the consumer bound to the descriptor. Consumer-owned:
    /// written when the slot is mapped, re-read when it completes so the seal is
    /// verified against the image that was actually transmitted.
    uint16_t selectedPayloadImage;
    /// Leading bytes that are identical in every image of this packet. Opaque:
    /// the consumer does not know what they mean, only that they never differ,
    /// so it can address them once and point a single descriptor field at
    /// whichever image supplies the rest. That makes switching images a single
    /// aligned store rather than two, which is the difference between a
    /// deterministic swap and one the hardware can catch half-done.
    /// Zero means "no such prefix"; the consumer then splits wherever it likes.
    uint16_t payloadPrefixBytes;
    uint64_t packetIndex;         ///< Absolute packet index.
    std::atomic<uint64_t> commitGeneration{0};
    /// Hash of the opaque payload the consumer bound, written at map time.
    /// Producer-written commit no longer implies payload finality, so the seal
    /// belongs to whoever froze the bytes.
    uint64_t payloadSeal;
    /// The single arbitration point between "producer offered image 1" and
    /// "consumer chose an image". Both sides only ever move it by CAS, so the
    /// two events are one event and neither side can observe a decision the
    /// other has not yet made. Encodes the slot's commit generation with a
    /// phase in the low bits, so a value left by an earlier lap can never be
    /// mistaken for this packet's state. See TxPayloadArbitration.
    std::atomic<uint64_t> payloadArbitration{0};
    uint8_t reserved1[64 - 48];
};

static_assert(sizeof(IsochTxPacketMeta) == 64);
static_assert(alignof(IsochTxPacketMeta) == 64);
static_assert(offsetof(IsochTxPacketMeta, immediateHeader) == 0);
static_assert(offsetof(IsochTxPacketMeta, payloadLength) == 8);
static_assert(offsetof(IsochTxPacketMeta, packetIndex) == 16);
static_assert(offsetof(IsochTxPacketMeta, commitGeneration) == 24);
static_assert(offsetof(IsochTxPacketMeta, selectedPayloadImage) == 12);
static_assert(offsetof(IsochTxPacketMeta, payloadPrefixBytes) == 14);
static_assert(offsetof(IsochTxPacketMeta, payloadSeal) == 32);
static_assert(offsetof(IsochTxPacketMeta, payloadArbitration) == 40);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

[[nodiscard]] constexpr uint32_t TxQueueSlotIndexFor(
    uint64_t packetIndex, uint32_t numSlots) noexcept {
    return static_cast<uint32_t>(packetIndex % numSlots);
}

[[nodiscard]] constexpr uint64_t ExpectedTxCommitGeneration(
    uint64_t packetIndex, uint32_t numSlots) noexcept {
    return packetIndex / numSlots + 1;
}

/// Phase of a slot's payload choice. The producer may only move a packet from
/// kNoAlternative to kLateImageReady; the consumer may only move it from either
/// of those to a terminal phase. Both transitions are CAS, and the terminal
/// phases are what the packet actually transmitted:
///
///   kLateImageBound      -> image 1 went on the wire
///   kFinalOnArmedImage   -> image 0 went on the wire
///
/// A producer whose CAS fails has lost the race and must account the packet as
/// the armed image, not as content. Comparing a published marker against a
/// frontier the consumer stores separately cannot express this: the consumer
/// can skip a packet and advance the frontier as two distinct steps, and a
/// producer that lands between them reads a frontier that has not yet caught
/// up with the decision already made about its packet.
enum class TxPayloadArbitration : uint64_t {
    kNoAlternative = 0,      ///< Armed. No alternative image offered.
    kLateImageReady = 1,     ///< Producer published a complete image 1.
    kLateImageBound = 2,     ///< Consumer bound image 1 to the descriptor.
    kFinalOnArmedImage = 3,  ///< Consumer froze the packet on image 0.
};

inline constexpr uint64_t kTxPayloadArbitrationPhaseBits = 2;

/// Generation is >= 1 for every armed slot, so an all-zero word is never a
/// valid state and a slot that was never armed cannot win any CAS.
[[nodiscard]] constexpr uint64_t MakeTxPayloadArbitration(
    uint64_t generation, TxPayloadArbitration phase) noexcept {
    return (generation << kTxPayloadArbitrationPhaseBits) |
           static_cast<uint64_t>(phase);
}

/// Producer side. Offers image 1 for a packet the consumer has not yet decided.
/// Returns false when the consumer already sealed this packet -- the caller
/// must then account the armed image, because that is what transmits.
[[nodiscard]] inline bool OfferLateTxPayload(IsochTxPacketMeta& meta,
                                             uint64_t generation) noexcept {
    uint64_t expected = MakeTxPayloadArbitration(
        generation, TxPayloadArbitration::kNoAlternative);
    return meta.payloadArbitration.compare_exchange_strong(
        expected,
        MakeTxPayloadArbitration(generation,
                                 TxPayloadArbitration::kLateImageReady),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

/// Consumer side. Claims a published image 1 for binding. Acquire on success
/// pairs with the producer's release, so image 1's bytes are visible.
[[nodiscard]] inline bool ClaimLateTxPayload(IsochTxPacketMeta& meta,
                                             uint64_t generation) noexcept {
    uint64_t expected = MakeTxPayloadArbitration(
        generation, TxPayloadArbitration::kLateImageReady);
    return meta.payloadArbitration.compare_exchange_strong(
        expected,
        MakeTxPayloadArbitration(generation,
                                 TxPayloadArbitration::kLateImageBound),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

/// Consumer side. Declares the packet's choice final on the armed image. Call
/// it for every packet whose finality frontier the consumer is about to cross
/// and which it did not bind, so no packet is left in a state the producer
/// could still win. Returns true when this discarded an image the producer had
/// already published -- that is the lost publication, counted at its one
/// authoritative site.
[[nodiscard]] inline bool FinalizeTxPayloadOnArmedImage(
    IsochTxPacketMeta& meta, uint64_t generation) noexcept {
    const uint64_t sealed = MakeTxPayloadArbitration(
        generation, TxPayloadArbitration::kFinalOnArmedImage);
    uint64_t expected = MakeTxPayloadArbitration(
        generation, TxPayloadArbitration::kNoAlternative);
    if (meta.payloadArbitration.compare_exchange_strong(
            expected, sealed, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    if (expected != MakeTxPayloadArbitration(
                        generation, TxPayloadArbitration::kLateImageReady)) {
        // Already bound, already sealed, or a slot this generation never armed.
        return false;
    }
    return meta.payloadArbitration.compare_exchange_strong(
        expected, sealed, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

// The producer is append-only. Its first physical lap is prefilled while OHCI
// is quiesced, before the transport resets its consumer cursor; after that it
// may reuse a slot only when completionCursor has returned ownership. Keeping
// this in the neutral queue contract prevents any content producer from
// mutating a committed/DMA-owned payload without making a producer reset transport
// state during startup.
[[nodiscard]] constexpr bool CanAcquireTxProducerSlot(
    uint64_t packetIndex,
    uint64_t committedEnd,
    uint64_t completionCursor,
    uint32_t numSlots) noexcept {
    if (numSlots == 0 || packetIndex != committedEnd) {
        return false;
    }
    if (committedEnd < numSlots) {
        return true;
    }
    return packetIndex >= completionCursor &&
           packetIndex - completionCursor < numSlots;
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
    kTransportProgressStall = 4,
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
    /// End-exclusive packet index the consumer has bound to descriptors. This
    /// is descriptor ownership only; binding no longer makes payload final because
    /// transport can atomically repoint the mutable tail to image 1.
    std::atomic<uint64_t> mappedEnd{0};
    /// End-exclusive packet index whose payload choice is final. A producer may
    /// write image 1 only at or beyond this frontier. Transport advances it
    /// from the live command pointer after refreshing safely distant bound
    /// descriptors, so it is independent of the deep arm horizon.
    std::atomic<uint64_t> finalizedEnd{0};
    std::atomic<uint64_t> latePayloadRebindCount{0};
    std::atomic<uint64_t> latePayloadRebindRejectedCount{0};
    /// Packets whose producer published an alternative image that transport
    /// then sealed on the armed image. This is the authoritative count of
    /// content the producer believed it placed and the wire never carried.
    std::atomic<uint64_t> latePayloadLostPublicationCount{0};
    std::atomic<uint32_t> minimumLatePayloadRebindDistance{~uint32_t{0}};
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
        mappedEnd.store(0, std::memory_order_relaxed);
        finalizedEnd.store(0, std::memory_order_relaxed);
        latePayloadRebindCount.store(0, std::memory_order_relaxed);
        latePayloadRebindRejectedCount.store(0, std::memory_order_relaxed);
        latePayloadLostPublicationCount.store(0, std::memory_order_relaxed);
        minimumLatePayloadRebindDistance.store(
            ~uint32_t{0}, std::memory_order_relaxed);
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
