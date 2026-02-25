// AudioRingBuffer.hpp
// ASFW - Phase 1.5 Encoding Layer
//
// Lock-free Single-Producer Single-Consumer (SPSC) ring buffer for audio.
// Producer: IOOperationHandler (CoreAudio callback)
// Consumer: Encoding pipeline (simulated at 8kHz cycle rate)
//
// Reference: docs/Isoch/PHASE_1_5_ENCODING.md
// Based on: ANSWERS.md decompiled RingBuffer analysis
//

#pragma once

#include <cstdint>
#include <atomic>
#include <cstring>

namespace ASFW {
namespace Encoding {

/// Default ring buffer size in frames (~85ms @ 48kHz)
constexpr uint32_t kDefaultRingBufferFrames = 4096;

/// Lock-free SPSC ring buffer for audio samples.
///
/// Thread-safety model:
///   - Single producer (CoreAudio callback) calls write()
///   - Single consumer (encoding timer) calls read()
///   - No locks required for SPSC pattern
///
/// Storage format:
///   - Interleaved: [ch0][ch1]...[chN][ch0][ch1]...
///   - Each sample is int32_t (24-bit audio in 32-bit container)
///
/// Channel count is runtime (1..kMaxSupportedChannels).
/// FrameCount is compile-time (power of 2 for efficient modulo).
///
template <uint32_t FrameCount = kDefaultRingBufferFrames>
class AudioRingBuffer {
public:
    static_assert((FrameCount & (FrameCount - 1)) == 0,
                  "FrameCount must be power of 2 for efficient modulo");

    static constexpr uint32_t kMaxSupportedChannels = 16;

    /// Max buffer size in samples (compile-time, uses max channel count)
    static constexpr uint32_t kMaxTotalSamples = FrameCount * kMaxSupportedChannels;

    /// Mask for efficient modulo (works because FrameCount is power of 2)
    static constexpr uint32_t kFrameMask = FrameCount - 1;

    /// Construct with runtime channel count.
    /// @param channels Number of audio channels (1..16, default 2)
    explicit AudioRingBuffer(uint32_t channels = 2) noexcept
        : channelCount_(channels) {
        std::memset(buffer_, 0, sizeof(buffer_));
    }

    /// Get channel count.
    uint32_t channelCount() const noexcept { return channelCount_; }

    /// Change channel count and reset buffer.
    void reconfigure(uint32_t channels) noexcept {
        channelCount_ = channels;
        reset();
    }

