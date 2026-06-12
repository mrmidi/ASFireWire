// IsochAudioTransport.hpp
// ASFW - Shared isoch TX transport contract (audio dext ⇄ OHCI core ⇄ lab).
//
// One header, three consumers: the audio side (ASFWAudioDriver) produces
// packets into the payload slab and this metadata ring; the core (ASFWDriver)
// refill ISR consumes ring entries by blitting immediateHeader + payloadLength
// into OHCI descriptors; ADKVirtualAudioLab rehearses the same contract with a
// fake consumer. Design: ISOCH_AUDIO_ADK.md §3 (regions), §5 (timing),
// ISOCH_AUDIO_CLEANUP_PREP.md §7 (AudioStreamProfile).
//
// Rules enforced here:
//  - Plain C++ only — no DriverKit, no Foundation; lock-free atomics only.
//  - Every index crossing the boundary is an absolute 64-bit monotonic count.
//    `% numSlots` happens only through the helpers below, so there is exactly
//    one definition of every ring's modulus (the 4096-vs-512 wrap-mismatch
//    class, commits 0d897ecb / 79e45e92, cannot recur between these sides).
//  - Layout is locked by static_asserts; bump kTransportAbiVersion on any
//    change and both sides assert it at allocation time.

#pragma once

#include "AudioTimingGeometry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ASFW::IsochTransport {

inline constexpr uint32_t kTransportAbiVersion = 3;

// =============================================================================
// Shared queue / buffer sizing (ADK §3.3 / §6.4).
// These constants are the "Iron Rule" ground truth for both sides.
// =============================================================================

inline constexpr uint32_t kAudioRingBufferFrames =
    AudioTimingGeometry::kFrameRingFrames;
inline constexpr uint32_t kAudioIoPeriodFrames =
    AudioTimingGeometry::kHalIoPeriodFrames;

/// Target gap (writtenEnd - consumer cursor) the isoch TX consumer maintains.
inline constexpr uint32_t kOutputConsumerLeadFrames = 384;  ///< ~0.75 period (~8ms @48k)

/// Deadband: rebase the consumer cursor only when |lead - target| exceeds this.
inline constexpr uint32_t kOutputCursorResyncDeadbandFrames = 64;  ///< ~0.125 period

// =============================================================================
// Metadata ring (ADK §3.2) — one entry per payload-slab slot.
// =============================================================================

/// Producer (audio pump): fill all plain fields, then release-store commitGen
/// last. Consumer (core refill ISR): acquire-load commitGen first; the entry is
/// trustworthy only if it equals ExpectedCommitGen(packetIndex, numSlots).
/// A mismatch at arm time is a DMA-side underrun — fatal by policy (ADK §6.2).
struct alignas(64) TxPacketMeta final {
    uint32_t immediateHeader[2];      ///< Ready-to-blit OHCI IT header quadlets
                                      ///< (OUTPUT_MORE_IMMEDIATE immediate data).
                                      ///< The core never interprets these.
    uint32_t payloadLength;           ///< Bytes: 8 (NO-DATA) or 8 + frames·dbs·4.
    uint32_t reserved0;
    uint64_t packetIndex;             ///< Absolute index this entry describes.
    std::atomic<uint64_t> commitGen;  ///< Lap-numbered commit marker; 0 = never
                                      ///< written. Release-stored last.
    uint8_t reserved1[64 - 32];
};

static_assert(sizeof(TxPacketMeta) == 64, "TxPacketMeta must be one cache line");
static_assert(alignof(TxPacketMeta) == 64, "TxPacketMeta must be cache-line aligned");
static_assert(offsetof(TxPacketMeta, immediateHeader) == 0);
static_assert(offsetof(TxPacketMeta, payloadLength) == 8);
static_assert(offsetof(TxPacketMeta, packetIndex) == 16);
static_assert(offsetof(TxPacketMeta, commitGen) == 24);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

/// The single definition of the metadata ring's slot mapping and lap number.
[[nodiscard]] constexpr uint32_t SlotIndexFor(uint64_t packetIndex, uint32_t numSlots) noexcept {
    return static_cast<uint32_t>(packetIndex % numSlots);
}

/// commitGen value a correctly committed entry for `packetIndex` must carry.
/// Lap-numbered (not a bare flag) so a stale entry from a previous lap of the
/// ring can never be mistaken for a fresh commit.
[[nodiscard]] constexpr uint64_t ExpectedCommitGen(uint64_t packetIndex, uint32_t numSlots) noexcept {
    return packetIndex / numSlots + 1;
}

// =============================================================================
// Clock pair (ADK §5.1) — bus clock ⇄ host clock affine anchor.
// =============================================================================

