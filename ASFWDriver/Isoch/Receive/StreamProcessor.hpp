//
// StreamProcessor.hpp
// ASFWDriver
//
// Validates and processes received isochronous packets
//

#pragma once

#include <span>
#include <cstdint>
#include <atomic>
#include "../Core/CIPHeader.hpp"
#include "../Audio/AM824Decoder.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/TxSharedQueue.hpp"

#ifndef ASFW_MAX_SUPPORTED_CHANNELS
#define ASFW_MAX_SUPPORTED_CHANNELS 16
#endif

namespace ASFW::Isoch {

class StreamProcessor {
public:
    StreamProcessor() = default;

    static constexpr size_t kIsochHeaderSize = 8;  // Timestamp + isoch header

    /// Process a single packet payload.
    /// @param payload Raw payload bytes (INCLUDING isoch header prefix when isochHeader=1).
    /// @param length Length in bytes.
    ///
    /// With isochHeader=1 in packet-per-buffer mode, the buffer layout is:
    ///   [0-3]  Timestamp quadlet (upper 16 bits INVALID in PPB mode)
    ///   [4-7]  Isochronous header (dataLength | tag | chan | tcode | sy)
    ///   [8+]   CIP Header + AM824 payload
    void ProcessPacket(const uint8_t* payload, size_t length) {
        // Need at least isoch header (8) + CIP header (8)
        if (length < kIsochHeaderSize + 8) {
            // ASFW_LOG(Isoch, "Short packet: %zu bytes", length);
            errorCount_++;
            return;
        }

        // Skip isoch header prefix to get to CIP header
        const uint8_t* cipStart = payload + kIsochHeaderSize;
        size_t cipLength = length - kIsochHeaderSize;

        // Read CIP Header (first 2 quadlets after isoch header)
        const uint32_t* quadlets = reinterpret_cast<const uint32_t*>(cipStart);
        
        auto header = CIPHeader::Decode(quadlets[0], quadlets[1]);
        if (!header) {
            // ASFW_LOG(Isoch, "Invalid CIP header");
            errorCount_++;
            return;
        }

        packetCount_++;
        
        // Continuity Check
        uint8_t expectedDBC = (lastDBC_ + lastDataBlockCount_) & 0xFF;
        // Skip check for first packet
        if (packetCount_ > 1) {
            if (header->dataBlockCounter != expectedDBC) {
                 // Discontinuity!
                 // ASFW_LOG(Isoch, "DBC Jump: Expected 0x%02X, Got 0x%02X", expectedDBC, header->dataBlockCounter);
                 discontinuityCount_++;
            }
        }
        
        lastDBC_ = header->dataBlockCounter;
        lastSYT_ = header->syt;
        
        // Cache last CIP for periodic logging
        lastCIP_DBS_ = header->dataBlockSize;
        lastCIP_FDF_ = header->fdf;
        lastCIP_SID_ = header->sourceNodeId;
        
        // Payload Calculation (cipLength = total minus isoch header, payloadBytes = minus CIP header)
        size_t payloadBytes = cipLength - 8;  // Subtract 8 bytes for CIP header
        size_t dbsBytes = header->dataBlockSize * 4;
        
        if (dbsBytes == 0) {
            // Should not happen for AM824
             errorCount_++;
             return;
        }
        
        size_t eventCount = payloadBytes / dbsBytes;
        lastDataBlockCount_ = static_cast<uint8_t>(eventCount);
        
        if (payloadBytes % dbsBytes != 0) {
             // Alignment error
             errorCount_++;
        }

        if (eventCount > 0) {
             samplePacketCount_++;
             
             // Update Min/Max Stats
             if (eventCount < minEvents_) minEvents_ = eventCount;
             if (eventCount > maxEvents_) maxEvents_ = eventCount;
             
             // Extract Samples (Just verify decodability for now)
             const uint32_t* dataPtr = &quadlets[2];
             
             // Iterate events
             for (size_t i = 0; i < eventCount; ++i) {
                 // Iterate DBS (channels/quadlets per event)
                 for (size_t ch = 0; ch < header->dataBlockSize; ++ch) {
                     uint32_t sampleQuad = dataPtr[(i * header->dataBlockSize) + ch];
                     
                     // Decode AM824 sample
                     auto sample = AM824Decoder::DecodeSample(sampleQuad);
                     if (sample) {
                         // Store in temp buffer for this event
                         eventSamples_[ch] = *sample;
                     } else if (AM824Decoder::IsMIDI(sampleQuad)) {
                         // MIDI: ignore for now, could route elsewhere
                         eventSamples_[ch] = 0;
                     } else {
                         // Unknown or Empty
                         eventSamples_[ch] = 0;
                     }
                 }
                 
                 // Write this event (1 frame of all channels) to shared RX queue
                 if (sharedRxQueue_) {
                     sharedRxQueue_->Write(eventSamples_, 1);
                 }
             }
             
        } else {
             emptyPacketCount_++;
        }
    }

    /// Record a packet without parsing CIP/AM824 (stabilization/debug mode).
    void RecordRawPacket(size_t length) {
        packetCount_++;
        if (length <= kIsochHeaderSize + 8) {
            emptyPacketCount_++;
        } else {
            samplePacketCount_++;
        }
    }
    
    void LogStatistics() {
         // Single-line compact stats
         ASFW_LOG(Isoch, "RxStats: Pkts=%llu Data=%llu Empty=%llu Errs=%llu Drops=%llu | CIP: SID=%u DBS=%u FDF=0x%02X SYT=0x%04X DBC=0x%02X",
             packetCount_.load(),
             samplePacketCount_.load(),
             emptyPacketCount_.load(),
             errorCount_.load(),
             discontinuityCount_.load(),
             lastCIP_SID_.load(),
             lastCIP_DBS_.load(),
             lastCIP_FDF_.load(),
             lastSYT_.load(),
             lastDBC_.load()
         );
    }
    
