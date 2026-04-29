// TxSharedQueue.hpp
// ASFW - Cross-process shared memory SPSC queue for audio transmission
//
// This queue is designed for DriverKit cross-process communication:
// - ASFWAudioNub (ASFWDriver process) allocates IOBufferMemoryDescriptor
// - Both ASFWAudioDriver and IsochTransmitContext map the same memory
// - Lock-free SPSC with cache-line padded indices
//
// Producer: ASFWAudioDriver (CoreAudio IO callback)
// Consumer: IsochTransmitContext (IT DMA refill)

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include <algorithm>

#include "../Isoch/Config/AudioConstants.hpp"

namespace ASFW::Shared {

// Magic number: 'ASFW'
static constexpr uint32_t kTxQueueMagic = 0x41534657u;
static constexpr uint16_t kTxQueueVersion = 3;
static constexpr uint32_t kTxQueueDefaultRxProfileId = 1;  // RX Profile B unless audio driver overrides.

static constexpr uint64_t AlignUp(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static constexpr bool IsPowerOfTwo(uint32_t v) {
    return v && ((v & (v - 1)) == 0);
}

// Cache-line aligned atomic for false-sharing prevention
struct alignas(64) CachelineAtomicU32 {
    std::atomic<uint32_t> v;
    uint8_t pad[64 - sizeof(std::atomic<uint32_t>)];
};
static_assert(sizeof(CachelineAtomicU32) == 64, "CachelineAtomicU32 must be 64 bytes");

struct alignas(64) CachelineAtomicU64 {
    std::atomic<uint64_t> v;
    uint8_t pad[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(CachelineAtomicU64) == 64, "CachelineAtomicU64 must be 64 bytes");

struct alignas(64) ConsumerReadDiagnostics {
    std::atomic<uint64_t> underreadEvents;
    std::atomic<uint64_t> underreadFrames;
    std::atomic<uint32_t> lastRequestedFrames;
    std::atomic<uint32_t> lastAvailableFrames;
    std::atomic<uint32_t> lastReadFrames;
    std::atomic<uint32_t> activeRxProfileId;
    uint8_t pad[32];
};
static_assert(sizeof(ConsumerReadDiagnostics) == 64, "ConsumerReadDiagnostics must be 64 bytes");

// Shared memory header layout
// This structure is at the beginning of the shared IOBufferMemoryDescriptor
struct TxQueueHeader {
    uint32_t magic;             // 'ASFW' for validation
    uint16_t version;           // Protocol version
    uint16_t channels;          // Number of audio channels (1..ASFW::Isoch::Config::kMaxPcmChannels)
    uint32_t capacityFrames;    // Power of two
    uint32_t frameStrideBytes;  // channels * sizeof(int32_t)
    uint32_t dataOffsetBytes;   // Offset to sample data from base
    uint32_t reserved0;

    // Producer-owned coordination fields for zero-copy timeline alignment.
    // controlEpoch:
    //   Producer increments to request a consumer-side queue resync/flush.
    // zeroCopyPhaseFrames:
    //   Additive phase so consumer maps queue frame index -> zero-copy buffer frame.
    CachelineAtomicU32 controlEpoch;
    CachelineAtomicU32 zeroCopyPhaseFrames;

    // Cache-line separated indices for lock-free operation
    CachelineAtomicU32 writeIndexFrames; // Producer writes (release), consumer reads (acquire)
    CachelineAtomicU32 readIndexFrames;  // Consumer writes (release), producer reads (acquire)

    // Cycle-time clock correlation: hostNanosPerSample * 256 as uint32_t
    // Written by IR Poll (controller process), read by audio driver process.
    // 0 = not yet computed. Example: 48kHz → 20833.33ns * 256 = 5,333,333
    CachelineAtomicU32 corrHostNanosPerSampleQ8;

    // Transport-owned timing anchor for ZTS publication.
    // transportTimingSeq:
    //   seqlock-style counter; odd = writer active, even = stable snapshot.
    // transportAnchorSampleFrame / transportAnchorHostTicks:
    //   latest transport-derived timing anchor visible across processes.
    CachelineAtomicU32 transportTimingSeq;
    CachelineAtomicU64 transportAnchorSampleFrame;
    CachelineAtomicU64 transportAnchorHostTicks;

    // Startup alignment latched on first host-read readiness.
    // startupAlignmentSeq:
    //   seqlock-style counter; odd = writer active, even = stable snapshot.
    // startupAlignmentSampleTime:
    //   host callback sampleTime at the moment transport startup alignment was latched.
    // input/outputSampleOffset:
    //   transportAnchorSampleFrame - startupAlignmentSampleTime at latch time.
    CachelineAtomicU32 startupAlignmentSeq;
    CachelineAtomicU64 startupAlignmentSampleTime;
    CachelineAtomicU64 startupInputSampleOffset;
    CachelineAtomicU64 startupOutputSampleOffset;

    // Consumer-side RX read health. For the RX queue, ASFWAudioDriver is the consumer
    // that zero-fills when CoreAudio asks for frames not yet decoded from FireWire.
    ConsumerReadDiagnostics consumerReadDiagnostics;
};
static_assert((sizeof(TxQueueHeader) % 8) == 0, "Header alignment sanity");

// SPSC ring buffer for cross-process audio streaming
// Both producer and consumer attach to the same shared memory region
class TxSharedQueueSPSC {
public:
    TxSharedQueueSPSC() = default;

    struct TransportTimingSnapshot {
        bool valid{false};
        uint64_t anchorSampleFrame{0};
        uint64_t anchorHostTicks{0};
        uint32_t hostNanosPerSampleQ8{0};
        uint32_t seq{0};
    };

    struct StartupAlignmentSnapshot {
        bool valid{false};
        uint64_t sampleTime{0};
        uint64_t inputSampleOffset{0};
        uint64_t outputSampleOffset{0};
        uint32_t seq{0};
    };

    // Calculate required memory size for given capacity
    static uint64_t RequiredBytes(uint32_t capacityFrames, uint32_t numChannels = 2) {
        const uint64_t headerBytes = AlignUp(sizeof(TxQueueHeader), 64);
        const uint64_t dataBytes = uint64_t(capacityFrames) * uint64_t(numChannels) * sizeof(int32_t);
        return headerBytes + dataBytes;
    }

    // Creator-side initialization (run once by ASFWAudioNub that owns the memory)
    // Positional initialization arguments mirror the backing shared-memory layout.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static bool InitializeInPlace(uint8_t* base, uint64_t bytes, uint32_t capacityFrames,
                                  uint32_t numChannels = 2) {
        if (!base) return false;
        if (!IsPowerOfTwo(capacityFrames)) return false;
        if (numChannels == 0 || numChannels > ASFW::Isoch::Config::kMaxPcmChannels) return false;

        const uint64_t need = RequiredBytes(capacityFrames, numChannels);
        if (bytes < need) return false;

        // Zero entire region for clean start
        std::memset(base, 0, size_t(bytes));

        auto* hdr = reinterpret_cast<TxQueueHeader*>(base);
        hdr->magic = kTxQueueMagic;
        hdr->version = kTxQueueVersion;
        hdr->channels = static_cast<uint16_t>(numChannels);
        hdr->capacityFrames = capacityFrames;
        hdr->frameStrideBytes = numChannels * uint32_t(sizeof(int32_t));
        hdr->dataOffsetBytes = uint32_t(AlignUp(sizeof(TxQueueHeader), 64));
        hdr->reserved0 = 0;

        hdr->controlEpoch.v.store(0, std::memory_order_relaxed);
        hdr->zeroCopyPhaseFrames.v.store(0, std::memory_order_relaxed);
        hdr->writeIndexFrames.v.store(0, std::memory_order_relaxed);
        hdr->readIndexFrames.v.store(0, std::memory_order_relaxed);
        hdr->corrHostNanosPerSampleQ8.v.store(0, std::memory_order_relaxed);
        hdr->transportTimingSeq.v.store(0, std::memory_order_relaxed);
        hdr->transportAnchorSampleFrame.v.store(0, std::memory_order_relaxed);
        hdr->transportAnchorHostTicks.v.store(0, std::memory_order_relaxed);
        hdr->startupAlignmentSeq.v.store(0, std::memory_order_relaxed);
        hdr->startupAlignmentSampleTime.v.store(0, std::memory_order_relaxed);
        hdr->startupInputSampleOffset.v.store(0, std::memory_order_relaxed);
        hdr->startupOutputSampleOffset.v.store(0, std::memory_order_relaxed);
        hdr->consumerReadDiagnostics.underreadEvents.store(0, std::memory_order_relaxed);
        hdr->consumerReadDiagnostics.underreadFrames.store(0, std::memory_order_relaxed);
        hdr->consumerReadDiagnostics.lastRequestedFrames.store(0, std::memory_order_relaxed);
        hdr->consumerReadDiagnostics.lastAvailableFrames.store(0, std::memory_order_relaxed);
        hdr->consumerReadDiagnostics.lastReadFrames.store(0, std::memory_order_relaxed);
        hdr->consumerReadDiagnostics.activeRxProfileId.store(kTxQueueDefaultRxProfileId,
                                                             std::memory_order_relaxed);

        // Publish header initialization
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    // Attach to existing shared memory (both producer and consumer call this)
    bool Attach(uint8_t* base, uint64_t bytes) {
        hdr_ = nullptr;
        data_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
        seenControlEpoch_ = 0;

        if (!base) return false;

        auto* hdr = reinterpret_cast<TxQueueHeader*>(base);
        // Acquire to observe header written by creator
        std::atomic_thread_fence(std::memory_order_acquire);

        if (hdr->magic != kTxQueueMagic || hdr->version != kTxQueueVersion) return false;
        if (hdr->channels == 0 || hdr->channels > ASFW::Isoch::Config::kMaxPcmChannels) return false;
        if (!IsPowerOfTwo(hdr->capacityFrames)) return false;

        const uint64_t need = RequiredBytes(hdr->capacityFrames, hdr->channels);
        if (bytes < need) return false;

        data_ = reinterpret_cast<int32_t*>(base + hdr->dataOffsetBytes);

        hdr_ = hdr;
        capacity_ = hdr->capacityFrames;
        mask_ = capacity_ - 1;
        seenControlEpoch_ = hdr_->controlEpoch.v.load(std::memory_order_acquire);
        return true;
    }

    bool IsValid() const { return hdr_ && data_ && capacity_; }

    uint32_t CapacityFrames() const { return capacity_; }
    uint32_t Channels() const { return IsValid() ? hdr_->channels : 0; }

    uint32_t WriteIndexFrames() const {
        if (!IsValid()) return 0;
        return hdr_->writeIndexFrames.v.load(std::memory_order_acquire);
    }

    uint32_t ReadIndexFrames() const {
        if (!IsValid()) return 0;
        return hdr_->readIndexFrames.v.load(std::memory_order_acquire);
    }

    // Get current fill level (may be slightly stale due to concurrent access)
    uint32_t FillLevelFrames() const {
        if (!IsValid()) return 0;
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_acquire);
        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_acquire);
        return uint32_t(w - r);
    }

    void RecordConsumerReadResult(uint32_t requestedFrames,
                                  uint32_t availableFrames,
                                  uint32_t readFrames) {
        if (!IsValid() || requestedFrames == 0) return;
        auto& diag = hdr_->consumerReadDiagnostics;
        diag.lastRequestedFrames.store(requestedFrames, std::memory_order_release);
        diag.lastAvailableFrames.store(availableFrames, std::memory_order_release);
        diag.lastReadFrames.store(readFrames, std::memory_order_release);
        if (readFrames < requestedFrames) {
            diag.underreadEvents.fetch_add(1, std::memory_order_acq_rel);
            diag.underreadFrames.fetch_add(requestedFrames - readFrames, std::memory_order_acq_rel);
        }
    }

    void ResetConsumerReadDiagnostics() {
        if (!IsValid()) return;
        auto& diag = hdr_->consumerReadDiagnostics;
        diag.underreadEvents.store(0, std::memory_order_release);
        diag.underreadFrames.store(0, std::memory_order_release);
        diag.lastRequestedFrames.store(0, std::memory_order_release);
        diag.lastAvailableFrames.store(0, std::memory_order_release);
        diag.lastReadFrames.store(0, std::memory_order_release);
    }

    uint64_t ConsumerUnderreadEvents() const {
        return IsValid()
            ? hdr_->consumerReadDiagnostics.underreadEvents.load(std::memory_order_acquire)
            : 0;
    }

    uint64_t ConsumerUnderreadFrames() const {
        return IsValid()
            ? hdr_->consumerReadDiagnostics.underreadFrames.load(std::memory_order_acquire)
            : 0;
    }

    uint32_t LastConsumerReadRequestedFrames() const {
        return IsValid()
            ? hdr_->consumerReadDiagnostics.lastRequestedFrames.load(std::memory_order_acquire)
            : 0;
    }

    uint32_t LastConsumerReadAvailableFrames() const {
        return IsValid()
            ? hdr_->consumerReadDiagnostics.lastAvailableFrames.load(std::memory_order_acquire)
            : 0;
    }

    uint32_t LastConsumerReadFrames() const {
        return IsValid()
            ? hdr_->consumerReadDiagnostics.lastReadFrames.load(std::memory_order_acquire)
            : 0;
    }

    void SetActiveRxProfileId(uint32_t profileId) {
        if (!IsValid()) return;
        hdr_->consumerReadDiagnostics.activeRxProfileId.store(profileId, std::memory_order_release);
    }

    uint32_t ActiveRxProfileId() const {
        return IsValid()
            ? hdr_->consumerReadDiagnostics.activeRxProfileId.load(std::memory_order_acquire)
            : kTxQueueDefaultRxProfileId;
    }

    // Producer: publish queue->buffer phase for zero-copy read mapping.
    void ProducerSetZeroCopyPhaseFrames(uint32_t phaseFrames) {
        if (!IsValid()) return;
        hdr_->zeroCopyPhaseFrames.v.store(phaseFrames, std::memory_order_release);
    }

    // Consumer: fetch queue->buffer phase.
    uint32_t ZeroCopyPhaseFrames() const {
        if (!IsValid()) return 0;
        return hdr_->zeroCopyPhaseFrames.v.load(std::memory_order_acquire);
    }

    // Producer: request consumer-side resync without touching consumer-owned index.
    void ProducerRequestConsumerResync() {
        if (!IsValid()) return;
        hdr_->controlEpoch.v.fetch_add(1, std::memory_order_release);
    }

    // Consumer: apply pending producer resync requests.
    // Returns true if a resync was applied.
    bool ConsumerApplyPendingResync() {
        if (!IsValid()) return false;
        const uint32_t epoch = hdr_->controlEpoch.v.load(std::memory_order_acquire);
        if (epoch == seenControlEpoch_) {
            return false;
        }
        seenControlEpoch_ = epoch;
        ConsumerDropQueuedFrames();
        return true;
    }

    // Consumer-owned safe flush: drop queued frames by advancing read to write.
    void ConsumerDropQueuedFrames() {
        if (!IsValid()) return;
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_acquire);
        hdr_->readIndexFrames.v.store(w, std::memory_order_release);
    }