struct ClockPairSample final {
    uint64_t hostTimeMid{0};   ///< (hostA + hostB) / 2 around the register read.
    uint32_t cycleTimer32{0};  ///< Raw CYCLE_TIMER register value.
};

/// Single-writer (core) seqlock. Reader retries while a write is in flight.
struct ClockPairSeqlock final {
    std::atomic<uint32_t> sequence{0};  // odd = write in progress
    std::atomic<uint64_t> hostTimeMid{0};
    std::atomic<uint32_t> cycleTimer32{0};

    void Publish(const ClockPairSample& sample) noexcept {
        const uint32_t seq = sequence.load(std::memory_order_relaxed);
        sequence.store(seq + 1, std::memory_order_release);   // → odd
        hostTimeMid.store(sample.hostTimeMid, std::memory_order_relaxed);
        cycleTimer32.store(sample.cycleTimer32, std::memory_order_relaxed);
        sequence.store(seq + 2, std::memory_order_release);   // → even
    }

    /// RT-safe bounded read; returns false if a write raced every attempt.
    [[nodiscard]] bool TryRead(ClockPairSample& out, uint32_t maxAttempts = 4) const noexcept {
        for (uint32_t attempt = 0; attempt < maxAttempts; ++attempt) {
            const uint32_t before = sequence.load(std::memory_order_acquire);
            if (before & 1u) continue;
            ClockPairSample sample{};
            sample.hostTimeMid = hostTimeMid.load(std::memory_order_relaxed);
            sample.cycleTimer32 = cycleTimer32.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence.load(std::memory_order_relaxed) == before) {
                out = sample;
                return true;
            }
        }
        return false;
    }
};

// =============================================================================
// Completion stamps (ADK §3.3 / §5.3) — hardware TX timestamps, core → audio.
// =============================================================================

inline constexpr uint32_t kCompletionStampSlots = 32;

struct CompletionStamp final {
    std::atomic<uint64_t> packetIndex{0};
    std::atomic<uint32_t> cycleTimestamp{0};  ///< xferStatus timestamp field
                                              ///< ([cycleSeconds:3][cycleCount:13]).
    uint32_t reserved{0};
};
static_assert(sizeof(CompletionStamp) == 16);

// =============================================================================
// Stream control block v2 (ADK §3.3) — small, fixed, one writer per field.
// =============================================================================

enum class TxStreamStatus : uint32_t {
    kStopped = 0,
    kRunning = 1,
    kUnderrunFatal = 2,  ///< Refill ISR met an uncommitted slot (ADK §6.2).
    kDeadContext = 3,    ///< OHCI context died / bus reset (ADK §6.3).
};

struct TxStreamControl final {
    // --- geometry: written once by the core at allocation; the audio side
    //     asserts every field against its own expectations at bind time so a
    //     mismatch fails loudly at bring-up, never as a silence mystery. ---
    uint32_t abiVersion{0};        ///< Must equal kTransportAbiVersion.
    uint32_t numSlots{0};          ///< Slab slots == metadata ring entries.
    uint32_t slotStrideBytes{0};   ///< Payload slab stride (cache-line rounded).
    uint32_t maxPacketBytes{0};    ///< CIP header + max PCM payload.
    uint32_t interruptInterval{0}; ///< Packets per refill-ISR batch.
    uint32_t ztsPeriodFrames{0};   ///< HAL ring length == ZTS period (§5.5).

    // --- core → audio ---
    std::atomic<uint32_t> streamGeneration{0}; ///< Bumped on reset/teardown/death.
    std::atomic<TxStreamStatus> statusWord{TxStreamStatus::kStopped};
    ClockPairSeqlock clockPair{};
    std::atomic<uint32_t> startCycleMatch{0};       ///< Raw cycleMatch value armed.
    std::atomic<uint64_t> startFirstPacketIndex{0}; ///< Packet that ships at startCycleMatch.
    std::atomic<uint64_t> completionCursor{0};      ///< Count of retired packets
                                                    ///< (end-exclusive absolute index).
    std::atomic<uint64_t> completionStampCount{0};  ///< Total stamps ever written.
    CompletionStamp completionStamps[kCompletionStampSlots]{};
    std::atomic<uint64_t> preparationRequestGeneration{0};
    std::atomic<uint64_t> preparationHandledGeneration{0};
    std::atomic<uint64_t> preparationRequestHostTicks{0};
    std::atomic<uint64_t> preparationRequestCount{0};
    std::atomic<uint64_t> preparationCoalescedCount{0};

    // --- audio → core ---
    std::atomic<uint64_t> exposeCursor{0};  ///< Count of committed packets
                                            ///< (end-exclusive absolute index).