    // Accessors for metrics export
    uint64_t PacketCount() const { return packetCount_.load(std::memory_order_relaxed); }
    uint64_t SamplePacketCount() const { return samplePacketCount_.load(std::memory_order_relaxed); }
    uint64_t EmptyPacketCount() const { return emptyPacketCount_.load(std::memory_order_relaxed); }
    uint64_t ErrorCount() const { return errorCount_.load(std::memory_order_relaxed); }
    uint64_t DiscontinuityCount() const { return discontinuityCount_.load(std::memory_order_relaxed); }
    
    uint8_t LastDBC() const { return lastDBC_.load(std::memory_order_relaxed); }
    uint16_t LastSYT() const { return lastSYT_.load(std::memory_order_relaxed); }
    uint8_t LastCipSID() const { return lastCIP_SID_.load(std::memory_order_relaxed); }
    uint8_t LastCipDBS() const { return lastCIP_DBS_.load(std::memory_order_relaxed); }
    uint8_t LastCipFDF() const { return lastCIP_FDF_.load(std::memory_order_relaxed); }
    
    // Latency histogram accessors
    uint64_t LatencyBucket0() const { return latencyBucket0_.load(std::memory_order_relaxed); }
    uint64_t LatencyBucket1() const { return latencyBucket1_.load(std::memory_order_relaxed); }
    uint64_t LatencyBucket2() const { return latencyBucket2_.load(std::memory_order_relaxed); }
    uint64_t LatencyBucket3() const { return latencyBucket3_.load(std::memory_order_relaxed); }
    uint32_t LastPollLatencyUs() const { return lastPollLatencyUs_.load(std::memory_order_relaxed); }
    uint32_t LastPollPackets() const { return lastPollPackets_.load(std::memory_order_relaxed); }
    
    // Record poll cycle latency (called at end of Poll())
    void RecordPollLatency(uint64_t microseconds, uint32_t packetsProcessed) {
        lastPollLatencyUs_.store(static_cast<uint32_t>(microseconds), std::memory_order_relaxed);
        lastPollPackets_.store(packetsProcessed, std::memory_order_relaxed);
        
        // Bucket: [0]: <100µs, [1]: 100-500µs, [2]: 500-1000µs, [3]: >1000µs
        if (microseconds < 100) {
            latencyBucket0_.fetch_add(1, std::memory_order_relaxed);
        } else if (microseconds < 500) {
            latencyBucket1_.fetch_add(1, std::memory_order_relaxed);
        } else if (microseconds < 1000) {
            latencyBucket2_.fetch_add(1, std::memory_order_relaxed);
        } else {
            latencyBucket3_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    void Reset() {
        packetCount_ = 0;
        samplePacketCount_ = 0;
        emptyPacketCount_ = 0;
        errorCount_ = 0;
        discontinuityCount_ = 0;
        lastDBC_ = 0;
        lastSYT_ = 0xFFFF;
        lastDataBlockCount_ = 0; // Or assumes 0
        minEvents_ = UINT64_MAX;
        maxEvents_ = 0;
        latencyBucket0_ = 0;
        latencyBucket1_ = 0;
        latencyBucket2_ = 0;
        latencyBucket3_ = 0;
        lastPollLatencyUs_ = 0;
        lastPollPackets_ = 0;
    }

private:
    std::atomic<uint64_t> packetCount_{0};
    std::atomic<uint64_t> samplePacketCount_{0};
    std::atomic<uint64_t> emptyPacketCount_{0};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> discontinuityCount_{0};
    
    // Non-atomic tracking for single-thread context (ProcessPacket is serialized per channel usually)
    // But safely atomic just case
    std::atomic<uint8_t>  lastDBC_{0};
    std::atomic<uint16_t> lastSYT_{0xFFFF};
    std::atomic<uint8_t>  lastDataBlockCount_{0};
    
    // Cached CIP fields for periodic logging
    std::atomic<uint8_t>  lastCIP_DBS_{0};
    std::atomic<uint8_t>  lastCIP_FDF_{0};
    std::atomic<uint8_t>  lastCIP_SID_{0};
    
    // Latency histogram buckets
    std::atomic<uint64_t> latencyBucket0_{0}; // <100µs
    std::atomic<uint64_t> latencyBucket1_{0}; // 100-500µs
    std::atomic<uint64_t> latencyBucket2_{0}; // 500-1000µs
    std::atomic<uint64_t> latencyBucket3_{0}; // >1000µs
    std::atomic<uint32_t> lastPollLatencyUs_{0};
    std::atomic<uint32_t> lastPollPackets_{0};
    
    uint64_t minEvents_{UINT64_MAX};
    uint64_t maxEvents_{0};
    
    // Output shared queue for decoded RX samples (set by IsochReceiveContext)
    ASFW::Shared::TxSharedQueueSPSC* sharedRxQueue_{nullptr};
    
    // Temp buffer for one event's worth of samples (all channels)
    int32_t eventSamples_[ASFW_MAX_SUPPORTED_CHANNELS]{};
    
public:
    /// Set the output shared queue for decoded samples.
    /// @param queue Pointer to shared RX queue (owned externally, typically by IsochReceiveContext)
    void SetOutputSharedQueue(ASFW::Shared::TxSharedQueueSPSC* queue) noexcept {
        sharedRxQueue_ = queue;
    }

    /// Get current output shared queue (for diagnostics).
    ASFW::Shared::TxSharedQueueSPSC* GetOutputSharedQueue() const noexcept {
        return sharedRxQueue_;
    }
};

} // namespace ASFW::Isoch
