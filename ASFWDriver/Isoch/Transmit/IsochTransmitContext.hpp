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

#include "../Memory/IIsochDMAMemory.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Shared/Isoch/IsochAudioTransport.hpp"

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
 * of the transport. It does not interpret the payload bytes (AMDTP/CIP/Audio).
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
     * @param ztsPeriodFrames Verification parameter for HAL ring wrap (passed through to control block).
     */
    kern_return_t SetSharedMemoryDescriptors(
        IOMemoryDescriptor* payloadSlab,
        IOMemoryDescriptor* metadataRing,
        IOMemoryDescriptor* controlBlock,
        uint32_t interruptInterval,
        uint32_t ztsPeriodFrames) noexcept;

    kern_return_t Start() noexcept;

    // Stage 5E descriptor safety seam: build and validate the complete OHCI IT descriptor
    // program against the already-prefilled shared packet ring, but do not
    // write CommandPtr, set RUN, enable interrupts, or transmit a bus packet.
    kern_return_t PrimeForPreflight() noexcept;

    // Stage 5E safety seam: program and read back the inert OHCI CommandPtr for
    // the already-validated descriptor ring. RUN and interrupts remain clear,
    // so the controller cannot fetch or transmit a packet.
    kern_return_t ProgramCommandPtrForPreflight() noexcept;

    // Stage 5E: replace the validated packet ring with a finite chain of 48
    // true skip descriptors, start the OHCI IT context with interrupts masked,
    // and prove execution from descriptor completion status. ACTIVE is sampled
    // only for diagnostics because a completed finite program normally becomes
    // dormant with RUN=1, ACTIVE=0 and DEAD=0. No bus packet descriptor exists.
    kern_return_t RunAllSkipForPreflight(uint32_t durationMs) noexcept;

    // Stage 5F: reduce the already-validated FF800 ring to one finite, silent,
    // no-CIP packet, run it with interrupts masked, require ack_complete, and
    // stop the context. This emits exactly one isochronous packet on the
    // protocol-held IRM channel; the FF800 communication engine remains stopped.
    kern_return_t RunSingleSilencePacketForPreflight(uint32_t timeoutMs) noexcept;

    // Stage 5G: prepare one finite 48-cycle D-D-D-S silent cadence while RUN
    // is clear, publish all payload/descriptor DMA state, and program/read back
    // CommandPtr. The caller starts the FF800 engine only after this succeeds.
    kern_return_t PrepareFiniteSilenceCadenceForPreflight() noexcept;

    // Stage 5G: execute the previously prepared finite cadence once with
    // interrupts masked, validate descriptor writeback and 1,1,2 cycle timing,
    // then stop the OHCI context cleanly. No descriptor refill or live stream.
    kern_return_t RunPreparedFiniteSilenceCadenceForPreflight(
        uint32_t timeoutMs) noexcept;

    // Stage 5G unconditional teardown seam. Clear RUN even when preparation or
    // device start failed before the normal RUN path, wait boundedly for ACTIVE
    // to drain, verify DEAD is clear, and discard all finite-cadence state.
    kern_return_t CleanupFiniteSilenceCadenceForPreflight() noexcept;

    // Stage 5H: prepare the same verified 48-cycle D-D-D-S silence cadence as
    // a circular immutable ring. RUN, interrupts, and refill remain disabled
    // until the FF800 engine has been started by the protocol layer.
    kern_return_t PrepareBoundedCircularSilenceCadenceForPreflight() noexcept;

    // Stage 5H: run the immutable circular ring for a strictly bounded window,
    // prove repeated wraps from anchor-descriptor timestamp changes, reject
    // DEAD/ACTIVE loss, and stop synchronously.
    kern_return_t RunPreparedBoundedCircularSilenceCadenceForPreflight(
        uint32_t durationMs) noexcept;

    // Stage 5H unconditional teardown seam used on success and every partial
    // failure before the protocol stops the device and releases IRM.
    kern_return_t CleanupBoundedCircularSilenceCadenceForPreflight() noexcept;
    void Stop() noexcept;
    
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
    ASFW::IsochTransport::TxPacketMeta* metadataRing_{nullptr};
    ASFW::IsochTransport::TxStreamControl* controlBlock_{nullptr};

    Tx::TxPayloadDmaMap payloadDmaMap_{};
    OSSharedPtr<IODMACommand> payloadDmaCmd_{nullptr};

    Tx::IsochTxDmaRing::FiniteSilenceCadenceProgram
        finiteCadenceProgram_{};
    bool finiteCadencePrepared_{false};

    Tx::IsochTxDmaRing::BoundedCircularSilenceCadenceProgram
        boundedCircularCadenceProgram_{};
    bool boundedCircularCadencePrepared_{false};
};

} // namespace Isoch
} // namespace ASFW
