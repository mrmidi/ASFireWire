// IsochTransmitContext.hpp
// ASFW - Isochronous Transmit Context
//
// Manages the IT context for transmitting audio via FireWire 1394.
// Phase 2: Real Hardware IT DMA with Linux-style page padding
//
// Data flow:
//   CoreAudio → IOOperationHandler → RingBuffer → PacketAssembler → DMA → Wire
//                                                    ↑
//                                              (owned by this class)
//
// Page Padding Strategy (Linux firewire-ohci):
//   OHCI controllers prefetch 32 bytes for each descriptor. If a descriptor
//   starts in the last 32 bytes of a 4K page, the prefetch crosses into the
//   next page, potentially causing URE if that page isn't mapped.
//   Solution: Use 252 descriptors per 4K page (packet-aligned), leaving 64 bytes
//   unused at the end of each page.

#pragma once

#include "../Encoding/PacketAssembler.hpp"
#include "../Encoding/SYTGenerator.hpp"
#include "../Memory/IIsochDMAMemory.hpp"
#include "../../Shared/TxSharedQueue.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"

#include "../../Logging/Logging.hpp"
#include <atomic>
#include <cstdint>
#include <memory>

namespace ASFW {

namespace Driver { class HardwareInterface; }

namespace Isoch {

enum class ITState {
    Unconfigured,
    Configured,
    Running,
    Stopped
};

class IsochTransmitContext {
public:
    using State = ITState;

    // ==========================================================================
    // Linux-style OHCI page padding constants
    // ==========================================================================
    static constexpr size_t kOHCIPageSize = 4096;
    static constexpr size_t kOHCIPrefetchSize = 32;
    static constexpr size_t kUsablePerPage = kOHCIPageSize - kOHCIPrefetchSize;  // 4064

    static constexpr uint32_t kBlocksPerPacket = 3;
    static constexpr uint32_t kNumPackets = 200;  // ~25ms audio buffer at 8000 pkts/sec
    static constexpr uint32_t kRingBlocks = kNumPackets * kBlocksPerPacket;  // 600

    static constexpr uint32_t kDescriptorStride = 16;
    static constexpr uint32_t kDescriptorsPerPageRaw = 
        static_cast<uint32_t>(kUsablePerPage / kDescriptorStride);  // 254
    // Packet-aligned: ensures packets never straddle page boundaries
    static constexpr uint32_t kDescriptorsPerPage = 
        (kDescriptorsPerPageRaw / kBlocksPerPacket) * kBlocksPerPacket;  // 252

    static constexpr uint32_t kTotalPages = 
        (kRingBlocks + kDescriptorsPerPage - 1) / kDescriptorsPerPage;  // 3

    static constexpr size_t kDescriptorRingSize = kTotalPages * kOHCIPageSize;  // 12288
    static constexpr uint32_t kMaxPacketSize = 4096;
    static constexpr size_t kPayloadBufferSize = kNumPackets * kMaxPacketSize;

    // Static assertions
    static_assert(kDescriptorsPerPage >= kBlocksPerPacket, "Need at least one packet per page");
    static_assert((kDescriptorsPerPage % kBlocksPerPacket) == 0, "Keep packets within a page");
    static_assert((kDescriptorsPerPage * kDescriptorStride) <= kUsablePerPage, "Must fit in usable space");
    static_assert(kBlocksPerPacket == 3, "Z must be 3 for OMI(2)+OL(1)");
    static_assert(sizeof(Async::HW::OHCIDescriptor) == 16, "OHCI descriptor must be 16 bytes");
    static_assert(kDescriptorStride == sizeof(Async::HW::OHCIDescriptor), "Stride must match descriptor size");

    static constexpr uint32_t kPacketsPerBatch = 8;
    
    // Initial prime target: ~8ms write-ahead (avoids full-ring index ambiguity)
    static constexpr uint32_t kInitialPrimePackets = 64;

    // ==========================================================================
    // Public interface
    // ==========================================================================
    IsochTransmitContext() noexcept = default;
    
    static std::unique_ptr<IsochTransmitContext> Create(
        Driver::HardwareInterface* hw,
        std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory) noexcept;

    kern_return_t Configure(uint8_t channel, uint8_t sid, uint32_t streamModeRaw = 0) noexcept;
    kern_return_t Start() noexcept;
    void Stop() noexcept;
    
    void Poll() noexcept;
    void HandleInterrupt() noexcept;

    State GetState() const noexcept { return state_; }
    Encoding::AudioRingBuffer<>& RingBuffer() noexcept { return assembler_.ringBuffer(); }

    void SetSharedTxQueue(void* base, uint64_t bytes) noexcept;
    uint32_t SharedTxFillLevelFrames() const noexcept;
    uint32_t SharedTxCapacityFrames() const noexcept;
    
    // ZERO-COPY: Set direct output audio buffer (from ASFWAudioNub)
    // This is the same buffer that CoreAudio writes to via IOUserAudioStream
    // No intermediate copy needed - IT DMA reads directly!
    void SetZeroCopyOutputBuffer(void* base, uint64_t bytes, uint32_t frameCapacity) noexcept;
    bool IsZeroCopyEnabled() const noexcept { return zeroCopyEnabled_; }
    Encoding::StreamMode RequestedStreamMode() const noexcept { return requestedStreamMode_; }
    Encoding::StreamMode EffectiveStreamMode() const noexcept { return effectiveStreamMode_; }
    
    uint64_t PacketsAssembled() const noexcept { return packetsAssembled_; }
    uint64_t DataPackets() const noexcept { return dataPackets_; }
    uint64_t NoDataPackets() const noexcept { return noDataPackets_; }
    uint64_t UnderrunCount() const noexcept { return assembler_.underrunCount(); }
    uint32_t BufferFillLevel() const noexcept { return assembler_.bufferFillLevel(); }
    
