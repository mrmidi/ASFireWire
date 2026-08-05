// IsochTransmitContext.hpp
// ASFW - Isochronous Transmit Context
//
// Public façade for IT transmit.
// Internals are modular:
//   - Tx::IsochTxDmaRing: low-level OHCI descriptor/payload engine (no transport semantics)
//   - Shared transport memory mapping (payload slab, metadata ring, control block)

#pragma once

#include "IsochTxDmaRing.hpp"
#include "IsochTxLayout.hpp"

#include "../Core/IsochTxQueue.hpp"
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

namespace Driver { class HardwareInterface; }

namespace Isoch {

enum class ITState {
    Unconfigured,
    Configured,
    Running,
    Stopped
};

/**
 * @brief Orchestrator for an Isochronous Transmit (IT) context.
 * 
 * This class owns the OHCI DMA ring (IsochTxDmaRing) and manages the lifecycle 
 * of the transport. It treats packet headers and payload bytes as opaque.
 */
class IsochTransmitContext final {
public:
    using State = ITState;
    using TxPreparationCallback = std::function<void(uint64_t generation)>;

    // ==========================================================================
    // Public interface
    // ==========================================================================
    IsochTransmitContext() noexcept = default;
    ~IsochTransmitContext() noexcept;
    
    static std::unique_ptr<IsochTransmitContext> Create(
        Driver::HardwareInterface* hw,
        std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory) noexcept;

    kern_return_t Configure(uint8_t channel, uint8_t sid) noexcept;

    // Select which OHCI IT hardware context backs this stream. Defaults to 0
    // (master); secondary streams must use their own context (== streamIndex) or
    // they collide with the master on context 0's registers. Set before Start().
    void SetContextIndex(uint8_t index) noexcept { contextIndex_ = index; }

    /**
     * @brief Map the shared memory regions allocated by the host into the Dext address space.
     * Prepares the payload slab for DMA and resolves its physical IOVA for descriptor priming.
     * @param payloadSlab Shared memory descriptor containing all packet payloads.
     * @param metadataRing Shared memory descriptor containing packet metadata.
     * @param controlBlock Shared memory descriptor containing stream control states.
     * @param interruptInterval Interrupt interval in packets.
     */
    kern_return_t SetSharedMemoryDescriptors(
        IOMemoryDescriptor* payloadSlab,
        IOMemoryDescriptor* metadataRing,
        IOMemoryDescriptor* controlBlock,
        uint32_t interruptInterval) noexcept;

    kern_return_t Start() noexcept;
    // Clearing RUN only prevents new descriptor fetches.  The caller must not
    // release any DMA-visible memory until this returns success (ACTIVE clear).
    [[nodiscard]] kern_return_t Stop() noexcept;
    
    void Poll() noexcept;
    void HandleInterrupt() noexcept;
    void SetTxPreparationCallback(TxPreparationCallback callback) noexcept;

    State GetState() const noexcept { return state_; }
    
    uint64_t PacketsAssembled() const noexcept { return packetsAssembled_; }
    
    void LogStatistics() const noexcept;
    void DumpDescriptorRing(uint32_t startPacket = 0, uint32_t numPackets = 8) const noexcept;

private:
    void WakeHardware() noexcept;
    void DoRefillOnce(uint64_t eventHostTicks, bool publishTimingEvent) noexcept;
    void StopImmediatelyForTxFault() noexcept;

    // ==========================================================================
    // Member variables
    // ==========================================================================
    Tx::IsochTxDmaRing ring_{};

    State state_{State::Unconfigured};
    uint8_t channel_{0};
    uint8_t contextIndex_{0};
    
    Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory_;

    uint64_t packetsAssembled_{0};
    uint64_t tickCount_{0};
    std::atomic<uint64_t> interruptCount_{0};
    
    // Refill coordination / IRQ-stall recovery
    std::atomic_flag refillInProgress_ = ATOMIC_FLAG_INIT;
    uint64_t lastInterruptCountSeen_{0};
    uint32_t irqStallTicks_{0};

    // Consecutive watchdog kicks with zero interrupts observed. The watchdog
    // bridges interrupt-delivery jitter only: its cadence is far coarser than
    // the 48-packet descriptor ring, so a stream carried by the watchdog
    // re-transmits stale ring laps between kicks. A sustained silent streak
    // is a dead interrupt path and the context must stop honestly.
    static constexpr uint32_t kIrqSilentKickFatalThreshold = 16;
    uint32_t irqSilentKickStreak_{0};

    // Refill Latency Histogram (buckets: <50us, 50-200us, 200-500us, >500us)
    std::atomic<uint64_t> latencyBucket0_{0};
    std::atomic<uint64_t> latencyBucket1_{0};
    std::atomic<uint64_t> latencyBucket2_{0};
    std::atomic<uint64_t> latencyBucket3_{0};
    std::atomic<uint32_t> maxRefillLatencyUs_{0};
    std::atomic<uint64_t> irqWatchdogKicks_{0};
    TxPreparationCallback txPreparationCallback_{};

    // Shared transport memory regions
    OSSharedPtr<IOMemoryMap> payloadMap_{nullptr};
    OSSharedPtr<IOMemoryMap> metadataMap_{nullptr};
    OSSharedPtr<IOMemoryMap> controlMap_{nullptr};

    uint8_t* payloadBase_{nullptr};
    IsochTxPacketMeta* metadataRing_{nullptr};
    IsochTxQueueControl* controlBlock_{nullptr};

    Tx::TxPayloadDmaMap payloadDmaMap_{};
    OSSharedPtr<IODMACommand> payloadDmaCmd_{nullptr};
};

} // namespace Isoch
} // namespace ASFW
