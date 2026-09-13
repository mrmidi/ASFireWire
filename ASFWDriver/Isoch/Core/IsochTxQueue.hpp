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

inline constexpr uint32_t kTxQueueAbiVersion = 13;

enum class SealResult : uint8_t {
    SealedWithoutAlternative = 0,
    SealedDiscardingAlternative = 1,
    AlreadyTerminal = 2,
    GenerationMismatch = 3,
};

enum class SealReason : uint8_t {
    None = 0,
    FinalityFrontier = 1,
    LiveGuardRejection = 2,
    ShapeRejection = 3,
    StartupPolicy = 4,
};

enum class LatePayloadAcquireResult : uint8_t {
    Success = 0,
    RejectedFinalized = 1,
    RejectedNotArmed = 2,
    RejectedStaleIdentity = 3,
    RejectedInvalidConfig = 4,
};

enum class LatePayloadBindResult : uint8_t {
    NotExamined = 0,
    Bound = 1,
    RejectedShapeMismatch = 2,
    RejectedUnavailableHwPos = 3,
    RejectedInsideGuard = 4,
    RejectedArbitrationLost = 5,
};

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
[[nodiscard]] inline bool OfferLateTxPayload(
    IsochTxPacketMeta& meta,
    uint64_t generation,
    uint64_t& outObservedArbitration) noexcept {
    uint64_t expected = MakeTxPayloadArbitration(
        generation, TxPayloadArbitration::kNoAlternative);
    const bool won = meta.payloadArbitration.compare_exchange_strong(
        expected,
        MakeTxPayloadArbitration(generation,
                                 TxPayloadArbitration::kLateImageReady),
        std::memory_order_acq_rel, std::memory_order_acquire);
    outObservedArbitration = expected;
    return won;
}

