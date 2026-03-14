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
#include <algorithm>
#include "../Core/CIPHeader.hpp"
#include "../Core/ExternalSyncBridge.hpp"
#include "../Config/AudioConstants.hpp"
#include "../Audio/AM824Decoder.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/TxSharedQueue.hpp"

namespace ASFW::Isoch {

class StreamProcessor {
public:
    StreamProcessor() = default;

    static constexpr size_t kIsochHeaderSize = 8;  // Timestamp + isoch header

    struct RxCipSummary {
        bool hasValidCip{false};
        uint16_t syt{Core::ExternalSyncBridge::kNoInfoSyt};
        uint8_t fdf{0};
        uint8_t dbs{0};
    };

    /// Process a single packet payload.
    /// @param payload Raw payload bytes (INCLUDING isoch header prefix when isochHeader=1).
    /// @param length Length in bytes.
    ///
    /// With isochHeader=1 in packet-per-buffer mode, the buffer layout is:
    ///   [0-3]  Timestamp quadlet (upper 16 bits INVALID in PPB mode)
    ///   [4-7]  Isochronous header (dataLength | tag | chan | tcode | sy)
    ///   [8+]   CIP Header + AM824 payload
    [[nodiscard]] RxCipSummary ProcessPacket(const uint8_t* payload, size_t length) noexcept {
        RxCipSummary summary{};
        if (length < kIsochHeaderSize + 8) {
            errorCount_++;
            return summary;
        }

        const auto header = DecodePacketHeader(payload);
        if (!header) {
            errorCount_++;
            return summary;
        }

        packetCount_++;
        PopulateSummary(*header, summary);
        RecordContinuity(*header);
        RecordCipState(*header);

        const auto layout = BuildPayloadLayout(payload, length, *header);
        if (!layout) {
            return summary;
        }

        LogInterestingDbs(*header, *layout, length);
        ProcessDecodedPayload(*header, *layout);

        return summary;
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
        lastUnsupportedWireDbs_ = 0;
    }

private:
    struct PayloadLayout {
        const uint32_t* dataPtr{nullptr};
        size_t payloadBytes{0};
        size_t eventCount{0};
        size_t wireSlotsPerEvent{0};
        size_t decodeSlotsPerEvent{0};
        uint32_t queueChannels{0};
        bool queueWriteSafe{true};
    };

    [[nodiscard]] std::optional<CIPHeader> DecodePacketHeader(const uint8_t* payload) const noexcept {
        const uint8_t* cipStart = payload + kIsochHeaderSize;
        const auto* quadlets = reinterpret_cast<const uint32_t*>(cipStart);
        return CIPHeader::Decode(quadlets[0], quadlets[1]);
    }

    void PopulateSummary(const CIPHeader& header, RxCipSummary& summary) const noexcept {
        summary.hasValidCip = true;
        summary.syt = header.syt;
        summary.fdf = header.fdf;
        summary.dbs = header.dataBlockSize;
    }

    void RecordContinuity(const CIPHeader& header) noexcept {
        const uint8_t expectedDBC =
            static_cast<uint8_t>((lastDBC_.load(std::memory_order_relaxed) +
                                  lastDataBlockCount_.load(std::memory_order_relaxed)) &
                                 0xFF);
        if (packetCount_.load(std::memory_order_relaxed) > 1 &&
            header.dataBlockCounter != expectedDBC) {
            discontinuityCount_++;
        }
    }

    void RecordCipState(const CIPHeader& header) noexcept {
        lastDBC_ = header.dataBlockCounter;
        lastSYT_ = header.syt;
        lastCIP_DBS_ = header.dataBlockSize;
        lastCIP_FDF_ = header.fdf;
        lastCIP_SID_ = header.sourceNodeId;
    }

    [[nodiscard]] std::optional<PayloadLayout> BuildPayloadLayout(
        const uint8_t* payload,
        size_t length,
        const CIPHeader& header) noexcept {
        const uint8_t* cipStart = payload + kIsochHeaderSize;
        const auto* quadlets = reinterpret_cast<const uint32_t*>(cipStart);
        const size_t cipLength = length - kIsochHeaderSize;
        const size_t payloadBytes = cipLength - 8;
        const size_t dbsBytes = static_cast<size_t>(header.dataBlockSize) * 4u;

        if (dbsBytes == 0) {
            errorCount_++;
            return std::nullopt;
        }

        const size_t eventCount = payloadBytes / dbsBytes;
        lastDataBlockCount_ = static_cast<uint8_t>(eventCount);
        if (payloadBytes % dbsBytes != 0) {
            errorCount_++;
        }

        const uint32_t queueChannels = sharedRxQueue_ ? sharedRxQueue_->Channels() : 0;
        size_t decodeSlotsPerEvent =
            std::min<size_t>(header.dataBlockSize, static_cast<size_t>(Config::kMaxPcmChannels));
        if (queueChannels > 0) {
            decodeSlotsPerEvent = std::min<size_t>(decodeSlotsPerEvent, queueChannels);
        }

        return PayloadLayout{
            .dataPtr = &quadlets[2],
            .payloadBytes = payloadBytes,
            .eventCount = eventCount,
            .wireSlotsPerEvent = header.dataBlockSize,
            .decodeSlotsPerEvent = decodeSlotsPerEvent,
            .queueChannels = queueChannels,
            .queueWriteSafe = (!sharedRxQueue_) || (queueChannels <= Config::kMaxPcmChannels),
        };
    }

