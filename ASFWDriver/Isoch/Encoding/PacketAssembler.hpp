// PacketAssembler.hpp
// ASFW - Phase 1.5 Encoding Layer
//
// Assembles complete AM824/CIP isochronous packets by combining:
//   - BlockingCadence48k / NonBlockingCadence48k (packet type + frame count)
//   - BlockingDbcGenerator (DBC tracking)
//   - AudioRingBuffer (sample source)
//   - AM824Encoder (sample encoding)
//   - CIPHeaderBuilder (header generation)
//
// Reference: docs/Isoch/PHASE_1_5_ENCODING.md
// Verified against: 000-48kORIG.txt FireBug capture
//

#pragma once

#include "AM824Encoder.hpp"
#include "CIPHeaderBuilder.hpp"
#include "BlockingCadence48k.hpp"
#include "NonBlockingCadence48k.hpp"
#include "BlockingDbcGenerator.hpp"
#include "AudioRingBuffer.hpp"
#include "../../Logging/Logging.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ASFW {
namespace Encoding {

enum class StreamMode : uint8_t {
    kNonBlocking = 0,
    kBlocking = 1,
};

/// Maximum supported channel count (compile-time buffer sizing)
constexpr uint32_t kMaxSupportedChannels = 16;

/// Compile-time maximum frames per DATA packet (48k blocking path).
constexpr uint32_t kSamplesPerDataPacket = 8;

/// CIP header size in bytes
constexpr uint32_t kCIPHeaderSize = 8;

/// Compile-time max audio data size (8 frames × 16 channels × 4 bytes)
constexpr uint32_t kMaxAudioDataSize = kSamplesPerDataPacket * kMaxSupportedChannels * sizeof(uint32_t);

/// Compile-time max assembled packet size (CIP header + max audio data)
constexpr uint32_t kMaxAssembledPacketSize = kCIPHeaderSize + kMaxAudioDataSize;

/// Assembled packet structure
struct AssembledPacket {
    uint8_t data[kMaxAssembledPacketSize]; ///< Packet bytes (big-endian wire order)
    uint32_t size;                   ///< Actual size: 8 for NO-DATA, varies for DATA
    bool isData;                     ///< True if DATA packet, false if NO-DATA
    uint8_t dbc;                     ///< DBC value used
    uint64_t cycleNumber;            ///< Cycle this packet is for
};

/// Assembles complete isochronous packets from audio samples.
///
/// Usage:
///   1. Create assembler with SID
///   2. Write audio to the ring buffer (from CoreAudio callback)
///   3. Call assembleNext() for each FireWire cycle (8000/sec)
///   4. Transmit or validate the assembled packet
///
class PacketAssembler {
public:
    /// Construct a packet assembler.
    /// @param channels Number of audio channels (1..kMaxSupportedChannels)
    /// @param sid Source node ID (6 bits)
    explicit PacketAssembler(uint32_t channels = 2, uint8_t sid = 0) noexcept
        : channelCount_(channels)
        , cipBuilder_(sid, static_cast<uint8_t>(channels)) {}

    /// Get channel count.
    uint32_t channelCount() const noexcept { return channelCount_; }

    /// Get runtime data packet size in bytes.
    uint32_t dataPacketSize() const noexcept {
        return kCIPHeaderSize + samplesPerDataPacket() * channelCount_ * sizeof(uint32_t);
    }

    /// Get DATA packet frame count for the active stream mode (48k paths only).
    uint32_t samplesPerDataPacket() const noexcept {
        switch (streamMode_) {
            case StreamMode::kBlocking:
                return kSamplesPerPacket48k;
            case StreamMode::kNonBlocking:
                return kNonBlockingSamplesPerPacket48k;
        }
        return kSamplesPerDataPacket;
    }

    /// Reconfigure channel count and SID (resets all state).
    /// Use this instead of assignment since atomics prevent copy/move.
    void reconfigure(uint32_t channels, uint8_t sid) noexcept {
        channelCount_ = channels;
        cipBuilder_ = CIPHeaderBuilder(sid, static_cast<uint8_t>(channels));
        ringBuffer_.reconfigure(channels);
        blockingCadence_.reset();
        nonBlockingCadence_.reset();
        dbcGen_.reset();
        zeroCopyReadPos_ = 0;
        zeroCopyEnabled_ = false;
        zeroCopyBase_ = nullptr;
        zeroCopyCapacity_ = 0;
        dbgDataPackets_.store(0, std::memory_order_relaxed);
        dbgUnderrunPackets_.store(0, std::memory_order_relaxed);
    }