    /// Write frames to the ring buffer (producer side).
    ///
    /// @param data Interleaved sample data
    /// @param frameCount Number of frames to write
    /// @return Number of frames actually written (may be less if buffer full)
    ///
    uint32_t write(const int32_t* data, uint32_t frameCount) noexcept {
        uint32_t writeIdx = writeIndex_.load(std::memory_order_relaxed);
        uint32_t readIdx = readIndex_.load(std::memory_order_acquire);

        // Calculate available space
        uint32_t available = availableForWrite(writeIdx, readIdx);
        uint32_t toWrite = (frameCount < available) ? frameCount : available;

        if (toWrite == 0) {
            overflowCount_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        // Write samples
        for (uint32_t i = 0; i < toWrite; ++i) {
            uint32_t frameIdx = (writeIdx + i) & kFrameMask;
            uint32_t sampleIdx = frameIdx * channelCount_;

            for (uint32_t ch = 0; ch < channelCount_; ++ch) {
                buffer_[sampleIdx + ch] = data[i * channelCount_ + ch];
            }
        }

        // Update write index (release ensures data is visible before index update)
        writeIndex_.store((writeIdx + toWrite) & kFrameMask, std::memory_order_release);

        return toWrite;
    }

    /// Read frames from the ring buffer (consumer side).
    ///
    /// @param data Output buffer for interleaved samples
    /// @param frameCount Number of frames to read
    /// @return Number of frames actually read (may be less if buffer empty)
    ///
    uint32_t read(int32_t* data, uint32_t frameCount) noexcept {
        // Zero-frame read is not an underrun
        if (frameCount == 0) {
            return 0;
        }

        uint32_t readIdx = readIndex_.load(std::memory_order_relaxed);
        uint32_t writeIdx = writeIndex_.load(std::memory_order_acquire);

        // Calculate available data
        uint32_t available = availableForRead(writeIdx, readIdx);
        uint32_t toRead = (frameCount < available) ? frameCount : available;

        if (toRead == 0) {
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
            // Fill output with silence
            std::memset(data, 0, frameCount * channelCount_ * sizeof(int32_t));
            return 0;
        }

        // Read samples
        for (uint32_t i = 0; i < toRead; ++i) {
            uint32_t frameIdx = (readIdx + i) & kFrameMask;
            uint32_t sampleIdx = frameIdx * channelCount_;

            for (uint32_t ch = 0; ch < channelCount_; ++ch) {
                data[i * channelCount_ + ch] = buffer_[sampleIdx + ch];
            }
        }

        // Partial underrun: zero-fill remainder to prevent garbage on wire
        // Per IT_BUGS.md: available < requested leaves remainder uninitialized
        if (toRead < frameCount) {
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
            const uint32_t remainStart = toRead * channelCount_;
            const uint32_t remainCount = (frameCount - toRead) * channelCount_;
            std::memset(&data[remainStart], 0, remainCount * sizeof(int32_t));
        }

        // Update read index
        readIndex_.store((readIdx + toRead) & kFrameMask, std::memory_order_release);

        return toRead;
    }

    /// Get current fill level in frames.
    uint32_t fillLevel() const noexcept {
        uint32_t writeIdx = writeIndex_.load(std::memory_order_acquire);
        uint32_t readIdx = readIndex_.load(std::memory_order_acquire);
        return availableForRead(writeIdx, readIdx);
    }

    /// Get available space in frames.
    uint32_t availableSpace() const noexcept {
        uint32_t writeIdx = writeIndex_.load(std::memory_order_acquire);
        uint32_t readIdx = readIndex_.load(std::memory_order_acquire);
        return availableForWrite(writeIdx, readIdx);
    }

    /// Check if buffer is empty.
    bool isEmpty() const noexcept {
        return fillLevel() == 0;
    }

    /// Check if buffer is full.
    bool isFull() const noexcept {
        return availableSpace() == 0;
    }

    /// Reset the buffer to empty state.
    void reset() noexcept {
        writeIndex_.store(0, std::memory_order_relaxed);
        readIndex_.store(0, std::memory_order_relaxed);
        underrunCount_.store(0, std::memory_order_relaxed);
        overflowCount_.store(0, std::memory_order_relaxed);
        std::memset(buffer_, 0, sizeof(buffer_));
    }

    /// Get underrun count (reads when buffer was empty).
    uint64_t underrunCount() const noexcept {
        return underrunCount_.load(std::memory_order_relaxed);
    }

    /// Get overflow count (writes when buffer was full).
    uint64_t overflowCount() const noexcept {
        return overflowCount_.load(std::memory_order_relaxed);
    }

    /// Get buffer capacity in frames.
    static constexpr uint32_t capacity() noexcept {
        return FrameCount - 1;  // One slot reserved to distinguish full from empty
    }

private:
    /// Calculate frames available for reading.
    static uint32_t availableForRead(uint32_t writeIdx, uint32_t readIdx) noexcept {
        if (writeIdx >= readIdx) {
            return writeIdx - readIdx;
        } else {
            return FrameCount - readIdx + writeIdx;
        }
    }

    /// Calculate frames available for writing (leave one slot empty).
    static uint32_t availableForWrite(uint32_t writeIdx, uint32_t readIdx) noexcept {
        uint32_t used = availableForRead(writeIdx, readIdx);
        return (FrameCount - 1) - used;  // -1 to leave one slot empty
    }

    uint32_t channelCount_;  ///< Runtime channel count

    alignas(64) int32_t buffer_[kMaxTotalSamples];  ///< Sample storage (cache-aligned, max-sized)

    alignas(64) std::atomic<uint32_t> writeIndex_{0};  ///< Producer write position
    alignas(64) std::atomic<uint32_t> readIndex_{0};   ///< Consumer read position

    std::atomic<uint64_t> underrunCount_{0};  ///< Reads when empty
    std::atomic<uint64_t> overflowCount_{0};  ///< Writes when full
};

/// Convenience alias for standard 4096-frame buffer (backward compat)
using StereoAudioRingBuffer = AudioRingBuffer<4096>;

} // namespace Encoding
} // namespace ASFW