    // -------------------------------------------------------------------------
    // Completion-stamp ring access (writer = core ISR, reader = audio pump).
    // -------------------------------------------------------------------------
    // `cycleTimestamp` is a full CYCLE_TIMER-format value. OHCI supplies the
    // authoritative seconds/cycle; the core pairs it with the same refill
    // sample's subcycle before publishing.
    void PushCompletionStamp(uint64_t packetIndex, uint32_t cycleTimestamp) noexcept {
        const uint64_t count = completionStampCount.load(std::memory_order_relaxed);
        auto& slot = completionStamps[count % kCompletionStampSlots];
        slot.packetIndex.store(packetIndex, std::memory_order_relaxed);
        slot.cycleTimestamp.store(cycleTimestamp, std::memory_order_relaxed);
        completionStampCount.store(count + 1, std::memory_order_release);
    }

    /// Reads the stamp at absolute index `stampIndex` (< completionStampCount).
    /// Returns false if the slot has been overwritten by a newer lap — the
    /// reader fell more than kCompletionStampSlots behind and must resync.
    [[nodiscard]] bool ReadCompletionStamp(uint64_t stampIndex,
                                           uint64_t& outPacketIndex,
                                           uint32_t& outCycleTimestamp) const noexcept {
        const auto& slot = completionStamps[stampIndex % kCompletionStampSlots];
        outPacketIndex = slot.packetIndex.load(std::memory_order_relaxed);
        outCycleTimestamp = slot.cycleTimestamp.load(std::memory_order_relaxed);
        const uint64_t countAfter = completionStampCount.load(std::memory_order_acquire);
        return stampIndex < countAfter &&
               countAfter - stampIndex <= kCompletionStampSlots;
    }
};

static_assert(std::atomic<TxStreamStatus>::is_always_lock_free);

// =============================================================================
// Device stream profile (ISOCH_AUDIO_CLEANUP_PREP.md §7.2) — resolved by the
// core (discovered > known-device table > family defaults) and delivered to
// the audio dext via the nub. The audio side derives all transport geometry
// from this; the core never interprets the audio-facing fields.
// =============================================================================

enum class PcmWireFormat : uint8_t {
    kAM824Labeled = 0,           ///< Label 0x40 + big-endian 24-bit sample.
    kRaw24In32SignExtended = 1,  ///< Sign-extended 24-in-32, no label (Saffire TX).
};

enum class CipBlockingMode : uint8_t {
    kBlocking = 0,     ///< Fixed frames/packet, NO-DATA fills the cadence.
    kNonBlocking = 1,
};

enum class TxSytMode : uint8_t {
    /// RX-recovered device cadence, disciplined against completed OHCI IT
    /// packet execution anchors (Saffire.kext full-duplex timing model).
    kRxRecoveredDeviceClock = 0,
};

struct StreamDescriptor final {
    PcmWireFormat wireFormat{PcmWireFormat::kAM824Labeled};
    uint8_t dbs{0};        ///< Quadlets per frame on the wire.
    uint8_t pcmSlots{0};   ///< Audio slots; dbs == pcmSlots + midiSlots.
    uint8_t midiSlots{0};
    CipBlockingMode blockingMode{CipBlockingMode::kBlocking};
    uint8_t fdf{0};
    uint8_t isoChannel{0xFF};  ///< 0xFF = not yet allocated.
    uint8_t speed{2};          ///< IEEE 1394 speed code (2 = S400).
};
static_assert(sizeof(StreamDescriptor) == 8);

struct AudioStreamProfile final {
    StreamDescriptor hostToDevice{};
    StreamDescriptor deviceToHost{};
    uint32_t sampleRateHz{0};
    uint8_t framesPerPacket{0};   ///< Rate ladder: 8 / 16 / 32 (ADK §6.6).
    TxSytMode sytMode{TxSytMode::kRxRecoveredDeviceClock};
    uint8_t latencyMode{0};       ///< Index into the delayPackets table.
    uint8_t inputDelayPackets{0};
    uint8_t outputDelayPackets{0};
    uint8_t reserved[1]{};
    uint16_t reportedLatencyBaseFrames{0};
    uint32_t profileGeneration{0};  ///< Bumped with streamGeneration (§7.4).
};
static_assert(sizeof(AudioStreamProfile) == 32, "AudioStreamProfile layout is ABI");
static_assert(offsetof(AudioStreamProfile, sampleRateHz) == 16);
static_assert(offsetof(AudioStreamProfile, profileGeneration) == 28);

/// Wire bytes of a DATA packet for a descriptor at a given frames/packet:
/// CIP header (8) + frames · dbs · 4. This is the only place the formula lives.
[[nodiscard]] constexpr uint32_t DataPacketBytes(const StreamDescriptor& d,
                                                 uint32_t framesPerPacket) noexcept {
    return 8u + framesPerPacket * static_cast<uint32_t>(d.dbs) * 4u;
}

} // namespace ASFW::IsochTransport
