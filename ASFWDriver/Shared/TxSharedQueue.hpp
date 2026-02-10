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

namespace ASFW::Shared {

// Magic number: 'ASFW'
static constexpr uint32_t kTxQueueMagic = 0x41534657u;
static constexpr uint16_t kTxQueueVersion = 1;

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

// Shared memory header layout
// This structure is at the beginning of the shared IOBufferMemoryDescriptor
struct TxQueueHeader {
    uint32_t magic;             // 'ASFW' for validation
    uint16_t version;           // Protocol version
    uint16_t channels;          // Number of audio channels (1..16)
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
};
static_assert((sizeof(TxQueueHeader) % 8) == 0, "Header alignment sanity");

// SPSC ring buffer for cross-process audio streaming
// Both producer and consumer attach to the same shared memory region
class TxSharedQueueSPSC {
public:
    TxSharedQueueSPSC() = default;

    // Calculate required memory size for given capacity
    static uint64_t RequiredBytes(uint32_t capacityFrames, uint32_t numChannels = 2) {
        const uint64_t headerBytes = AlignUp(sizeof(TxQueueHeader), 64);
        const uint64_t dataBytes = uint64_t(capacityFrames) * uint64_t(numChannels) * sizeof(int32_t);
        return headerBytes + dataBytes;
    }

    // Creator-side initialization (run once by ASFWAudioNub that owns the memory)
    static bool InitializeInPlace(void* base, uint64_t bytes, uint32_t capacityFrames,
                                  uint32_t numChannels = 2) {
        if (!base) return false;
        if (!IsPowerOfTwo(capacityFrames)) return false;
        if (numChannels == 0 || numChannels > 16) return false;

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

        // Publish header initialization
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    // Attach to existing shared memory (both producer and consumer call this)
    bool Attach(void* base, uint64_t bytes) {
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
        if (hdr->channels == 0 || hdr->channels > 16) return false;
        if (!IsPowerOfTwo(hdr->capacityFrames)) return false;

        const uint64_t need = RequiredBytes(hdr->capacityFrames, hdr->channels);
        if (bytes < need) return false;

        uint8_t* base8 = reinterpret_cast<uint8_t*>(base);
        data_ = reinterpret_cast<int32_t*>(base8 + hdr->dataOffsetBytes);

        hdr_ = hdr;
        capacity_ = hdr->capacityFrames;
        mask_ = capacity_ - 1;
        seenControlEpoch_ = hdr_->controlEpoch.v.load(std::memory_order_acquire);
        return true;
    }

    bool IsValid() const { return hdr_ && data_ && capacity_; }

    uint32_t CapacityFrames() const { return capacity_; }

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

        std::memcpy(&data_[idx * ch], interleavedData, size_t(first) * ch * sizeof(int32_t));
        if (second) {
            std::memcpy(&data_[0], &interleavedData[first * ch], size_t(second) * ch * sizeof(int32_t));
        }

        // Publish data then bump index
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
        if (n == 0) return 0;

        const uint32_t idx = r & mask_;
        const uint32_t first = std::min(n, capacity_ - idx);
        const uint32_t second = n - first;

        std::memcpy(outInterleavedData, &data_[idx * ch], size_t(first) * ch * sizeof(int32_t));
        if (second) {
            std::memcpy(&outInterleavedData[first * ch], &data_[0], size_t(second) * ch * sizeof(int32_t));
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

        std::memcpy(outInterleavedData, &data_[idx * ch], size_t(first) * ch * sizeof(int32_t));
        if (second) {
            std::memcpy(&outInterleavedData[first * ch], &data_[0], size_t(second) * ch * sizeof(int32_t));
        }

        return n;
    }

    // Reset indices (ONLY when both producer and consumer are quiesced)
    void Reset() {
        if (!IsValid()) return;
        hdr_->writeIndexFrames.v.store(0, std::memory_order_release);
        hdr_->readIndexFrames.v.store(0, std::memory_order_release);
    }

    // Cycle-time clock correlation: write (controller side) / read (audio driver side)
    void SetCorrHostNanosPerSampleQ8(uint32_t q8) {
        if (hdr_) hdr_->corrHostNanosPerSampleQ8.v.store(q8, std::memory_order_release);
    }
    uint32_t CorrHostNanosPerSampleQ8() const {
        return hdr_ ? hdr_->corrHostNanosPerSampleQ8.v.load(std::memory_order_acquire) : 0;
    }

private:
    TxQueueHeader* hdr_{nullptr};
    int32_t* data_{nullptr};
    uint32_t capacity_{0};
    uint32_t mask_{0};
    uint32_t seenControlEpoch_{0};
};

} // namespace ASFW::Shared