    /// Set the source node ID.
    void setSID(uint8_t sid) noexcept {
        cipBuilder_.setSID(sid);
    }

    /// Set stream mode for upcoming packetization.
    void setStreamMode(StreamMode mode) noexcept {
        streamMode_ = mode;
    }

    /// Get configured stream mode.
    StreamMode streamMode() const noexcept {
        return streamMode_;
    }
    
    /// Get reference to the audio ring buffer for writing samples.
    AudioRingBuffer<>& ringBuffer() noexcept {
        return ringBuffer_;
    }

    /// Get const reference to the ring buffer.
    const AudioRingBuffer<>& ringBuffer() const noexcept {
        return ringBuffer_;
    }
    
    /// ZERO-COPY: Set direct audio source buffer (bypasses ring buffer)
    /// @param base Pointer to interleaved int32_t samples (channelCount_ channels)
    /// @param frameCapacity Total frames in buffer
    void setZeroCopySource(const int32_t* base, uint32_t frameCapacity) noexcept {
        zeroCopyBase_ = base;
        zeroCopyCapacity_ = frameCapacity;
        zeroCopyReadPos_ = 0;
        zeroCopyEnabled_ = (base != nullptr && frameCapacity > 0);
    }
    
    /// Check if zero-copy mode is enabled
    bool isZeroCopyEnabled() const noexcept { return zeroCopyEnabled_; }
    
    /// Get zero-copy read position (for diagnostics)
    uint32_t zeroCopyReadPosition() const noexcept { return zeroCopyReadPos_; }

    /// Force zero-copy read position (used to synchronize with shared counters).
    void setZeroCopyReadPosition(uint32_t framePos) noexcept {
        if (zeroCopyCapacity_ == 0) return;
        zeroCopyReadPos_ = framePos % zeroCopyCapacity_;
    }
    
    /// Assemble the next packet based on current cadence position.
    ///
    /// @param syt Presentation timestamp (SYT) for DATA packets
    /// @return Assembled packet ready for transmission
    ///
    AssembledPacket assembleNext(uint16_t syt = 0) noexcept {
        AssembledPacket packet{};
        packet.cycleNumber = currentCycleNumber();
        packet.isData = nextIsData();
        const uint8_t samplesInPacket = static_cast<uint8_t>(samplesPerDataPacket());
        packet.dbc = dbcGen_.getDbc(packet.isData, samplesInPacket);
        
        if (packet.isData) {
            assembleDataPacket(packet, syt);
        } else {
            assembleNoDataPacket(packet);
        }
        
        // Advance cadence for next cycle
        advanceCadence();
        
        return packet;
    }
    
    /// Get current fill level of the ring buffer in frames.
    uint32_t bufferFillLevel() const noexcept {
        return ringBuffer_.fillLevel();
    }
    
    /// Get underrun count (cycles where buffer was empty).
    uint64_t underrunCount() const noexcept {
        return ringBuffer_.underrunCount();
    }
    
    /// Get current cycle number.
    uint64_t currentCycle() const noexcept {
        return currentCycleNumber();
    }
    
    /// Check if next packet will be DATA.
    bool nextIsData() const noexcept {
        switch (streamMode_) {
            case StreamMode::kBlocking:
                return blockingCadence_.isDataPacket();
            case StreamMode::kNonBlocking:
                return nonBlockingCadence_.isDataPacket();
        }
        return true;
    }
    
    /// Reset all state to initial conditions.
    void reset() noexcept {
        blockingCadence_.reset();
        nonBlockingCadence_.reset();
        dbcGen_.reset();
        ringBuffer_.reset();
        zeroCopyReadPos_ = 0;  // Reset zero-copy read position
    }
    