    void LogInterestingDbs(const CIPHeader& header,
                           const PayloadLayout& layout,
                           size_t length) noexcept {
        const uint32_t cipDbs = header.dataBlockSize;
        const bool interestingDbs =
            (cipDbs > Config::kMaxAmdtpDbs) ||
            (layout.queueChannels > 0 && cipDbs > layout.queueChannels);
        if (!interestingDbs) {
            return;
        }

        const bool stateChanged =
            (lastDbsDiagCipDbs_ != cipDbs) ||
            (lastDbsDiagQueueChannels_ != layout.queueChannels);
        ++dbsDiagHitCount_;
        if (!stateChanged) {
            return;
        }

        lastDbsDiagCipDbs_ = cipDbs;
        lastDbsDiagQueueChannels_ = layout.queueChannels;
        ASFW_LOG(Isoch,
                 "IR RX: len=%zu payload=%zu cipDbs=%u events=%zu queueCh=%u%{public}s",
                 length,
                 layout.payloadBytes,
                 cipDbs,
                 layout.eventCount,
                 layout.queueChannels,
                 (layout.queueChannels > 0 && cipDbs > layout.queueChannels)
                     ? " likely extra AM824 slot(s), possibly MIDI"
                     : "");

        uint32_t extraSlotIndex = UINT32_MAX;
        if (layout.eventCount > 0) {
            if (layout.queueChannels > 0 && cipDbs > layout.queueChannels) {
                extraSlotIndex = layout.queueChannels;
            } else if (cipDbs > Config::kMaxPcmChannels) {
                extraSlotIndex = Config::kMaxPcmChannels;
            }
        }

        if (extraSlotIndex == UINT32_MAX || extraSlotIndex >= header.dataBlockSize) {
            return;
        }

        const uint32_t q = SwapBigToHost(layout.dataPtr[extraSlotIndex]);
        const uint8_t label = static_cast<uint8_t>((q >> 24) & 0xFF);
        ASFW_LOG(Isoch,
                 "IR RX DICE diag: first extra slot[%u] label=0x%02x (%{public}s)",
                 extraSlotIndex,
                 label,
                 (label >= 0x80 && label <= 0x83) ? "MIDI-likely" : "non-MIDI/unknown");
    }

    void ProcessDecodedPayload(const CIPHeader& header, const PayloadLayout& layout) noexcept {
        if (layout.eventCount == 0) {
            emptyPacketCount_++;
            return;
        }

        samplePacketCount_++;
        if (layout.eventCount < minEvents_) {
            minEvents_ = layout.eventCount;
        }
        if (layout.eventCount > maxEvents_) {
            maxEvents_ = layout.eventCount;
        }

        if (layout.wireSlotsPerEvent > Config::kMaxAmdtpDbs) {
            errorCount_++;
            if (lastUnsupportedWireDbs_ != header.dataBlockSize) {
                lastUnsupportedWireDbs_ = header.dataBlockSize;
                ASFW_LOG(Isoch,
                         "IR RX: Unsupported wire DBS=%u (max AM824 slots=%zu, queueCh=%u) - skipping decode",
                         header.dataBlockSize,
                         static_cast<size_t>(Config::kMaxAmdtpDbs),
                         layout.queueChannels);
            }
            return;
        }

        for (size_t i = 0; i < layout.eventCount; ++i) {
            std::fill(std::begin(eventSamples_), std::end(eventSamples_), 0);

            for (size_t ch = 0; ch < layout.decodeSlotsPerEvent; ++ch) {
                const uint32_t sampleQuad =
                    layout.dataPtr[(i * layout.wireSlotsPerEvent) + ch];
                if (auto sample = AM824Decoder::DecodeSample(sampleQuad)) {
                    eventSamples_[ch] = *sample;
                } else if (AM824Decoder::IsMIDI(sampleQuad)) {
                    eventSamples_[ch] = 0;
                } else {
                    eventSamples_[ch] = 0;
                }
            }

            if (sharedRxQueue_ && layout.queueWriteSafe) {
                sharedRxQueue_->Write(eventSamples_, 1);
            } else if (sharedRxQueue_ && !layout.queueWriteSafe) {
                errorCount_++;
            }
        }
    }

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

    // Temporary rate-limited diagnostics for correlating CIP DBS with DICE stream formats.
    uint32_t dbsDiagHitCount_{0};
    uint32_t lastDbsDiagQueueChannels_{0};
    uint8_t lastDbsDiagCipDbs_{0xFF};
    
    // Output shared queue for decoded RX samples (set by IsochReceiveContext)
    ASFW::Shared::TxSharedQueueSPSC* sharedRxQueue_{nullptr};
    
    uint8_t lastUnsupportedWireDbs_{0};

    // Temp buffer for one PCM event's worth of samples (host-facing channels only).
    int32_t eventSamples_[Config::kMaxPcmChannels]{};
    
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
