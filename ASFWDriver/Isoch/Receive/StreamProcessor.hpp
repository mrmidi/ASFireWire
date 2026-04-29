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
#include <cstring>
#include <optional>
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
        uint32_t decodedFrames{0};
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

        const size_t effectiveLength = EffectivePacketLength(payload, length);
        if (effectiveLength < kIsochHeaderSize + 8) {
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

        RecordIsochLengthClamp(length, effectiveLength, *header);

        const auto layout = BuildPayloadLayout(payload, effectiveLength, *header);
        if (!layout) {
            return summary;
        }

        summary.decodedFrames = (layout->eventCount > UINT32_MAX)
                                  ? UINT32_MAX
                                  : static_cast<uint32_t>(layout->eventCount);
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
         FlushDecodedAllZeroRun(lastDBC_.load(std::memory_order_relaxed),
                                lastSYT_.load(std::memory_order_relaxed),
                                lastCIP_DBS_.load(std::memory_order_relaxed));
         // Single-line compact stats
         ASFW_LOG(Isoch, "RxStats: Pkts=%llu Data=%llu Empty=%llu Errs=%llu Drops=%llu Decoded=%llu ZeroDecoded=%llu/%llu maxZeroRun=%llu | CIP: SID=%u DBS=%u FDF=0x%02X SYT=0x%04X DBC=0x%02X",
             packetCount_.load(),
             samplePacketCount_.load(),
             emptyPacketCount_.load(),
             errorCount_.load(),
             discontinuityCount_.load(),
             decodedFrameCount_.load(std::memory_order_relaxed),
             decodedAllZeroFrameCount_.load(std::memory_order_relaxed),
             decodedAllZeroRunEvents_.load(std::memory_order_relaxed),
             decodedAllZeroMaxRunFrames_.load(std::memory_order_relaxed),
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
    uint64_t DecodedFrameCount() const { return decodedFrameCount_.load(std::memory_order_relaxed); }
    uint64_t DecodedAllZeroFrameCount() const {
        return decodedAllZeroFrameCount_.load(std::memory_order_relaxed);
    }
    uint64_t DecodedNonZeroFrameCount() const {
        return decodedNonZeroFrameCount_.load(std::memory_order_relaxed);
    }
    uint64_t DecodedAllZeroRunEvents() const {
        return decodedAllZeroRunEvents_.load(std::memory_order_relaxed);
    }
    uint64_t DecodedAllZeroMaxRunFrames() const {
        return decodedAllZeroMaxRunFrames_.load(std::memory_order_relaxed);
    }
    uint64_t RxQueueProducerDropEvents() const {
        return rxQueueProducerDropEvents_.load(std::memory_order_relaxed);
    }
    uint64_t RxQueueProducerDropFrames() const {
        return rxQueueProducerDropFrames_.load(std::memory_order_relaxed);
    }
    uint64_t PcmPayloadFallbackSamples() const {
        return pcmPayloadFallbackSamples_.load(std::memory_order_relaxed);
    }
    
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
        decodedFrameCount_ = 0;
        decodedAllZeroFrameCount_ = 0;
        decodedNonZeroFrameCount_ = 0;
        decodedAllZeroRunEvents_ = 0;
        decodedAllZeroMaxRunFrames_ = 0;
        currentAllZeroRunFrames_ = 0;
        currentAllZeroRunStartFrame_ = 0;
        currentAllZeroRunLogged_ = false;
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
        rxQueueProducerDropEvents_ = 0;
        rxQueueProducerDropFrames_ = 0;
        pcmPayloadFallbackSamples_ = 0;
        lastPcmPayloadFallbackLabel_ = 0;
        lastPcmPayloadFallbackChannel_ = 0;
        isochLengthClampEvents_ = 0;
        isochLengthClampBytes_ = 0;
        lastIsochHeaderDataLength_ = 0;
    }

private:
    struct IsochHeaderDecode {
        bool valid{false};
        uint16_t dataLength{0};
        uint32_t header{0};
    };

    struct PayloadLayout {
        const uint32_t* dataPtr{nullptr};
        size_t payloadBytes{0};
        size_t eventCount{0};
        size_t wireSlotsPerEvent{0};
        size_t decodeSlotsPerEvent{0};
        uint32_t queueChannels{0};
        bool queueWriteSafe{true};
    };

    [[nodiscard]] static uint32_t LoadU32(const uint8_t* bytes) noexcept {
        uint32_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }

    [[nodiscard]] static bool IsPlausibleIsochHeader(uint32_t header,
                                                     size_t availableDataBytes) noexcept {
        const uint16_t dataLength = static_cast<uint16_t>((header >> 16) & 0xFFFFU);
        const uint8_t tcode = static_cast<uint8_t>((header >> 4) & 0x0FU);

        return tcode == 0x0AU &&
               dataLength >= 8U &&
               dataLength <= availableDataBytes &&
               (dataLength % sizeof(uint32_t)) == 0;
    }

    [[nodiscard]] static IsochHeaderDecode DecodeIsochHeader(const uint8_t* payload,
                                                             size_t length) noexcept {
        if (!payload || length < kIsochHeaderSize + 8) {
            return {};
        }

        const size_t availableDataBytes = length - kIsochHeaderSize;
        const uint32_t native = LoadU32(payload + 4);
        const uint32_t candidates[] = {
            native,
            SwapBigToHost(native),
        };

        for (uint32_t header : candidates) {
            if (!IsPlausibleIsochHeader(header, availableDataBytes)) {
                continue;
            }
            return IsochHeaderDecode{
                .valid = true,
                .dataLength = static_cast<uint16_t>((header >> 16) & 0xFFFFU),
                .header = header,
            };
        }

        return {};
    }

    [[nodiscard]] size_t EffectivePacketLength(const uint8_t* payload, size_t length) noexcept {
        const auto decoded = DecodeIsochHeader(payload, length);
        if (!decoded.valid) {
            return length;
        }

        lastIsochHeaderDataLength_.store(decoded.dataLength, std::memory_order_relaxed);
        return kIsochHeaderSize + static_cast<size_t>(decoded.dataLength);
    }

    void RecordIsochLengthClamp(size_t descriptorLength,
                                size_t effectiveLength,
                                const CIPHeader& header) noexcept {
        if (effectiveLength >= descriptorLength) {
            return;
        }

        const uint64_t bytes =
            isochLengthClampBytes_.fetch_add(descriptorLength - effectiveLength,
                                             std::memory_order_relaxed) +
            (descriptorLength - effectiveLength);
        const uint64_t events =
            isochLengthClampEvents_.fetch_add(1, std::memory_order_relaxed) + 1;

        ASFW_LOG_RL(Isoch,
                    "rx/isoch-length-clamp",
                    1000,
                    OS_LOG_TYPE_DEFAULT,
                    "IR RX isoch length clamp descriptor=%zu effective=%zu trimmedBytes=%llu events=%llu dataLength=%u dbc=0x%02x syt=0x%04x dbs=%u",
                    descriptorLength,
                    effectiveLength,
                    bytes,
                    events,
                    lastIsochHeaderDataLength_.load(std::memory_order_relaxed),
                    header.dataBlockCounter,
                    header.syt,
                    header.dataBlockSize);
    }

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
            (layout.queueChannels > 0 && cipDbs != layout.queueChannels);
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
                     : (layout.queueChannels > 0 && cipDbs < layout.queueChannels)
                         ? " likely host queue larger than AM824 stream"
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
        const uint64_t decodedFrameBase =
            decodedFrameCount_.fetch_add(static_cast<uint64_t>(layout.eventCount),
                                         std::memory_order_relaxed);
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
                const uint8_t label = AM824Decoder::Label(sampleQuad);
                if (auto sample = AM824Decoder::DecodeConfiguredPcmSlot(sampleQuad)) {
                    eventSamples_[ch] = *sample;
                    if (!AM824Decoder::IsMultiBitLinearAudioLabel(label)) {
                        RecordPcmPayloadFallback(header, ch, label);
                    }
                }
            }

            const bool hasNonZeroDecodedSample = EventHasNonZeroDecodedSample(layout);
            RecordDecodedFrameContent(header,
                                      layout,
                                      decodedFrameBase + static_cast<uint64_t>(i),
                                      hasNonZeroDecodedSample);

            if (sharedRxQueue_ && layout.queueWriteSafe) {
                if (sharedRxQueue_->Write(eventSamples_, 1) != 1) {
                    const uint64_t frames =
                        rxQueueProducerDropFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
                    const uint64_t events =
                        rxQueueProducerDropEvents_.fetch_add(1, std::memory_order_relaxed) + 1;
                    ASFW_LOG_RL(Isoch,
                                "rxq/producer-drop",
                                500,
                                OS_LOG_TYPE_DEFAULT,
                                "IR RX QUEUE OVERRUN droppedFrames=%llu events=%llu fill=%u cap=%u dbc=0x%02x syt=0x%04x",
                                frames,
                                events,
                                sharedRxQueue_->FillLevelFrames(),
                                sharedRxQueue_->CapacityFrames(),
                                header.dataBlockCounter,
                                header.syt);
                }
            } else if (sharedRxQueue_ && !layout.queueWriteSafe) {
                errorCount_++;
            }
        }
    }

    [[nodiscard]] bool EventHasNonZeroDecodedSample(const PayloadLayout& layout) const noexcept {
        for (size_t ch = 0; ch < layout.decodeSlotsPerEvent; ++ch) {
            if (eventSamples_[ch] != 0) {
                return true;
            }
        }
        return false;
    }

    void RecordDecodedFrameContent(const CIPHeader& header,
                                   const PayloadLayout& layout,
                                   uint64_t decodedFrameIndex,
                                   bool hasNonZeroDecodedSample) noexcept {
        if (hasNonZeroDecodedSample) {
            decodedNonZeroFrameCount_.fetch_add(1, std::memory_order_relaxed);
            FlushDecodedAllZeroRun(header.dataBlockCounter, header.syt, header.dataBlockSize);
            return;
        }

        decodedAllZeroFrameCount_.fetch_add(1, std::memory_order_relaxed);
        if (currentAllZeroRunFrames_ == 0) {
            currentAllZeroRunStartFrame_ = decodedFrameIndex;
            currentAllZeroRunLogged_ = false;
        }
        ++currentAllZeroRunFrames_;

        if (!currentAllZeroRunLogged_ &&
            currentAllZeroRunFrames_ >= kDecodedAllZeroOngoingLogFrames) {
            currentAllZeroRunLogged_ = true;
            ASFW_LOG(Isoch,
                     "IR RX decoded all-zero run ongoing frames=%llu startFrame=%llu dbc=0x%02x syt=0x%04x dbs=%u queueFill=%u cap=%u decodeSlots=%zu queueCh=%u",
                     currentAllZeroRunFrames_,
                     currentAllZeroRunStartFrame_,
                     header.dataBlockCounter,
                     header.syt,
                     header.dataBlockSize,
                     sharedRxQueue_ ? sharedRxQueue_->FillLevelFrames() : 0,
                     sharedRxQueue_ ? sharedRxQueue_->CapacityFrames() : 0,
                     layout.decodeSlotsPerEvent,
                     layout.queueChannels);
        }
    }

    void FlushDecodedAllZeroRun(uint8_t dbc, uint16_t syt, uint8_t dbs) noexcept {
        if (currentAllZeroRunFrames_ == 0) {
            return;
        }

        const uint64_t runFrames = currentAllZeroRunFrames_;
        const uint64_t runStart = currentAllZeroRunStartFrame_;
        const uint64_t runEvents =
            decodedAllZeroRunEvents_.fetch_add(1, std::memory_order_relaxed) + 1;

        uint64_t maxRun = decodedAllZeroMaxRunFrames_.load(std::memory_order_relaxed);
        if (runFrames > maxRun) {
            decodedAllZeroMaxRunFrames_.store(runFrames, std::memory_order_relaxed);
            maxRun = runFrames;
        }

        if (runFrames >= kDecodedAllZeroRunLogFrames || currentAllZeroRunLogged_) {
            ASFW_LOG(Isoch,
                     "IR RX decoded all-zero run frames=%llu startFrame=%llu endFrame=%llu runs=%llu totalZeroFrames=%llu maxRun=%llu dbc=0x%02x syt=0x%04x dbs=%u queueFill=%u cap=%u",
                     runFrames,
                     runStart,
                     runStart + runFrames,
                     runEvents,
                     decodedAllZeroFrameCount_.load(std::memory_order_relaxed),
                     maxRun,
                     dbc,
                     syt,
                     dbs,
                     sharedRxQueue_ ? sharedRxQueue_->FillLevelFrames() : 0,
                     sharedRxQueue_ ? sharedRxQueue_->CapacityFrames() : 0);
        }

        currentAllZeroRunFrames_ = 0;
        currentAllZeroRunStartFrame_ = 0;
        currentAllZeroRunLogged_ = false;
    }

    void RecordPcmPayloadFallback(const CIPHeader& header, size_t channel, uint8_t label) noexcept {
        const uint64_t count =
            pcmPayloadFallbackSamples_.fetch_add(1, std::memory_order_relaxed) + 1;
        lastPcmPayloadFallbackLabel_.store(label, std::memory_order_relaxed);
        lastPcmPayloadFallbackChannel_.store(static_cast<uint32_t>(channel),
                                             std::memory_order_relaxed);
        ASFW_LOG_RL(Isoch,
                    "rx/pcm-label-fallback",
                    1000,
                    OS_LOG_TYPE_DEFAULT,
                    "IR RX PCM slot payload fallback label=0x%02x ch=%zu samples=%llu dbc=0x%02x syt=0x%04x dbs=%u",
                    label,
                    channel,
                    count,
                    header.dataBlockCounter,
                    header.syt,
                    header.dataBlockSize);
    }

    std::atomic<uint64_t> packetCount_{0};
    std::atomic<uint64_t> samplePacketCount_{0};
    std::atomic<uint64_t> emptyPacketCount_{0};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> discontinuityCount_{0};
    std::atomic<uint64_t> decodedFrameCount_{0};
    std::atomic<uint64_t> decodedAllZeroFrameCount_{0};
    std::atomic<uint64_t> decodedNonZeroFrameCount_{0};
    std::atomic<uint64_t> decodedAllZeroRunEvents_{0};
    std::atomic<uint64_t> decodedAllZeroMaxRunFrames_{0};
    std::atomic<uint64_t> rxQueueProducerDropEvents_{0};
    std::atomic<uint64_t> rxQueueProducerDropFrames_{0};
    std::atomic<uint64_t> pcmPayloadFallbackSamples_{0};
    std::atomic<uint8_t> lastPcmPayloadFallbackLabel_{0};
    std::atomic<uint32_t> lastPcmPayloadFallbackChannel_{0};
    std::atomic<uint64_t> isochLengthClampEvents_{0};
    std::atomic<uint64_t> isochLengthClampBytes_{0};
    std::atomic<uint32_t> lastIsochHeaderDataLength_{0};
    
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

    static constexpr uint64_t kDecodedAllZeroRunLogFrames = 48;       // 1 ms @ 48 kHz
    static constexpr uint64_t kDecodedAllZeroOngoingLogFrames = 480;  // 10 ms @ 48 kHz
    uint64_t currentAllZeroRunFrames_{0};
    uint64_t currentAllZeroRunStartFrame_{0};
    bool currentAllZeroRunLogged_{false};
    
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