    void LogStatistics() const noexcept;
    void DumpPayloadBuffers(uint32_t numPackets = 4) const noexcept;
    void PrimeOnly() noexcept;
    void DumpDescriptorRing(uint32_t startPacket = 0, uint32_t numPackets = 8) const noexcept;

private:
    kern_return_t SetupRings() noexcept;
    void PrimeRing() noexcept;
    void RefillRing() noexcept;
    void WakeHardware() noexcept;
    void DumpAtCmdPtr() const noexcept;

    // ==========================================================================
    // Page-aware descriptor access helpers (Linux-style padding)
    // ==========================================================================
    /// Get virtual pointer to descriptor by logical index (handles page gaps)
    [[nodiscard]] Async::HW::OHCIDescriptor* GetDescriptorPtr(uint32_t logicalIndex) noexcept;
    [[nodiscard]] const Async::HW::OHCIDescriptor* GetDescriptorPtr(uint32_t logicalIndex) const noexcept;

    /// Get IOVA of descriptor by logical index (handles page gaps)
    [[nodiscard]] uint32_t GetDescriptorIOVA(uint32_t logicalIndex) const noexcept;

    /// Decode hardware cmdAddr to logical descriptor index (inverse mapping)
    [[nodiscard]] bool DecodeCmdAddrToLogicalIndex(uint32_t cmdAddr, uint32_t& outLogicalIndex) const noexcept;

    /// Validate layout in debug builds (called once in PrimeRing)
    void ValidateDescriptorLayout() const noexcept;

    // ==========================================================================
    // Member variables
    // ==========================================================================
    Encoding::PacketAssembler assembler_;
    Shared::TxSharedQueueSPSC sharedTxQueue_;
    
    // ZERO-COPY: Direct pointer to CoreAudio output buffer
    // When enabled, skip sharedTxQueue_ and read audio directly from here
    void* zeroCopyAudioBase_{nullptr};
    uint64_t zeroCopyAudioBytes_{0};
    uint32_t zeroCopyFrameCapacity_{0};
    uint32_t zeroCopyReadFrame_{0};      // Current read position in frames
    bool zeroCopyEnabled_{false};
    Encoding::StreamMode requestedStreamMode_{Encoding::StreamMode::kNonBlocking};
    Encoding::StreamMode effectiveStreamMode_{Encoding::StreamMode::kNonBlocking};
    
    State state_{State::Unconfigured};
    uint8_t channel_{0};
    uint8_t contextIndex_{0};
    
    Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory_;

    // Raw DMA regions (no DescriptorRing class - incompatible with page gaps)
    Shared::DMARegion descRegion_{};
    Shared::DMARegion bufRegion_{};

    uint32_t payloadIndex_{0};
    uint32_t softwareFillIndex_{0};
    uint32_t lastHwPacketIndex_{0};

    uint64_t packetsAssembled_{0};
    uint64_t dataPackets_{0};
    uint64_t noDataPackets_{0};
    uint64_t tickCount_{0};
    std::atomic<uint64_t> interruptCount_{0};
    
    // SYT generation (cycle-based per IEC 61883-6, Linux approach)
    std::unique_ptr<Encoding::SYTGenerator> sytGenerator_;
    uint64_t samplesSinceStart_{0};

    // OHCI cycle tracking for SYT generation
    uint32_t nextTransmitCycle_{0};    // Expected cycle for next packet to fill
    bool cycleTrackingValid_{false};   // Whether we've synced to hardware
    uint32_t lastHwTimestamp_{0};      // Last read hardware timestamp (diagnostics)

    // Refill coordination / IRQ-stall recovery
    std::atomic_flag refillInProgress_ = ATOMIC_FLAG_INIT;
    uint64_t lastInterruptCountSeen_{0};
    uint32_t irqStallTicks_{0};

    // RT-safe refill telemetry (written in ISR/refill path, read/logged from Poll).
    struct RTRefillCounters {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> exitNotRunning{0};
        std::atomic<uint64_t> exitDead{0};
        std::atomic<uint64_t> exitDecodeFail{0};
        std::atomic<uint64_t> exitHwOOB{0};
        std::atomic<uint64_t> exitZeroRefill{0};
        std::atomic<uint64_t> resyncApplied{0};
        std::atomic<uint64_t> staleFramesDropped{0};
        std::atomic<uint64_t> refills{0};
        std::atomic<uint64_t> packetsRefilled{0};
        std::atomic<uint64_t> legacyPumpMovedFrames{0};
        std::atomic<uint64_t> legacyPumpSkipped{0};
        std::atomic<uint64_t> fatalPacketSize{0};
        std::atomic<uint64_t> fatalDescriptorBounds{0};
        std::atomic<uint64_t> irqWatchdogKicks{0};
    } rtRefill_;

    struct RTRefillSnapshot {
        uint64_t calls{0};
        uint64_t exitNotRunning{0};
        uint64_t exitDead{0};
        uint64_t exitDecodeFail{0};
        uint64_t exitHwOOB{0};
        uint64_t exitZeroRefill{0};
        uint64_t resyncApplied{0};
        uint64_t staleFramesDropped{0};
        uint64_t refills{0};
        uint64_t packetsRefilled{0};
        uint64_t legacyPumpMovedFrames{0};
        uint64_t legacyPumpSkipped{0};
        uint64_t fatalPacketSize{0};
        uint64_t fatalDescriptorBounds{0};
        uint64_t irqWatchdogKicks{0};
    } rtRefillLast_;
};

} // namespace Isoch
} // namespace ASFW
