// PacketAssembler.hpp
// ASFW - Phase 1.5 Encoding Layer
//
// Assembles complete AM824/CIP isochronous packets by combining:
//   - BlockingCadence48k (packet type decision)
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
#include "BlockingDbcGenerator.hpp"
#include "AudioRingBuffer.hpp"
#include "../../Logging/Logging.hpp"

#include <cstdint>
#include <cstring>

namespace ASFW {
namespace Encoding {

/// Maximum packet size in bytes (CIP header + 8 stereo frames)
constexpr uint32_t kMaxPacketSize = 72;

/// CIP header size in bytes
constexpr uint32_t kCIPHeaderSize = 8;

/// Audio data size in bytes (8 frames × 2 channels × 4 bytes)
constexpr uint32_t kAudioDataSize = 64;

/// Samples per DATA packet
constexpr uint32_t kSamplesPerDataPacket = 8;

/// Channels per frame
constexpr uint32_t kChannelsPerFrame = 2;

/// Assembled packet structure
struct AssembledPacket {
    uint8_t data[kMaxPacketSize];   ///< Packet bytes (big-endian wire order)
    uint32_t size;                   ///< Actual size: 8 for NO-DATA, 72 for DATA
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
    /// @param sid Source node ID (6 bits)
    explicit PacketAssembler(uint8_t sid = 0) noexcept
        : cipBuilder_(sid) {}
    
    /// Set the source node ID.
    void setSID(uint8_t sid) noexcept {
        cipBuilder_.setSID(sid);
    }
    
    /// Get reference to the audio ring buffer for writing samples.
    StereoAudioRingBuffer& ringBuffer() noexcept {
        return ringBuffer_;
    }
    
    /// Get const reference to the ring buffer.
    const StereoAudioRingBuffer& ringBuffer() const noexcept {
        return ringBuffer_;
    }
    
    /// Assemble the next packet based on current cadence position.
    ///
    /// @param syt Presentation timestamp (SYT) for DATA packets
    /// @return Assembled packet ready for transmission
    ///
    AssembledPacket assembleNext(uint16_t syt = 0) noexcept {
        AssembledPacket packet{};
        packet.cycleNumber = cadence_.getTotalCycles();
        packet.isData = cadence_.isDataPacket();
        packet.dbc = dbcGen_.getDbc(packet.isData);
        
        if (packet.isData) {
            assembleDataPacket(packet, syt);
        } else {
            assembleNoDataPacket(packet);
        }
        
        // Advance cadence for next cycle
        cadence_.advance();
        
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
        return cadence_.getTotalCycles();
    }
    
    /// Check if next packet will be DATA.
    bool nextIsData() const noexcept {
        return cadence_.isDataPacket();
    }
    
    /// Reset all state to initial conditions.
    void reset() noexcept {
        cadence_.reset();
        dbcGen_.reset();
        ringBuffer_.reset();
    }
    
    /// Reset with specific initial DBC value.
    void reset(uint8_t initialDbc) noexcept {
        cadence_.reset();
        dbcGen_.reset(initialDbc);
        ringBuffer_.reset();
    }
    
private:
    /// Assemble a DATA packet (72 bytes: CIP + audio).
    void assembleDataPacket(AssembledPacket& packet, uint16_t syt) noexcept {
        packet.size = kMaxPacketSize;
        
        // Build CIP header
        CIPHeader cip = cipBuilder_.build(packet.dbc, syt, false);
        
        // Copy CIP header (already in wire order)
        std::memcpy(packet.data, &cip.q0, 4);
        std::memcpy(packet.data + 4, &cip.q1, 4);
        
        // Read audio samples from ring buffer
        int32_t samples[kSamplesPerDataPacket * kChannelsPerFrame];
        uint32_t framesRead = ringBuffer_.read(samples, kSamplesPerDataPacket);
        
        // DEBUG: Log first samples and encoded values periodically
        static uint64_t dataPacketCount = 0;
        if (dataPacketCount < 5 || (dataPacketCount % 1000) == 0) {
            // Log raw samples and what AM824 produces
            uint32_t encoded0 = AM824Encoder::encode(samples[0]);
            uint32_t encoded1 = AM824Encoder::encode(samples[1]);
            ASFW_LOG(Isoch, "ENC[%llu]: framesRead=%u samples=[%08x,%08x] encoded=[%08x,%08x]",
                     dataPacketCount, framesRead,
                     static_cast<uint32_t>(samples[0]), static_cast<uint32_t>(samples[1]),
                     encoded0, encoded1);
        }
        dataPacketCount++;
        
        // Encode samples to AM824 format
        uint32_t* audioQuadlets = reinterpret_cast<uint32_t*>(packet.data + kCIPHeaderSize);
        
        for (uint32_t i = 0; i < kSamplesPerDataPacket * kChannelsPerFrame; ++i) {
            audioQuadlets[i] = AM824Encoder::encode(samples[i]);
        }
        
        // Track if we had samples or used silence
        (void)framesRead;  // Underrun is tracked by ring buffer
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
    
    BlockingCadence48k cadence_;          ///< Cadence pattern generator
    BlockingDbcGenerator dbcGen_;         ///< DBC tracker
    CIPHeaderBuilder cipBuilder_;         ///< CIP header builder
    StereoAudioRingBuffer ringBuffer_;    ///< Audio sample buffer
};

} // namespace Encoding
} // namespace ASFW