[[nodiscard]] inline bool OfferLateTxPayload(IsochTxPacketMeta& meta,
                                             uint64_t generation) noexcept {
    uint64_t unused = 0;
    return OfferLateTxPayload(meta, generation, unused);
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
/// could still win. Exposes a rich SealResult distinguishing unprompted seals
/// from discarded alternatives, terminal states, and generation mismatches.
[[nodiscard]] inline SealResult FinalizeTxPayloadOnArmedImage(
    IsochTxPacketMeta& meta,
    uint64_t generation,
    uint64_t& outPreviousArbitration) noexcept {
    const uint64_t sealed = MakeTxPayloadArbitration(
        generation, TxPayloadArbitration::kFinalOnArmedImage);
    uint64_t expected = MakeTxPayloadArbitration(
        generation, TxPayloadArbitration::kNoAlternative);
    if (meta.payloadArbitration.compare_exchange_strong(
            expected, sealed, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        outPreviousArbitration = expected;
        return SealResult::SealedWithoutAlternative;
    }
    outPreviousArbitration = expected;
    const uint64_t currentGen = expected >> kTxPayloadArbitrationPhaseBits;
    if (currentGen != generation) {
        return SealResult::GenerationMismatch;
    }
    const auto currentPhase = static_cast<TxPayloadArbitration>(
        expected & ((1ULL << kTxPayloadArbitrationPhaseBits) - 1));
    if (currentPhase != TxPayloadArbitration::kLateImageReady) {
        return SealResult::AlreadyTerminal;
    }
    if (meta.payloadArbitration.compare_exchange_strong(
            expected, sealed, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        outPreviousArbitration = expected;
        return SealResult::SealedDiscardingAlternative;
    }
    outPreviousArbitration = expected;
    return SealResult::AlreadyTerminal;
}

[[nodiscard]] inline bool FinalizeTxPayloadOnArmedImage(
    IsochTxPacketMeta& meta, uint64_t generation) noexcept {
    uint64_t unused = 0;
    return FinalizeTxPayloadOnArmedImage(meta, generation, unused) ==
           SealResult::SealedDiscardingAlternative;
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
    uint32_t bracketTicks{0};
};

struct IsochTxClockPairSeqlock final {
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint64_t> hostTimeMid{0};
    std::atomic<uint32_t> cycleTimer32{0};
    std::atomic<uint32_t> bracketTicks{0};

    void Publish(const IsochTxClockPairSample& sample) noexcept {
        const uint32_t sequenceBefore = sequence.load(std::memory_order_relaxed);
        sequence.store(sequenceBefore + 1, std::memory_order_release);
        hostTimeMid.store(sample.hostTimeMid, std::memory_order_relaxed);
        cycleTimer32.store(sample.cycleTimer32, std::memory_order_relaxed);
        bracketTicks.store(sample.bracketTicks, std::memory_order_relaxed);
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
                .bracketTicks = bracketTicks.load(std::memory_order_relaxed),
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

/// Decision-diagnostic slots. A record must survive from the instant the
/// producer writes it -- at a packet index up to a full producer lead ahead of
/// the wire -- until the completion for that same packet is consumed. That span
/// is exactly the queue's own slot count: CanAcquireTxProducerSlot refuses a
/// slot until completionCursor has returned ownership, so a packet's queue slot
/// and its diagnostic record become reusable at the same instant.
///
/// Diagnostics are therefore indexed by the packet's QUEUE slot index
/// (packetIndex % numSlots), not by a private modulus. A separate, smaller
/// modulus would recycle evidence while the packet it describes was still in
/// flight, which is the one failure this storage exists to avoid. This bound
/// only has to be >= the largest numSlots any producer configures; the audio
/// geometry header static_asserts its shared slot count against it.
inline constexpr uint32_t kTxDecisionRingSlots = 1536;

/// Which capture a record belongs to, published as one word so a writer can
/// never stamp a record with a generation from one capture and an epoch from
/// another. Generation 0 means "no capture active" and is never a valid stamp.
[[nodiscard]] constexpr uint64_t MakeTxCaptureToken(uint32_t generation,
                                                    uint32_t epoch) noexcept {
    return (static_cast<uint64_t>(generation) << 32) | epoch;
}

[[nodiscard]] constexpr uint32_t TxCaptureTokenGeneration(uint64_t token) noexcept {
    return static_cast<uint32_t>(token >> 32);
}

[[nodiscard]] constexpr uint32_t TxCaptureTokenEpoch(uint64_t token) noexcept {
    return static_cast<uint32_t>(token & 0xFFFFFFFFU);
}

/// Why a decision record could not be joined to a completion. These are
/// genuinely different findings -- "the capture began after this packet was
/// prepared" is evidence about coverage, "a later packet reused the slot" is
/// evidence the retention window is too short, and a torn snapshot is a
/// transient worth retrying. Collapsing them into one absent/present bit is
/// what makes a telemetry run unfalsifiable.
enum class TxDecisionJoinResult : uint8_t {
    Valid = 0,
    /// Slot carries no stamp at all: nothing ever recorded here.
    NeverWritten = 1,
    /// Slot carries a stamp from an earlier capture and the packet index still
    /// matches or precedes ours -- this packet was prepared before capture
    /// began, so no record for it could exist.
    CaptureStartedLater = 2,
    /// Slot carries a stamp from a different capture token.
    TokenMismatch = 3,
    /// Slot has been claimed by a LATER packet: our evidence was overwritten.
    SlotReused = 4,
    /// The writer was mid-update for every read attempt.
    SnapshotCollision = 5,
    /// Slot holds this capture's stamp but for a different packet that is not
    /// later than ours -- a stale record the producer never refreshed.
    PacketMismatch = 6,
};

/// One transport examination of one packet.
///
/// A packet is examined on every refill pass that can still reach it, so a
/// single mutable "last result" answers the wrong question: it reports where
/// transport ended up, never whether a ready image sat unserviced across a
/// pass boundary. Three are kept -- the last look before the producer's offer,
/// the first look after it, and the terminal decision -- because the interval
/// between the first two is the missed-service window this telemetry exists to
/// measure.
struct TxExaminationEvent final {
    std::atomic<uint64_t> passId{0};
    std::atomic<uint64_t> hostTicks{0};
    std::atomic<uint64_t> liveHwPos{0};
    std::atomic<uint32_t> packed{0};  ///< See TxPackExamination.

    void Clear() noexcept {
        passId.store(0, std::memory_order_relaxed);
        hostTicks.store(0, std::memory_order_relaxed);
        liveHwPos.store(0, std::memory_order_relaxed);
        packed.store(0, std::memory_order_relaxed);
    }
};

/// Plain snapshot of a TxExaminationEvent. Readers copy into this; the shared
/// form is never copied, because a struct of atomics has no copy assignment.
struct TxExaminationEventSnapshot final {
    uint64_t passId{0};
    uint64_t hostTicks{0};
    uint64_t liveHwPos{0};
    uint8_t  arbitrationPhase{0};
    uint8_t  bindResult{0};   ///< LatePayloadBindResult
    uint8_t  hwPosValid{0};
    uint8_t  present{0};
};

[[nodiscard]] constexpr uint32_t TxPackExamination(uint8_t arbitrationPhase,
                                                   uint8_t bindResult,
                                                   bool hwPosValid,
                                                   bool present) noexcept {
    return static_cast<uint32_t>(arbitrationPhase) |
           (static_cast<uint32_t>(bindResult) << 8) |
           (static_cast<uint32_t>(hwPosValid ? 1u : 0u) << 16) |
           (static_cast<uint32_t>(present ? 1u : 0u) << 24);
}

inline void TxUnpackExamination(uint32_t packed,
                                TxExaminationEventSnapshot& out) noexcept {
    out.arbitrationPhase = static_cast<uint8_t>(packed & 0xFFu);
    out.bindResult = static_cast<uint8_t>((packed >> 8) & 0xFFu);
    out.hwPosValid = static_cast<uint8_t>((packed >> 16) & 0x1u);
    out.present = static_cast<uint8_t>((packed >> 24) & 0x1u);
}

/// Producer-lane record flags.
inline constexpr uint32_t kTxProducerFlagAcquireRecorded = 1U << 0;
inline constexpr uint32_t kTxProducerFlagEncodeRecorded  = 1U << 1;
inline constexpr uint32_t kTxProducerFlagOfferRecorded   = 1U << 2;
/// The queue slot no longer carried this packet when the producer looked.
/// Recorded as an observation only; it does not change what the producer does.
inline constexpr uint32_t kTxProducerFlagStaleSlotSeen   = 1U << 3;

/// Set in packedSeal alongside the result/reason to mean "a seal was actually
/// recorded". SealResult::SealedWithoutAlternative is 0, so a zero word cannot
/// double as "nothing recorded"; and a terminal event is NOT evidence of a
/// seal, because a successful bind is terminal without ever sealing.
inline constexpr uint32_t kTxSealRecordedBit = 1U << 16;

/// Transport-lane record flags.
inline constexpr uint32_t kTxTransportFlagTerminalRecorded = 1U << 0;
inline constexpr uint32_t kTxTransportFlagClaimRecorded    = 1U << 1;
inline constexpr uint32_t kTxTransportFlagDescriptorWritten= 1U << 2;
/// The seal path was handed a position snapshot rather than a fresh read, so
/// liveHwPos on the terminal event is an upper bound, not an observation.
inline constexpr uint32_t kTxTransportFlagTerminalPosIsSnapshot = 1U << 3;

/// Storage discipline for both lanes below.
///
/// Every field is atomic and accessed relaxed, wrapped in an odd/even sequence.
/// The sequence alone would not be enough: a seqlock over plain C++ objects is
/// a data race however carefully the reader retries, and detecting a torn read
/// after the fact does not make the read defined. Relaxed atomics cost nothing
/// on arm64 (plain ldr/str) and make the protocol actually well-defined.
///
/// Each lane has exactly ONE writer context:
///   producerDecisions[]  -- whichever context the packet producer runs on.
///   transportDecisions[] -- the transport refill context.
/// A seqlock requires a single writer, and this is where that is guaranteed.
struct alignas(64) TxProducerDecisionRecord final {
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> flags{0};
    std::atomic<uint64_t> captureToken{0};
    std::atomic<uint64_t> packetIndex{0};
    std::atomic<uint64_t> slotGeneration{0};
    std::atomic<uint64_t> encodeHostTicks{0};
    std::atomic<uint64_t> offerStartHostTicks{0};
    std::atomic<uint64_t> offerEndHostTicks{0};
    /// [7:0] acquireResult, [15:8] offerResult (0=none,1=won,2=lost),
    /// [23:16] observed arbitration phase.
    std::atomic<uint32_t> packedResults{0};
    std::atomic<uint32_t> reserved0{0};
};

struct TxProducerDecisionSnapshot final {
    uint64_t captureToken{0};
    uint64_t packetIndex{0};
    uint64_t slotGeneration{0};
    uint64_t encodeHostTicks{0};
    uint64_t offerStartHostTicks{0};
    uint64_t offerEndHostTicks{0};
    uint32_t flags{0};
    uint8_t  acquireResult{0};
    uint8_t  offerResult{0};
    uint8_t  observedArbitrationPhase{0};
};

struct alignas(64) TxTransportDecisionRecord final {
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> flags{0};
    std::atomic<uint64_t> captureToken{0};
    std::atomic<uint64_t> packetIndex{0};
    std::atomic<uint64_t> slotGeneration{0};
    /// Last examination strictly before the producer's offer was visible.
    TxExaminationEvent lastBeforeOffer{};
    /// First examination that saw the offer.
    TxExaminationEvent firstAfterOffer{};
    /// The examination that ended the packet, bound or sealed.
    TxExaminationEvent terminal{};
    std::atomic<uint64_t> claimStartHostTicks{0};
    std::atomic<uint64_t> claimEndHostTicks{0};
    std::atomic<uint64_t> descriptorUpdateHostTicks{0};
    std::atomic<uint64_t> sealStartHostTicks{0};
    std::atomic<uint64_t> sealEndHostTicks{0};
    /// [7:0] sealResult, [15:8] sealReason, [16] recorded.
    std::atomic<uint32_t> packedSeal{0};
    std::atomic<uint32_t> examinationCount{0};
    /// The most recent examination that actually concluded a bind attempt.
    /// Kept apart from firstAfterOffer, which is pinned to the first look and
    /// must keep its timestamp, and from terminal, which a non-terminal
    /// failure deliberately does not claim.
    std::atomic<uint64_t> lastAttemptPassId{0};
    std::atomic<uint32_t> lastAttemptPacked{0};
    std::atomic<uint32_t> reserved1{0};
};

struct TxTransportDecisionSnapshot final {
    uint64_t captureToken{0};
    uint64_t packetIndex{0};
    uint64_t slotGeneration{0};
    TxExaminationEventSnapshot lastBeforeOffer{};
    TxExaminationEventSnapshot firstAfterOffer{};
    TxExaminationEventSnapshot terminal{};
    uint64_t claimStartHostTicks{0};
    uint64_t claimEndHostTicks{0};
    uint64_t descriptorUpdateHostTicks{0};
    uint64_t sealStartHostTicks{0};
    uint64_t sealEndHostTicks{0};
    uint32_t flags{0};
    uint32_t examinationCount{0};
    uint8_t  sealResult{0};
    uint8_t  sealReason{0};
    /// Whether a seal was recorded at all. Without this, sealResult 0 and a
    /// never-sealed packet are the same value.
    uint8_t  sealRecorded{0};
    /// Outcome of the most recent concluded bind attempt, which may be a
    /// non-terminal failure that leaves the packet open (an unavailable
    /// hardware position, say). The terminal event cannot carry these: the
    /// packet is deliberately left open, so there is no terminal decision.
    uint8_t  lastAttemptBindResult{0};
    uint64_t lastAttemptPassId{0};
};

// Layout is derived from the fields, then pinned. Each record occupies whole
// cachelines so that the single writer of a lane never shares a line with the
// record a reader is snapshotting -- diagnostics must not add coherence traffic
// to the transport refill path they are measuring.
static_assert(sizeof(TxExaminationEvent) == 32);
static_assert(sizeof(TxProducerDecisionRecord) == 64);
static_assert(alignof(TxProducerDecisionRecord) == 64);
static_assert(sizeof(TxTransportDecisionRecord) == 192);
static_assert(alignof(TxTransportDecisionRecord) == 64);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

[[nodiscard]] constexpr uint32_t PackCompletionMetadata(
    uint8_t selectedImage,
    uint8_t arbitrationPhase,
    uint16_t eventCode) noexcept {
    return (static_cast<uint32_t>(selectedImage) & 0xFFu) |
           ((static_cast<uint32_t>(arbitrationPhase) & 0xFFu) << 8) |
           ((static_cast<uint32_t>(eventCode) & 0xFFFFu) << 16);
}

[[nodiscard]] constexpr uint8_t CompletionSelectedImage(uint32_t metadata) noexcept {
    return static_cast<uint8_t>(metadata & 0xFFu);
}

[[nodiscard]] constexpr uint8_t CompletionArbitrationPhase(uint32_t metadata) noexcept {
    return static_cast<uint8_t>((metadata >> 8) & 0xFFu);
}

[[nodiscard]] constexpr uint16_t CompletionEventCode(uint32_t metadata) noexcept {
    return static_cast<uint16_t>((metadata >> 16) & 0xFFFFu);
}

struct IsochTxCompletionStamp final {
    std::atomic<uint64_t> packetIndex{0};
    std::atomic<uint32_t> cycleTimestamp{0};
    std::atomic<uint32_t> metadata{0};
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
    /// Rebinds abandoned because the controller had reached the packet by the
    /// time the descriptor store was authorised against a fresh position read.
    /// The armed image stands, so this is a missed improvement, not a fault.
    std::atomic<uint64_t> latePayloadRebindMissedDeadlineCount{0};
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
    /// Seqlock over the finality seal above; written by transport only.
    std::atomic<uint64_t> finalitySealSequence{0};
    std::atomic<uint64_t> finalitySealPublished{0};
    std::atomic<uint64_t> finalitySealFrontier{0};
    std::atomic<uint32_t> finalitySealCycleTimer{0};

    // ---- Producer -> transport service doorbell -------------------------
    //
    // A published replacement creates a servicing obligation NOW. Without this
    // the offer waits for the next completion interrupt, and since the producer
    // may offer from hw + kPayloadFinalityLeadPackets while transport can only
    // act at or above hw + guard + one completion group, everything offered in
    // between is accepted and then discarded.
    //
    // Coalesced: a producer that finds a request already pending does not raise
    // another. Transport re-reads the requested generation after servicing, so
    // a batch published while it was working is not lost -- it is serviced by
    // the following pass rather than dropped.
    std::atomic<uint64_t> offerRequestGeneration{0};
    std::atomic<uint64_t> offerHandledGeneration{0};
    std::atomic<uint32_t> offerNotifyPending{0};
    std::atomic<uint64_t> offerServiceCount{0};
    std::atomic<uint64_t> offerNotifyCoalescedCount{0};
    std::atomic<uint64_t> offerNotifyDroppedCount{0};

    /// Producer side, called once after a batch of offers -- never per packet.
    /// Returns true when the caller owns the obligation to notify transport;
    /// false means a notification is already outstanding and will cover this
    /// batch, because transport re-reads the generation before it finishes.
    [[nodiscard]] bool PublishOfferBatch() noexcept {
        offerRequestGeneration.fetch_add(1, std::memory_order_release);
        uint32_t expected = 0;
        if (offerNotifyPending.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
        offerNotifyCoalescedCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Transport side. The generation this service pass is answering for.
    /// Read BEFORE the sweep, so a batch published during it is newer and is
    /// detected by FinishOfferService.
    [[nodiscard]] uint64_t BeginOfferService() const noexcept {
        return offerRequestGeneration.load(std::memory_order_acquire);
    }

    /// Transport side, after the sweep. Publishes what was serviced, drops the
    /// pending flag, then rechecks. Returns true when another batch arrived
    /// while this one was being serviced and a further pass is owed.
    ///
    /// Order matters: clearing the flag before the recheck is what lets a
    /// producer that lost the coalescing race raise a fresh notification. The
    /// reverse order leaves a window in which the producer sees "pending" and
    /// declines, while transport has already decided it is done.
    [[nodiscard]] bool FinishOfferService(uint64_t servicedGeneration) noexcept {
        offerServiceCount.fetch_add(1, std::memory_order_relaxed);
        MarkOfferHandled(servicedGeneration);
        offerNotifyPending.store(0, std::memory_order_release);
        return offerRequestGeneration.load(std::memory_order_acquire) !=
               servicedGeneration;
    }

    void MarkOfferHandled(uint64_t generation) noexcept {
        uint64_t handled = offerHandledGeneration.load(std::memory_order_relaxed);
        while (handled < generation &&
               !offerHandledGeneration.compare_exchange_weak(
                   handled, generation, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    /// The owner of a notification could not deliver it.
    ///
    /// Clears the pending flag WITHOUT marking the generation handled, so the
    /// offers stay outstanding for the next completion interrupt and a later
    /// batch can raise a fresh notification. Marking them handled here would
    /// claim a service pass that never ran; leaving the flag set would make
    /// every later batch coalesce into a notification nobody is delivering,
    /// which is the doorbell failing silently rather than degrading.
    void AbandonOfferNotification() noexcept {
        offerNotifyDroppedCount.fetch_add(1, std::memory_order_relaxed);
        offerNotifyPending.store(0, std::memory_order_release);
    }

    /// True when a producer has published offers transport has not serviced.
    [[nodiscard]] bool HasUnservicedOffers() const noexcept {
        return offerRequestGeneration.load(std::memory_order_acquire) !=
               offerHandledGeneration.load(std::memory_order_acquire);
    }

    /// Teardown. A notification raised before the stream stopped must not make
    /// the next stream service a generation that belonged to the last one.
    void ResetOfferDoorbell() noexcept {
        offerRequestGeneration.store(0, std::memory_order_relaxed);
        offerHandledGeneration.store(0, std::memory_order_relaxed);
        offerNotifyPending.store(0, std::memory_order_relaxed);
        offerServiceCount.store(0, std::memory_order_relaxed);
        offerNotifyCoalescedCount.store(0, std::memory_order_relaxed);
        offerNotifyDroppedCount.store(0, std::memory_order_relaxed);
    }

    // ---- Decision-diagnostic lanes -------------------------------------
    //
    // Capture identity is ONE word. Generation and epoch published as two
    // stores can be observed mixed -- a writer reading the new generation and
    // the old epoch stamps a record that belongs to neither capture. Packing
    // them means a writer loads the token once and every field it then writes
    // is consistent with that one load.
    //
    // The lanes are never bulk-cleared. Clearing shared records requires every
    // writer to be quiescent, which cannot be arranged from either side of this
    // seam without a handshake neither side has. Instead each record carries
    // the token it was written under, so a record left by an earlier capture is
    // simply not this capture's record, and is reported as such rather than
    // mistaken for missing evidence.
    std::atomic<uint64_t> captureToken{0};

    TxProducerDecisionRecord producerDecisions[kTxDecisionRingSlots]{};
    TxTransportDecisionRecord transportDecisions[kTxDecisionRingSlots]{};

    /// Non-zero while a capture is active. Callers gate timestamp acquisition
    /// on this: reading a clock costs the same whether or not anyone wants the
    /// number, so an ungated read makes the instrumented build measurably
    /// different from the shipping one.
    [[nodiscard]] uint64_t ActiveCaptureToken() const noexcept {
        return captureToken.load(std::memory_order_acquire);
    }

    /// Begin a capture. Generation must be non-zero.
    void BeginCapture(uint32_t generation, uint32_t epoch) noexcept {
        captureToken.store(MakeTxCaptureToken(generation, epoch),
                           std::memory_order_release);
    }

    /// End a capture, but only the one named. A stop for a generation that has
    /// already been superseded must not silence the capture that replaced it;
    /// that is how a rearm races a late stop and records nothing at all.
    /// Returns true when this call actually ended a capture.
    bool EndCapture(uint32_t generation) noexcept {
        uint64_t cur = captureToken.load(std::memory_order_acquire);
        while (cur != 0 && TxCaptureTokenGeneration(cur) == generation) {
            if (captureToken.compare_exchange_weak(cur, 0,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /// Unconditional stop, for teardown paths that own the whole queue.
    void EndAnyCapture() noexcept {
        captureToken.store(0, std::memory_order_release);
    }

    [[nodiscard]] uint32_t DecisionSlotFor(uint64_t packetIndex) const noexcept {
        const uint32_t slots = (numSlots != 0 && numSlots <= kTxDecisionRingSlots)
                                   ? numSlots
                                   : kTxDecisionRingSlots;
        return static_cast<uint32_t>(packetIndex % slots);
    }

    // -- Producer lane. Single writer: the audio producer context. ----------

    void RecordProducerAcquire(uint64_t captureTokenValue, uint64_t packetIndex,
                               uint64_t slotGeneration,
                               LatePayloadAcquireResult result,
                               bool staleSlotSeen) noexcept {
        if (captureTokenValue == 0) return;
        auto& slot = producerDecisions[DecisionSlotFor(packetIndex)];
        const uint32_t seq = slot.sequence.load(std::memory_order_relaxed) + 1;
        slot.sequence.store(seq, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.captureToken.store(captureTokenValue, std::memory_order_relaxed);
        slot.packetIndex.store(packetIndex, std::memory_order_relaxed);
        slot.slotGeneration.store(slotGeneration, std::memory_order_relaxed);
        slot.encodeHostTicks.store(0, std::memory_order_relaxed);
        slot.offerStartHostTicks.store(0, std::memory_order_relaxed);
        slot.offerEndHostTicks.store(0, std::memory_order_relaxed);
        slot.packedResults.store(static_cast<uint32_t>(result),
                                 std::memory_order_relaxed);
        slot.flags.store(kTxProducerFlagAcquireRecorded |
                             (staleSlotSeen ? kTxProducerFlagStaleSlotSeen : 0u),
                         std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence.store(seq + 1, std::memory_order_release);
    }

    void RecordProducerEncode(uint64_t captureTokenValue, uint64_t packetIndex,
                              uint64_t hostTicks) noexcept {
        if (captureTokenValue == 0) return;
        auto& slot = producerDecisions[DecisionSlotFor(packetIndex)];
        // Only annotate a record this capture already opened for this packet.
        if (slot.packetIndex.load(std::memory_order_relaxed) != packetIndex ||
            slot.captureToken.load(std::memory_order_relaxed) != captureTokenValue) {
            return;
        }
        const uint32_t seq = slot.sequence.load(std::memory_order_relaxed) + 1;
        slot.sequence.store(seq, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.encodeHostTicks.store(hostTicks, std::memory_order_relaxed);
        slot.flags.fetch_or(kTxProducerFlagEncodeRecorded,
                            std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence.store(seq + 1, std::memory_order_release);
    }

    void RecordProducerOffer(uint64_t captureTokenValue, uint64_t packetIndex,
                             uint64_t slotGeneration, uint64_t startTicks,
                             uint64_t endTicks, bool won,
                             uint8_t observedPhase) noexcept {
        if (captureTokenValue == 0) return;
        auto& slot = producerDecisions[DecisionSlotFor(packetIndex)];
        const bool sameRecord =
            slot.packetIndex.load(std::memory_order_relaxed) == packetIndex &&
            slot.captureToken.load(std::memory_order_relaxed) == captureTokenValue;
        const uint32_t seq = slot.sequence.load(std::memory_order_relaxed) + 1;
        slot.sequence.store(seq, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (!sameRecord) {
            // The offer is the first thing recorded for this packet under this
            // capture; open a clean record rather than inheriting another
            // packet's acquire/encode timestamps.
            slot.captureToken.store(captureTokenValue, std::memory_order_relaxed);
            slot.packetIndex.store(packetIndex, std::memory_order_relaxed);
            slot.slotGeneration.store(slotGeneration, std::memory_order_relaxed);
            slot.encodeHostTicks.store(0, std::memory_order_relaxed);
            slot.flags.store(0, std::memory_order_relaxed);
            slot.packedResults.store(0, std::memory_order_relaxed);
        }
        slot.offerStartHostTicks.store(startTicks, std::memory_order_relaxed);
        slot.offerEndHostTicks.store(endTicks, std::memory_order_relaxed);
        const uint32_t prior = slot.packedResults.load(std::memory_order_relaxed);
        slot.packedResults.store(
            (prior & 0x000000FFu) |
                (static_cast<uint32_t>(won ? 1u : 2u) << 8) |
                (static_cast<uint32_t>(observedPhase) << 16),
            std::memory_order_relaxed);
        slot.flags.fetch_or(kTxProducerFlagOfferRecorded,
                            std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence.store(seq + 1, std::memory_order_release);
    }

    // -- Transport lane. Single writer: the transport refill context. -------

    /// Open (or re-open) the transport record for a packet under this capture.
    /// Returns a reference so the caller can write examinations into it while
    /// holding the sequence odd. Callers must use the Record* helpers below.
    void RecordTransportExamination(uint64_t captureTokenValue,
                                    uint64_t packetIndex,
                                    uint64_t slotGeneration, uint64_t passId,
                                    uint64_t hostTicks, uint8_t arbPhase,
                                    LatePayloadBindResult bindResult,
                                    uint64_t liveHwPos, bool hwPosValid,
                                    bool offerVisible, bool terminal) noexcept {
        if (captureTokenValue == 0) return;
        auto& slot = transportDecisions[DecisionSlotFor(packetIndex)];
        const bool sameRecord =
            slot.packetIndex.load(std::memory_order_relaxed) == packetIndex &&
            slot.captureToken.load(std::memory_order_relaxed) == captureTokenValue;
        const uint32_t seq = slot.sequence.load(std::memory_order_relaxed) + 1;
        slot.sequence.store(seq, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (!sameRecord) {
            slot.captureToken.store(captureTokenValue, std::memory_order_relaxed);
            slot.packetIndex.store(packetIndex, std::memory_order_relaxed);
            slot.slotGeneration.store(slotGeneration, std::memory_order_relaxed);
            slot.flags.store(0, std::memory_order_relaxed);
            slot.packedSeal.store(0, std::memory_order_relaxed);
            slot.examinationCount.store(0, std::memory_order_relaxed);
            slot.claimStartHostTicks.store(0, std::memory_order_relaxed);
            slot.claimEndHostTicks.store(0, std::memory_order_relaxed);
            slot.descriptorUpdateHostTicks.store(0, std::memory_order_relaxed);
            slot.sealStartHostTicks.store(0, std::memory_order_relaxed);
            slot.sealEndHostTicks.store(0, std::memory_order_relaxed);
            // These must be cleared here even though nothing else writes them
            // on this path. They are deliberately left untouched by the
            // NotExamined observations below -- that is what keeps a verdict
            // from being erased by the sweep -- so a packet that only ever
            // receives such observations would otherwise inherit whatever the
            // slot's previous occupant concluded and export it as its own.
            slot.lastAttemptPassId.store(0, std::memory_order_relaxed);
            slot.lastAttemptPacked.store(0, std::memory_order_relaxed);
            slot.lastBeforeOffer.Clear();
            slot.firstAfterOffer.Clear();
            slot.terminal.Clear();
        }
        slot.examinationCount.fetch_add(1, std::memory_order_relaxed);

        const uint32_t packed =
            TxPackExamination(arbPhase, static_cast<uint8_t>(bindResult),
                              hwPosValid, /*present=*/true);
        const uint32_t priorFlags = slot.flags.load(std::memory_order_relaxed);

        // An examination that reached a verdict is retained here regardless of
        // which of the three slots it lands in. Without this, a non-terminal
        // rejection -- an unavailable hardware position, which leaves the
        // packet open by design -- could not be stored anywhere: firstAfterOffer
        // is already taken by the look that spotted the offer and must keep its
        // timestamp, and terminal belongs to a decision that was never taken.
        // The reason service failed simply vanished.
        if (bindResult != LatePayloadBindResult::NotExamined) {
            slot.lastAttemptPassId.store(passId, std::memory_order_relaxed);
            slot.lastAttemptPacked.store(packed, std::memory_order_relaxed);
        }

        if (terminal) {
            // The first terminal decision stands. A later pass can revisit a
            // slot the producer has already re-armed for a different packet,
            // and letting that overwrite the terminal event would erase the
            // decision actually taken about the packet being measured.
            if ((priorFlags & kTxTransportFlagTerminalRecorded) == 0) {
                slot.terminal.passId.store(passId, std::memory_order_relaxed);
                slot.terminal.hostTicks.store(hostTicks, std::memory_order_relaxed);
                slot.terminal.liveHwPos.store(liveHwPos, std::memory_order_relaxed);
                slot.terminal.packed.store(packed, std::memory_order_relaxed);
                slot.flags.fetch_or(kTxTransportFlagTerminalRecorded,
                                    std::memory_order_relaxed);
            }
        } else if (offerVisible) {
            // First look that saw the offer; later ones do not replace it.
            if (slot.firstAfterOffer.packed.load(std::memory_order_relaxed) == 0) {
                slot.firstAfterOffer.passId.store(passId, std::memory_order_relaxed);
                slot.firstAfterOffer.hostTicks.store(hostTicks, std::memory_order_relaxed);
                slot.firstAfterOffer.liveHwPos.store(liveHwPos, std::memory_order_relaxed);
                slot.firstAfterOffer.packed.store(packed, std::memory_order_relaxed);
            }
        } else {
            // No offer yet. Keep the most recent such look: paired with
            // firstAfterOffer it brackets when the offer became visible, and
            // paired with the offer timestamp it is the service gap.
            slot.lastBeforeOffer.passId.store(passId, std::memory_order_relaxed);
            slot.lastBeforeOffer.hostTicks.store(hostTicks, std::memory_order_relaxed);
            slot.lastBeforeOffer.liveHwPos.store(liveHwPos, std::memory_order_relaxed);
            slot.lastBeforeOffer.packed.store(packed, std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence.store(seq + 1, std::memory_order_release);
    }

    void RecordTransportClaim(uint64_t captureTokenValue, uint64_t packetIndex,
                              uint64_t claimStartTicks, uint64_t claimEndTicks,
                              uint64_t descriptorUpdateTicks,
                              bool descriptorWritten) noexcept {
        if (captureTokenValue == 0) return;
        auto& slot = transportDecisions[DecisionSlotFor(packetIndex)];
        if (slot.packetIndex.load(std::memory_order_relaxed) != packetIndex ||
            slot.captureToken.load(std::memory_order_relaxed) != captureTokenValue) {
            return;
        }
        const uint32_t seq = slot.sequence.load(std::memory_order_relaxed) + 1;
        slot.sequence.store(seq, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.claimStartHostTicks.store(claimStartTicks, std::memory_order_relaxed);
        slot.claimEndHostTicks.store(claimEndTicks, std::memory_order_relaxed);
        slot.descriptorUpdateHostTicks.store(descriptorUpdateTicks,
                                             std::memory_order_relaxed);
        slot.flags.fetch_or(kTxTransportFlagClaimRecorded |
                                (descriptorWritten
                                     ? kTxTransportFlagDescriptorWritten
                                     : 0u),
                            std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence.store(seq + 1, std::memory_order_release);
    }

    void RecordTransportSeal(uint64_t captureTokenValue, uint64_t packetIndex,
                             SealResult sealResult, SealReason sealReason,
                             uint64_t sealStartTicks, uint64_t sealEndTicks,
                             bool positionIsSnapshot) noexcept {
        if (captureTokenValue == 0) return;
        auto& slot = transportDecisions[DecisionSlotFor(packetIndex)];
        if (slot.packetIndex.load(std::memory_order_relaxed) != packetIndex ||
            slot.captureToken.load(std::memory_order_relaxed) != captureTokenValue) {
            return;
        }
        const uint32_t seq = slot.sequence.load(std::memory_order_relaxed) + 1;
        slot.sequence.store(seq, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        // packedSeal carries an explicit recorded bit: SealResult 0 is a real
        // outcome (SealedWithoutAlternative), so a zero word cannot double as
        // "nothing recorded yet" the way the first draft assumed.
        if ((slot.packedSeal.load(std::memory_order_relaxed) & kTxSealRecordedBit) == 0) {
            slot.packedSeal.store(static_cast<uint32_t>(sealResult) |
                                      (static_cast<uint32_t>(sealReason) << 8) |
                                      kTxSealRecordedBit,
                                  std::memory_order_relaxed);
            slot.sealStartHostTicks.store(sealStartTicks, std::memory_order_relaxed);
            slot.sealEndHostTicks.store(sealEndTicks, std::memory_order_relaxed);
            if (positionIsSnapshot) {
                slot.flags.fetch_or(kTxTransportFlagTerminalPosIsSnapshot,
                                    std::memory_order_relaxed);
            }
        }
        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence.store(seq + 1, std::memory_order_release);
    }

    // -- Readers. Any context; snapshot into plain PODs. --------------------

    [[nodiscard]] TxDecisionJoinResult ReadProducerDecision(
        uint64_t packetIndex, uint64_t expectedCaptureToken,
        uint64_t expectedSlotGen, TxProducerDecisionSnapshot& out,
        uint32_t maxAttempts = 4) const noexcept {
        const auto& slot = producerDecisions[DecisionSlotFor(packetIndex)];
        for (uint32_t attempt = 0; attempt < maxAttempts; ++attempt) {
            const uint32_t before = slot.sequence.load(std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            TxProducerDecisionSnapshot snap{};
            snap.captureToken = slot.captureToken.load(std::memory_order_relaxed);
            snap.packetIndex = slot.packetIndex.load(std::memory_order_relaxed);
            snap.slotGeneration = slot.slotGeneration.load(std::memory_order_relaxed);
            snap.encodeHostTicks = slot.encodeHostTicks.load(std::memory_order_relaxed);
            snap.offerStartHostTicks = slot.offerStartHostTicks.load(std::memory_order_relaxed);
            snap.offerEndHostTicks = slot.offerEndHostTicks.load(std::memory_order_relaxed);
            snap.flags = slot.flags.load(std::memory_order_relaxed);
            const uint32_t packed = slot.packedResults.load(std::memory_order_relaxed);
            snap.acquireResult = static_cast<uint8_t>(packed & 0xFFu);
            snap.offerResult = static_cast<uint8_t>((packed >> 8) & 0xFFu);
            snap.observedArbitrationPhase = static_cast<uint8_t>((packed >> 16) & 0xFFu);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.sequence.load(std::memory_order_relaxed) != before) continue;
            out = snap;
            return ClassifyDecisionJoin(snap.captureToken, snap.packetIndex,
                                        snap.slotGeneration, packetIndex,
                                        expectedCaptureToken, expectedSlotGen);
        }
        return TxDecisionJoinResult::SnapshotCollision;
    }

    [[nodiscard]] TxDecisionJoinResult ReadTransportDecision(
        uint64_t packetIndex, uint64_t expectedCaptureToken,
        uint64_t expectedSlotGen, TxTransportDecisionSnapshot& out,
        uint32_t maxAttempts = 4) const noexcept {
        const auto& slot = transportDecisions[DecisionSlotFor(packetIndex)];
        for (uint32_t attempt = 0; attempt < maxAttempts; ++attempt) {
            const uint32_t before = slot.sequence.load(std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            TxTransportDecisionSnapshot snap{};
            snap.captureToken = slot.captureToken.load(std::memory_order_relaxed);
            snap.packetIndex = slot.packetIndex.load(std::memory_order_relaxed);
            snap.slotGeneration = slot.slotGeneration.load(std::memory_order_relaxed);
            ReadExamination(slot.lastBeforeOffer, snap.lastBeforeOffer);
            ReadExamination(slot.firstAfterOffer, snap.firstAfterOffer);
            ReadExamination(slot.terminal, snap.terminal);
            snap.claimStartHostTicks = slot.claimStartHostTicks.load(std::memory_order_relaxed);
            snap.claimEndHostTicks = slot.claimEndHostTicks.load(std::memory_order_relaxed);
            snap.descriptorUpdateHostTicks = slot.descriptorUpdateHostTicks.load(std::memory_order_relaxed);
            snap.sealStartHostTicks = slot.sealStartHostTicks.load(std::memory_order_relaxed);
            snap.sealEndHostTicks = slot.sealEndHostTicks.load(std::memory_order_relaxed);
            snap.flags = slot.flags.load(std::memory_order_relaxed);
            snap.examinationCount = slot.examinationCount.load(std::memory_order_relaxed);
            const uint32_t packedSeal = slot.packedSeal.load(std::memory_order_relaxed);
            snap.sealResult = static_cast<uint8_t>(packedSeal & 0xFFu);
            snap.sealReason = static_cast<uint8_t>((packedSeal >> 8) & 0xFFu);
            snap.sealRecorded = (packedSeal & kTxSealRecordedBit) != 0 ? 1u : 0u;
            snap.lastAttemptPassId = slot.lastAttemptPassId.load(std::memory_order_relaxed);
            snap.lastAttemptBindResult = static_cast<uint8_t>(
                (slot.lastAttemptPacked.load(std::memory_order_relaxed) >> 8) & 0xFFu);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.sequence.load(std::memory_order_relaxed) != before) continue;
            out = snap;
            return ClassifyDecisionJoin(snap.captureToken, snap.packetIndex,
                                        snap.slotGeneration, packetIndex,
                                        expectedCaptureToken, expectedSlotGen);
        }
        return TxDecisionJoinResult::SnapshotCollision;
    }

    /// True only when a seal was actually recorded.
    ///
    /// This must NOT be inferred from the presence of a terminal event: a
    /// successful bind is terminal and never seals, so testing the terminal
    /// event marked every bound packet as sealed and computed offer-to-seal
    /// intervals against zeroed timestamps -- reporting large negative
    /// durations for exactly the packets that worked.
    [[nodiscard]] static bool TransportSealRecorded(
        const TxTransportDecisionSnapshot& snap) noexcept {
        return snap.sealRecorded != 0;
    }

    static void ReadExamination(const TxExaminationEvent& src,
                                TxExaminationEventSnapshot& dst) noexcept {
        dst.passId = src.passId.load(std::memory_order_relaxed);
        dst.hostTicks = src.hostTicks.load(std::memory_order_relaxed);
        dst.liveHwPos = src.liveHwPos.load(std::memory_order_relaxed);
        TxUnpackExamination(src.packed.load(std::memory_order_relaxed), dst);
    }

    /// Name precisely why a record does or does not answer for this packet.
    ///
    /// "No record" has several causes that mean opposite things: a capture that
    /// began after the packet was prepared proves nothing about the driver,
    /// while a slot already claimed by a later packet proves the retention
    /// window is too short and the run must be discarded. Reporting both as a
    /// single missing flag is what would let a broken capture read as a clean
    /// one.
    [[nodiscard]] static TxDecisionJoinResult ClassifyDecisionJoin(
        uint64_t recordToken, uint64_t recordPacketIndex,
        uint64_t recordSlotGeneration, uint64_t wantedPacketIndex,
        uint64_t expectedCaptureToken, uint64_t expectedSlotGen) noexcept {
        if (recordToken == 0) {
            return TxDecisionJoinResult::NeverWritten;
        }
        if (recordToken != expectedCaptureToken) {
            // A record from an older capture that still names a packet at or
            // after ours means our packet was prepared before this capture
            // armed, so no record for it was ever going to exist.
            if (TxCaptureTokenGeneration(recordToken) !=
                    TxCaptureTokenGeneration(expectedCaptureToken) &&
                recordPacketIndex >= wantedPacketIndex) {
                return TxDecisionJoinResult::CaptureStartedLater;
            }
            return TxDecisionJoinResult::TokenMismatch;
        }
        if (recordPacketIndex != wantedPacketIndex) {
            return recordPacketIndex > wantedPacketIndex
                       ? TxDecisionJoinResult::SlotReused
                       : TxDecisionJoinResult::PacketMismatch;
        }
        if (expectedSlotGen != 0 && recordSlotGeneration != expectedSlotGen) {
            return TxDecisionJoinResult::SlotReused;
        }
        return TxDecisionJoinResult::Valid;
    }

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
        latePayloadRebindMissedDeadlineCount.store(0, std::memory_order_relaxed);
        minimumLatePayloadRebindDistance.store(
            ~uint32_t{0}, std::memory_order_relaxed);
        completionStampCount.store(0, std::memory_order_relaxed);
        finalitySealSequence.store(0, std::memory_order_relaxed);
        finalitySealPublished.store(0, std::memory_order_relaxed);
        finalitySealFrontier.store(0, std::memory_order_relaxed);
        finalitySealCycleTimer.store(0, std::memory_order_relaxed);
        refillRequestGeneration.store(0, std::memory_order_relaxed);
        refillHandledGeneration.store(0, std::memory_order_relaxed);
        refillRequestHostTicks.store(0, std::memory_order_relaxed);
        refillRequestCount.store(0, std::memory_order_relaxed);
        refillCoalescedCount.store(0, std::memory_order_relaxed);
        maxCompletionDelta.store(0, std::memory_order_relaxed);
        maxCompletionDeltaEvents.store(0, std::memory_order_relaxed);
        ResetOfferDoorbell();
        // Deliberately NOT touching captureToken or the decision lanes here.
        // This runs on the transport arm path, which knows nothing about
        // whether a diagnostic capture is in progress; clearing the token here
        // silently disarmed any capture armed before the stream started. Stale
        // lane records are handled by token comparison at read time instead,
        // which needs no writer quiescence.
    }

    void MarkRefillHandled(uint64_t generation) noexcept {
        uint64_t handled = refillHandledGeneration.load(std::memory_order_relaxed);
        while (handled < generation &&
               !refillHandledGeneration.compare_exchange_weak(
                   handled, generation, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    /// Record when the finality frontier actually advanced.
    ///
    /// Finality is a transport decision, so only transport knows when it was
    /// taken. A consumer that instead dates it from its own wake attributes
    /// every packet sealed since the previous wake to the later instant, which
    /// biases any finality-to-wire measurement short by up to one wake
    /// interval. This is payload-opaque: a frontier and the cycle timer read
    /// during the pass that moved it, nothing about content.
    void PublishFinalitySeal(uint64_t frontier, uint32_t cycleTimer) noexcept {
        const uint64_t seq =
            finalitySealSequence.load(std::memory_order_relaxed) + 1;
        finalitySealSequence.store(seq, std::memory_order_relaxed);
        finalitySealFrontier.store(frontier, std::memory_order_relaxed);
        finalitySealCycleTimer.store(cycleTimer, std::memory_order_relaxed);
        finalitySealPublished.store(seq, std::memory_order_release);
    }

    /// The newest seal, or false when none has been published or one was being
    /// written. Torn reads are dropped rather than blended.
    [[nodiscard]] bool ReadFinalitySeal(uint64_t& outFrontier,
                                        uint32_t& outCycleTimer) const noexcept {
        const uint64_t published =
            finalitySealPublished.load(std::memory_order_acquire);
        if (published == 0) return false;
        outFrontier = finalitySealFrontier.load(std::memory_order_relaxed);
        outCycleTimer = finalitySealCycleTimer.load(std::memory_order_relaxed);
        return finalitySealSequence.load(std::memory_order_acquire) == published;
    }

    void PushCompletionStamp(uint64_t packetIndex, uint32_t cycleTimestamp,
                             uint32_t metadata = 0) noexcept {
        const uint64_t count = completionStampCount.load(std::memory_order_relaxed);
        auto& slot = completionStamps[count % kIsochTxCompletionStampSlots];
        slot.packetIndex.store(packetIndex, std::memory_order_relaxed);
        slot.cycleTimestamp.store(cycleTimestamp, std::memory_order_relaxed);
        slot.metadata.store(metadata, std::memory_order_relaxed);
        completionStampCount.store(count + 1, std::memory_order_release);
    }

    [[nodiscard]] bool ReadCompletionStamp(uint64_t stampIndex,
                                           uint64_t& outPacketIndex,
                                           uint32_t& outCycleTimestamp,
                                           uint32_t& outMetadata) const noexcept {
        const auto& slot = completionStamps[stampIndex % kIsochTxCompletionStampSlots];
        outPacketIndex = slot.packetIndex.load(std::memory_order_relaxed);
        outCycleTimestamp = slot.cycleTimestamp.load(std::memory_order_relaxed);
        outMetadata = slot.metadata.load(std::memory_order_relaxed);
        const uint64_t countAfter = completionStampCount.load(std::memory_order_acquire);
        return stampIndex < countAfter &&
               countAfter - stampIndex <= kIsochTxCompletionStampSlots;
    }

    [[nodiscard]] bool ReadCompletionStamp(uint64_t stampIndex,
                                           uint64_t& outPacketIndex,
                                           uint32_t& outCycleTimestamp) const noexcept {
        uint32_t ignoredMetadata = 0;
        return ReadCompletionStamp(stampIndex, outPacketIndex, outCycleTimestamp, ignoredMetadata);
    }
};

static_assert(std::atomic<IsochTxQueueStatus>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace ASFW::Isoch