    /// Reset with specific initial DBC value.
    void reset(uint8_t initialDbc) noexcept {
        blockingCadence_.reset();
        nonBlockingCadence_.reset();
        dbcGen_.reset(initialDbc);
        ringBuffer_.reset();
        zeroCopyReadPos_ = 0;  // Reset zero-copy read position
    }
    
private:
    /// Assemble a DATA packet (CIP + audio).
    void assembleDataPacket(AssembledPacket& packet, uint16_t syt) noexcept {
        const uint32_t framesPerPacket = samplesPerDataPacket();
        packet.size = dataPacketSize();

        // Build CIP header
        CIPHeader cip = cipBuilder_.build(packet.dbc, syt, false);

        // Copy CIP header (already in wire order)
        std::memcpy(packet.data, &cip.q0, 4);
        std::memcpy(packet.data + 4, &cip.q1, 4);

        // Read audio samples - ZERO-COPY path or ring buffer fallback
        int32_t samples[kSamplesPerDataPacket * kMaxSupportedChannels];
        uint32_t framesRead = 0;

        if (zeroCopyEnabled_ && zeroCopyBase_) {
            // ZERO-COPY: Read directly from CoreAudio buffer
            // Buffer is interleaved, wraps at zeroCopyCapacity_
            for (uint32_t f = 0; f < framesPerPacket; ++f) {
                uint32_t frameIdx = (zeroCopyReadPos_ + f) % zeroCopyCapacity_;
                uint32_t sampleIdx = frameIdx * channelCount_;
                for (uint32_t ch = 0; ch < channelCount_; ++ch) {
                    samples[f * channelCount_ + ch] = zeroCopyBase_[sampleIdx + ch];
                }
            }
            zeroCopyReadPos_ = (zeroCopyReadPos_ + framesPerPacket) % zeroCopyCapacity_;
            framesRead = framesPerPacket;
        } else {
            // Fallback: Read from ring buffer (old path)
            framesRead = ringBuffer_.read(samples, framesPerPacket);
        }

        // Track counters (NO LOGGING IN HOT PATH - can stall for milliseconds)
        dbgDataPackets_.fetch_add(1, std::memory_order_relaxed);
        if (framesRead < framesPerPacket) {
            dbgUnderrunPackets_.fetch_add(1, std::memory_order_relaxed);

            // SAFETY: Zero remaining samples to prevent encoding stale stack data
            size_t samplesRead = framesRead * channelCount_;
            size_t totalSamples = framesPerPacket * channelCount_;
            std::memset(&samples[samplesRead], 0, (totalSamples - samplesRead) * sizeof(int32_t));
        }

        // Encode samples to AM824 format
        uint32_t* audioQuadlets = reinterpret_cast<uint32_t*>(packet.data + kCIPHeaderSize);

        for (uint32_t i = 0; i < framesPerPacket * channelCount_; ++i) {
            audioQuadlets[i] = AM824Encoder::encode(samples[i]);
        }
    }
    
    /// Assemble a NO-DATA packet (8 bytes: CIP only).
    void assembleNoDataPacket(AssembledPacket& packet) noexcept {
        packet.size = kCIPHeaderSize;
        
        // Build CIP header with SYT=0xFFFF
        CIPHeader cip = cipBuilder_.buildNoData(packet.dbc);
        
        // Copy CIP header (already in wire order)
        std::memcpy(packet.data, &cip.q0, 4);
        std::memcpy(packet.data + 4, &cip.q1, 4);
    }
    
    uint32_t channelCount_{2};               ///< Number of audio channels
    uint64_t currentCycleNumber() const noexcept {
        switch (streamMode_) {
            case StreamMode::kBlocking:
                return blockingCadence_.getTotalCycles();
            case StreamMode::kNonBlocking:
                return nonBlockingCadence_.getTotalCycles();
        }
        return 0;
    }

    void advanceCadence() noexcept {
        switch (streamMode_) {
            case StreamMode::kBlocking:
                blockingCadence_.advance();
                break;
            case StreamMode::kNonBlocking:
                nonBlockingCadence_.advance();
                break;
        }
    }

    BlockingCadence48k blockingCadence_;     ///< 48k blocking cadence pattern
    NonBlockingCadence48k nonBlockingCadence_; ///< 48k non-blocking cadence pattern
    BlockingDbcGenerator dbcGen_;            ///< DBC tracker
    CIPHeaderBuilder cipBuilder_;            ///< CIP header builder
    AudioRingBuffer<> ringBuffer_;           ///< Audio sample buffer (fallback)
    
    // ZERO-COPY: Direct audio source (bypasses ring buffer)
    const int32_t* zeroCopyBase_{nullptr};
    uint32_t zeroCopyCapacity_{0};
    mutable uint32_t zeroCopyReadPos_{0}; // mutable for read position tracking
    bool zeroCopyEnabled_{false};
    StreamMode streamMode_{StreamMode::kBlocking};
    
    // Debug counters (for 1Hz logging instead of hot-path logging)
    std::atomic<uint64_t> dbgDataPackets_{0};
    std::atomic<uint64_t> dbgUnderrunPackets_{0};
    
public:
    /// Snapshot debug counters for 1Hz logging (resets counters atomically)
    void snapshotDebug(uint64_t& dataPkts, uint64_t& underruns) noexcept {
        dataPkts = dbgDataPackets_.exchange(0, std::memory_order_relaxed);
        underruns = dbgUnderrunPackets_.exchange(0, std::memory_order_relaxed);
    }
};

} // namespace Encoding
} // namespace ASFW
