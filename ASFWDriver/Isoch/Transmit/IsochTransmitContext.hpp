// IsochTransmitContext.hpp
// ASFW - Isochronous Transmit Context
//
// Public façade for IT transmit.
// Internals are modular:
//   - Tx::IsochTxDmaRing: low-level OHCI descriptor/payload engine (no audio semantics)
//   - IsochAudioTxPipeline: CIP/AM824 + buffering policy + near-HW audio injection
//   - IsochTxVerifier + IsochTxRecoveryController: dev-only verification + restart gating

#pragma once

#include "IsochAudioTxPipeline.hpp"
#include "IsochTxDmaRing.hpp"
#include "IsochTxLayout.hpp"
#include "IsochTxVerifier.hpp"
#include "IsochTxRecoveryController.hpp"

#include "../Memory/IIsochDMAMemory.hpp"
#include "../../Hardware/RegisterMap.hpp"

#include "../../Logging/Logging.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

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
    // Linux-style OHCI page padding constants (public API)
    // ==========================================================================
    static constexpr size_t kOHCIPageSize = Tx::Layout::kOHCIPageSize;
    static constexpr size_t kOHCIPrefetchSize = Tx::Layout::kOHCIPrefetchSize;
    static constexpr size_t kUsablePerPage = Tx::Layout::kUsablePerPage;

    static constexpr uint32_t kBlocksPerPacket = Tx::Layout::kBlocksPerPacket;
    static constexpr uint32_t kNumPackets = Tx::Layout::kNumPackets;
    static constexpr uint32_t kRingBlocks = Tx::Layout::kRingBlocks;

    static constexpr uint32_t kDescriptorStride = Tx::Layout::kDescriptorStride;
    static constexpr uint32_t kDescriptorsPerPageRaw = Tx::Layout::kDescriptorsPerPageRaw;
    static constexpr uint32_t kDescriptorsPerPage = Tx::Layout::kDescriptorsPerPage;
    static constexpr uint32_t kTotalPages = Tx::Layout::kTotalPages;
    static constexpr size_t kDescriptorRingSize = Tx::Layout::kDescriptorRingSize;

    static constexpr uint32_t kMaxPacketSize = Tx::Layout::kMaxPacketSize;
    static constexpr size_t kPayloadBufferSize = Tx::Layout::kPayloadBufferSize;

    static constexpr uint32_t kGuardBandPackets = Tx::Layout::kGuardBandPackets;
    static constexpr uint32_t kAudioWriteAhead = Tx::Layout::kAudioWriteAhead;
    static constexpr uint32_t kMaxWriteAhead = Tx::Layout::kMaxWriteAhead;

    // ==========================================================================
    // Public interface
    // ==========================================================================
    IsochTransmitContext() noexcept = default;
    ~IsochTransmitContext() noexcept;
    
    static std::unique_ptr<IsochTransmitContext> Create(
        Driver::HardwareInterface* hw,
        std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory) noexcept;

    kern_return_t Configure(uint8_t channel,
                            uint8_t sid,
                            uint32_t streamModeRaw = 0,
                            uint32_t requestedChannels = 0) noexcept;
    kern_return_t Start() noexcept;
    void Stop() noexcept;
    
    void Poll() noexcept;
    void HandleInterrupt() noexcept;
    void KickTxVerifier() noexcept;
    void ServiceTxRecovery() noexcept;

    State GetState() const noexcept { return state_; }
    Encoding::AudioRingBuffer<>& RingBuffer() noexcept { return audio_.RingBuffer(); }

    void SetSharedTxQueue(void* base, uint64_t bytes) noexcept;
    void SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept;
    uint32_t SharedTxFillLevelFrames() const noexcept;
    uint32_t SharedTxCapacityFrames() const noexcept;
    
    // ZERO-COPY: Set direct output audio buffer (from ASFWAudioNub)
    // This is the same buffer that CoreAudio writes to via IOUserAudioStream
    // No intermediate copy needed - IT DMA reads directly!
    void SetZeroCopyOutputBuffer(void* base, uint64_t bytes, uint32_t frameCapacity) noexcept;
    bool IsZeroCopyEnabled() const noexcept { return audio_.IsZeroCopyEnabled(); }
    Encoding::StreamMode RequestedStreamMode() const noexcept { return audio_.RequestedStreamMode(); }
    Encoding::StreamMode EffectiveStreamMode() const noexcept { return audio_.EffectiveStreamMode(); }
    
    uint64_t PacketsAssembled() const noexcept { return packetsAssembled_; }
    uint64_t DataPackets() const noexcept { return dataPackets_; }
    uint64_t NoDataPackets() const noexcept { return noDataPackets_; }
    uint64_t UnderrunCount() const noexcept { return audio_.UnderrunCount(); }
    uint32_t BufferFillLevel() const noexcept { return audio_.BufferFillLevel(); }
    
    void LogStatistics() const noexcept;
    void DumpPayloadBuffers(uint32_t numPackets = 4) const noexcept;
    void PrimeOnly() noexcept;
    void DumpDescriptorRing(uint32_t startPacket = 0, uint32_t numPackets = 8) const noexcept;

private:
    void WakeHardware() noexcept;
    void DoRefillOnce() noexcept;

    // ==========================================================================
    // Member variables
    // ==========================================================================
    Tx::IsochTxDmaRing ring_{};
    IsochAudioTxPipeline audio_{};
    IsochTxVerifier verifier_{};
    IsochTxRecoveryController recovery_{};

    State state_{State::Unconfigured};
    uint8_t channel_{0};
    uint8_t contextIndex_{0};
    
    Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory_;

    uint64_t packetsAssembled_{0};
    uint64_t dataPackets_{0};
    uint64_t noDataPackets_{0};
    uint64_t tickCount_{0};
    std::atomic<uint64_t> interruptCount_{0};
    
    // Refill coordination / IRQ-stall recovery
    std::atomic_flag refillInProgress_ = ATOMIC_FLAG_INIT;
    uint64_t lastInterruptCountSeen_{0};
    uint32_t irqStallTicks_{0};

    // 1F: Refill Latency Histogram (buckets: <50us, 50-200us, 200-500us, >500us)
    std::atomic<uint64_t> latencyBucket0_{0};
    std::atomic<uint64_t> latencyBucket1_{0};
    std::atomic<uint64_t> latencyBucket2_{0};
    std::atomic<uint64_t> latencyBucket3_{0};
    std::atomic<uint32_t> maxRefillLatencyUs_{0};
    std::atomic<uint64_t> irqWatchdogKicks_{0};

    // 1A: Last underrun count seen (for delta logging in Poll)
    uint64_t lastUnderrunCount_{0};
};

} // namespace Isoch
} // namespace ASFW
