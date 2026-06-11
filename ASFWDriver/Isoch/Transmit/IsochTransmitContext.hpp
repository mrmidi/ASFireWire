// IsochTransmitContext.hpp
// ASFW - Isochronous Transmit Context
//
// Public façade for IT transmit.
// Internals are modular:
//   - Tx::IsochTxDmaRing: low-level OHCI descriptor/payload engine (no audio semantics)
//   - IsochAudioTxPipeline: CIP/AM824 + direct ADK memory mapping

#pragma once

#include "../../AudioEngine/DirectIsoch/IsochAudioTxPipeline.hpp"
#include "IsochTxDmaRing.hpp"
#include "IsochTxLayout.hpp"

#include "../Memory/IIsochDMAMemory.hpp"
#include "../../Hardware/RegisterMap.hpp"

#include "../../Logging/Logging.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW {

namespace Audio::Runtime {
class IDirectAudioBindingSource;
}

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
                            uint32_t requestedChannels = 0,
                            uint32_t requestedAm824Slots = 0,
                            Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824) noexcept;
    kern_return_t Start() noexcept;
    void Stop() noexcept;
    
    void Poll() noexcept;
    void HandleInterrupt() noexcept;
    void RequestPayloadPreparation(uint64_t requestGeneration) noexcept;

    State GetState() const noexcept { return state_; }

    void SetExternalSyncBridge(ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge* bridge) noexcept;
    
    void SetDirectAudioBindingSource(ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept;
    void SetDirectTxRuntimeBinding(const IsochAudioTxPipeline::DirectTxRuntimeBinding& binding) noexcept;
    Encoding::StreamMode RequestedStreamMode() const noexcept { return audio_.RequestedStreamMode(); }
    Encoding::StreamMode EffectiveStreamMode() const noexcept { return audio_.EffectiveStreamMode(); }
    
    uint64_t PacketsAssembled() const noexcept { return packetsAssembled_; }
    uint64_t DataPackets() const noexcept { return dataPackets_; }
    uint64_t NoDataPackets() const noexcept { return noDataPackets_; }
    
    void LogStatistics() const noexcept;
    void DumpDescriptorRing(uint32_t startPacket = 0, uint32_t numPackets = 8) const noexcept;

private:
    void WakeHardware() noexcept;
    void RefreshDirectAudioBinding(bool force) noexcept;
    void DoRefillOnce(uint64_t eventHostTicks, bool publishTimingEvent) noexcept;
    [[nodiscard]] bool DoPrepareOnce() noexcept;
    [[nodiscard]] bool DrainPreparationRequests() noexcept;
    void ReleasePreparationGate() noexcept;
    void StopImmediatelyForTxFault() noexcept;
    void StopImmediatelyForTxFault(const Tx::IsochTxDmaRing::PreparationOutcome& outcome) noexcept;

    // ==========================================================================
    // Member variables
    // ==========================================================================
    Tx::IsochTxDmaRing ring_{};
    IsochAudioTxPipeline audio_{};

    State state_{State::Unconfigured};
    uint8_t channel_{0};
    uint8_t contextIndex_{0};
    
    Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory_;

    ASFW::Audio::Runtime::IDirectAudioBindingSource* directAudioBindingSource_{nullptr};
    uint64_t lastDirectAudioGeneration_{0};

    uint64_t packetsAssembled_{0};
    uint64_t dataPackets_{0};
    uint64_t noDataPackets_{0};
    uint64_t tickCount_{0};
    std::atomic<uint64_t> interruptCount_{0};
    
    // Refill coordination / IRQ-stall recovery
    std::atomic_flag refillInProgress_ = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> requestedPreparationGeneration_{0};
    uint64_t lastInterruptCountSeen_{0};
    uint32_t irqStallTicks_{0};

    // Refill Latency Histogram (buckets: <50us, 50-200us, 200-500us, >500us)
    std::atomic<uint64_t> latencyBucket0_{0};
    std::atomic<uint64_t> latencyBucket1_{0};
    std::atomic<uint64_t> latencyBucket2_{0};
    std::atomic<uint64_t> latencyBucket3_{0};
    std::atomic<uint32_t> maxRefillLatencyUs_{0};
    std::atomic<uint64_t> irqWatchdogKicks_{0};
};

} // namespace Isoch
} // namespace ASFW