    // Producer-side start reset. Only use while the consumer is stopped.
    // It discards stale producer data without changing the consumer-owned index.
    uint32_t ProducerDropQueuedFrames() {
        if (!IsValid()) return 0;
        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_acquire);
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_relaxed);
        const uint32_t dropped = uint32_t(w - r);
        hdr_->writeIndexFrames.v.store(r, std::memory_order_release);
        return dropped;
    }

    // Producer-only: publish newly written frames without copying payload.
    // Useful when audio samples live in a separate shared zero-copy buffer.
    uint32_t PublishFrames(uint32_t frames) {
        if (!IsValid() || frames == 0) return 0;

        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_relaxed);
        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_acquire);

        const uint32_t used = uint32_t(w - r);
        const uint32_t free = capacity_ - used;
        const uint32_t n = (frames <= free) ? frames : free;
        if (n == 0) return 0;

        hdr_->writeIndexFrames.v.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer-only: mark frames as consumed without copying payload.
    // Returns frames actually consumed (clamped to available).
    uint32_t ConsumeFrames(uint32_t frames) {
        if (!IsValid() || frames == 0) return 0;

        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_relaxed);
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_acquire);

        const uint32_t avail = uint32_t(w - r);
        const uint32_t n = (frames <= avail) ? frames : avail;
        if (n == 0) return 0;

        hdr_->readIndexFrames.v.store(r + n, std::memory_order_release);
        return n;
    }

    // Producer: write interleaved int32 frames
    // Returns number of frames actually written
    uint32_t Write(const int32_t* interleavedData, uint32_t frames) {
        if (!IsValid() || !interleavedData || frames == 0) return 0;

        const uint32_t ch = hdr_->channels;
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_relaxed);
        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_acquire);

        const uint32_t used = uint32_t(w - r);
        const uint32_t free = capacity_ - used;
        const uint32_t n = (frames <= free) ? frames : free;
        if (n == 0) return 0;

        const uint32_t idx = w & mask_;
        const uint32_t first = std::min(n, capacity_ - idx);
        const uint32_t second = n - first;
        const size_t firstSamples = static_cast<size_t>(first) * static_cast<size_t>(ch);
        const size_t secondSamples = static_cast<size_t>(second) * static_cast<size_t>(ch);

        std::memcpy(&data_[static_cast<size_t>(idx) * ch], interleavedData, firstSamples * sizeof(int32_t));
        if (second) {
            std::memcpy(&data_[0], &interleavedData[firstSamples], secondSamples * sizeof(int32_t));
        }

        // Publish data then bump index
        hdr_->writeIndexFrames.v.store(w + n, std::memory_order_release);
        return n;
    }

    // Producer: append silent interleaved frames.
    // Returns number of frames actually written.
    uint32_t WriteSilence(uint32_t frames) {
        if (!IsValid() || frames == 0) return 0;

        const uint32_t ch = hdr_->channels;
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_relaxed);
        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_acquire);

        const uint32_t used = uint32_t(w - r);
        const uint32_t free = capacity_ - used;
        const uint32_t n = (frames <= free) ? frames : free;
        if (n == 0) return 0;

        const uint32_t idx = w & mask_;
        const uint32_t first = std::min(n, capacity_ - idx);
        const uint32_t second = n - first;
        const size_t firstSamples = static_cast<size_t>(first) * static_cast<size_t>(ch);
        const size_t secondSamples = static_cast<size_t>(second) * static_cast<size_t>(ch);

        std::memset(&data_[static_cast<size_t>(idx) * ch], 0, firstSamples * sizeof(int32_t));
        if (second) {
            std::memset(&data_[0], 0, secondSamples * sizeof(int32_t));
        }

        hdr_->writeIndexFrames.v.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer: read interleaved int32 frames into output buffer
    // Returns number of frames actually read
    uint32_t Read(int32_t* outInterleavedData, uint32_t frames) {
        if (!IsValid() || !outInterleavedData || frames == 0) return 0;

        const uint32_t ch = hdr_->channels;
        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_relaxed);
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_acquire);

        const uint32_t avail = uint32_t(w - r);
        const uint32_t n = (frames <= avail) ? frames : avail;
        RecordConsumerReadResult(frames, avail, n);
        if (n == 0) return 0;

        const uint32_t idx = r & mask_;
        const uint32_t first = std::min(n, capacity_ - idx);
        const uint32_t second = n - first;
        const size_t firstSamples = static_cast<size_t>(first) * static_cast<size_t>(ch);
        const size_t secondSamples = static_cast<size_t>(second) * static_cast<size_t>(ch);

        std::memcpy(outInterleavedData, &data_[static_cast<size_t>(idx) * ch], firstSamples * sizeof(int32_t));
        if (second) {
            std::memcpy(&outInterleavedData[firstSamples], &data_[0], secondSamples * sizeof(int32_t));
        }

        hdr_->readIndexFrames.v.store(r + n, std::memory_order_release);
        return n;
    }

    // Peek at data without advancing read index (for debugging)
    uint32_t Peek(int32_t* outInterleavedData, uint32_t frames) const {
        if (!IsValid() || !outInterleavedData || frames == 0) return 0;

        const uint32_t ch = hdr_->channels;
        const uint32_t r = hdr_->readIndexFrames.v.load(std::memory_order_acquire);
        const uint32_t w = hdr_->writeIndexFrames.v.load(std::memory_order_acquire);

        const uint32_t avail = uint32_t(w - r);
        const uint32_t n = (frames <= avail) ? frames : avail;
        if (n == 0) return 0;

        const uint32_t idx = r & mask_;
        const uint32_t first = std::min(n, capacity_ - idx);
        const uint32_t second = n - first;
        const size_t firstSamples = static_cast<size_t>(first) * static_cast<size_t>(ch);
        const size_t secondSamples = static_cast<size_t>(second) * static_cast<size_t>(ch);

        std::memcpy(outInterleavedData, &data_[static_cast<size_t>(idx) * ch], firstSamples * sizeof(int32_t));
        if (second) {
            std::memcpy(&outInterleavedData[firstSamples], &data_[0], secondSamples * sizeof(int32_t));
        }

        return n;
    }

    // Reset indices (ONLY when both producer and consumer are quiesced)
    void Reset() {
        if (!IsValid()) return;
        hdr_->writeIndexFrames.v.store(0, std::memory_order_release);
        hdr_->readIndexFrames.v.store(0, std::memory_order_release);
        ResetConsumerReadDiagnostics();
    }

    // Cycle-time clock correlation: write (controller side) / read (audio driver side)
    void SetCorrHostNanosPerSampleQ8(uint32_t q8) {
        if (hdr_) hdr_->corrHostNanosPerSampleQ8.v.store(q8, std::memory_order_release);
    }
    uint32_t CorrHostNanosPerSampleQ8() const {
        return hdr_ ? hdr_->corrHostNanosPerSampleQ8.v.load(std::memory_order_acquire) : 0;
    }

    void ResetTransportTiming() {
        if (!hdr_) return;
        hdr_->transportTimingSeq.v.store(0, std::memory_order_release);
        hdr_->transportAnchorSampleFrame.v.store(0, std::memory_order_release);
        hdr_->transportAnchorHostTicks.v.store(0, std::memory_order_release);
        hdr_->corrHostNanosPerSampleQ8.v.store(0, std::memory_order_release);
    }

    void ResetStartupAlignment() {
        if (!hdr_) return;
        hdr_->startupAlignmentSeq.v.store(0, std::memory_order_release);
        hdr_->startupAlignmentSampleTime.v.store(0, std::memory_order_release);
        hdr_->startupInputSampleOffset.v.store(0, std::memory_order_release);
        hdr_->startupOutputSampleOffset.v.store(0, std::memory_order_release);
    }

    void SetTransportTiming(uint64_t sampleFrame, uint64_t hostTicks, uint32_t q8) {
        if (!hdr_) return;
        hdr_->transportTimingSeq.v.fetch_add(1, std::memory_order_acq_rel); // odd = writer active
        hdr_->transportAnchorSampleFrame.v.store(sampleFrame, std::memory_order_release);
        hdr_->transportAnchorHostTicks.v.store(hostTicks, std::memory_order_release);
        hdr_->corrHostNanosPerSampleQ8.v.store(q8, std::memory_order_release);
        hdr_->transportTimingSeq.v.fetch_add(1, std::memory_order_acq_rel); // even = stable snapshot
    }

    [[nodiscard]] TransportTimingSnapshot ReadTransportTiming() const {
        if (!hdr_) {
            return {};
        }

        for (;;) {
            const uint32_t seqBefore = hdr_->transportTimingSeq.v.load(std::memory_order_acquire);
            if ((seqBefore & 1U) != 0U) {
                continue;
            }

            TransportTimingSnapshot snapshot{
                .valid = seqBefore != 0U,
                .anchorSampleFrame =
                    hdr_->transportAnchorSampleFrame.v.load(std::memory_order_acquire),
                .anchorHostTicks =
                    hdr_->transportAnchorHostTicks.v.load(std::memory_order_acquire),
                .hostNanosPerSampleQ8 =
                    hdr_->corrHostNanosPerSampleQ8.v.load(std::memory_order_acquire),
                .seq = seqBefore,
            };

            const uint32_t seqAfter = hdr_->transportTimingSeq.v.load(std::memory_order_acquire);
            if (seqBefore == seqAfter) {
                return snapshot;
            }
        }
    }

    void SetStartupAlignment(uint64_t sampleTime, uint64_t inputOffset, uint64_t outputOffset) {
        if (!hdr_) return;
        hdr_->startupAlignmentSeq.v.fetch_add(1, std::memory_order_acq_rel); // odd = writer active
        hdr_->startupAlignmentSampleTime.v.store(sampleTime, std::memory_order_release);
        hdr_->startupInputSampleOffset.v.store(inputOffset, std::memory_order_release);
        hdr_->startupOutputSampleOffset.v.store(outputOffset, std::memory_order_release);
        hdr_->startupAlignmentSeq.v.fetch_add(1, std::memory_order_acq_rel); // even = stable snapshot
    }

    [[nodiscard]] StartupAlignmentSnapshot ReadStartupAlignment() const {
        if (!hdr_) {
            return {};
        }

        for (;;) {
            const uint32_t seqBefore = hdr_->startupAlignmentSeq.v.load(std::memory_order_acquire);
            if ((seqBefore & 1U) != 0U) {
                continue;
            }

            StartupAlignmentSnapshot snapshot{
                .valid = seqBefore != 0U,
                .sampleTime =
                    hdr_->startupAlignmentSampleTime.v.load(std::memory_order_acquire),
                .inputSampleOffset =
                    hdr_->startupInputSampleOffset.v.load(std::memory_order_acquire),
                .outputSampleOffset =
                    hdr_->startupOutputSampleOffset.v.load(std::memory_order_acquire),
                .seq = seqBefore,
            };

            const uint32_t seqAfter = hdr_->startupAlignmentSeq.v.load(std::memory_order_acquire);
            if (seqBefore == seqAfter) {
                return snapshot;
            }
        }
    }

private:
    TxQueueHeader* hdr_{nullptr};
    int32_t* data_{nullptr};
    uint32_t capacity_{0};
    uint32_t mask_{0};
    uint32_t seenControlEpoch_{0};
};

} // namespace ASFW::Shared
