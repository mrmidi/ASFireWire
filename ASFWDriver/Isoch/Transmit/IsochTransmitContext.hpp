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

#include "../Core/IsochProgressMonitor.hpp"
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

    // Terminal transport fault notification. Fired once per fatal stop, after
    // the terminal state is published to the seam, so the consumer above the
    // seam learns that TX is dead instead of discovering it whenever it next
    // happens to stop the stream. Before this existed, the 2026-08-25 stall
    // left the consumer believing the stream was running for 63 s after the
    // context had already stopped.
    //
    // CONTRACT: this fires on the isoch watchdog/poll thread, which also
    // carries the RX drain. The callee MUST NOT block: it must hand the fault
    // to another queue and return. See AudioCoordinator::HandleTxTransportFault
    // for the hop and for why the two cheaper wirings were rejected.
    using TxTransportFaultCallback =
        std::function<void(uint32_t statusRaw, uint64_t streamGeneration)>;

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
    void SetTxTransportFaultCallback(TxTransportFaultCallback callback) noexcept;

    State GetState() const noexcept { return state_; }
    
    uint64_t PacketsAssembled() const noexcept { return packetsAssembled_; }
    
    void LogStatistics() const noexcept;
    void DumpDescriptorRing(uint32_t startPacket = 0, uint32_t numPackets = 8) const noexcept;

#ifdef ASFW_HOST_TEST
    void SetProgressThresholdsForTesting(
        Core::IsochProgressThresholds thresholds) noexcept;
#endif

private:
    void WakeHardware() noexcept;
    void DoRefillOnce(uint64_t eventHostTicks, bool publishTimingEvent) noexcept;
    void ObserveTransportProgress(
        const Tx::IsochTxDmaRing::RefillOutcome& outcome,
        uint64_t eventHostTicks) noexcept;
    void StopImmediatelyForTxFault(
        IsochTxQueueStatus status = IsochTxQueueStatus::kDeadContext) noexcept;

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

    // Ticks of interrupt silence per diagnostic "kick". The watchdog runs at
    // kAsyncWatchdogPeriodUsec (1 ms) and interrupts arrive every
    // kPacketsPerInterrupt packets (750 us at 6), so a single silent tick means
    // nothing: hysteresis is required before *reporting*. Refill deliberately
    // does NOT wait for it — see Poll().
    static constexpr uint32_t kIrqStallTicksPerKick = 5;

    // Consecutive watchdog kicks with zero interrupts observed.
    //
    // This used to fatal the context at 16 kicks (~80 ms). It no longer does.
    // Interrupt silence is not the same fault as no DMA progress, and the two
    // were conflated:
    //
    //   * The watchdog carries the stream. Poll() now refills on EVERY silent
    //     tick (1 ms) rather than every 5th, so the refill cadence is well
    //     inside the descriptor ring's drain time and stale-lap re-transmission
    //     — the 2026-07-19 all-zero-stream failure this fatal was added for —
    //     cannot occur through mere interrupt loss.
    //   * Genuine stalls are still caught, independently and by direct
    //     observation rather than inference: IsochProgressMonitor fatals at
    //     kProgressFatalAfterNanos (100 ms) when the DMA cursor stops advancing,
    //     with kTransportProgressStall. That is the honest backstop; it watches
    //     progress, not interrupts.
    //
    // Per TX-IRQ-001 the interrupt path can wedge node-wide while the device and
    // the chip stay healthy, and killing the stream there loses audio for a
    // condition we can carry. Report it loudly instead: this threshold now
    // raises a one-shot error and a counter, and the stream keeps running.
    // See documentation/TX_IRQ_001_INTERRUPT_STALL.md sections 8 and 10.
    static constexpr uint32_t kIrqSilentKickErrorThreshold = 16;
    uint32_t irqSilentKickStreak_{0};
    //: One-shot latch so a wedged path logs once, not 1000x/s.
    bool irqSilenceReported_{false};
    //: Times the error threshold was crossed, and ticks spent carrying the
    //: stream without interrupts. Both surface in LogStatistics().
    std::atomic<uint64_t> irqSilenceEvents_{0};
    std::atomic<uint64_t> irqCarriedTicks_{0};
    /// Previous values at the last statistics line, so it can report a rate
    /// rather than a monotone total. Mutable because LogStatistics is const and
    /// this is bookkeeping for the log, not observable state.
    mutable uint64_t lastLoggedCarriedTicks_{0};
    mutable uint64_t lastLoggedPollTicks_{0};

    // Interrupt-delivery re-arm attempts, taken before the fatal above.
    //
    // The observed 2026-08-25 stall left IntEvent with isochTx|isochRx|RQPkt
    // latched, IntMask master-enabled, and no handler entry: events pending,
    // delivery dead. The host controller is an Agere FW643 (PCI 0x11C1/0x5901)
    // driven by MSI ("IOPCIMSIMode" = Yes), and MSI is a message per interrupt
    // condition rather than a level: a message that is lost, or never
    // re-asserted after the handler cleared IntEvent, is never retried by the
    // hardware. Toggling masterIntEnable off and on rebuilds the 0->1
    // interrupt condition over the still-latched events, which should emit a
    // fresh message.
    //
    // This is NOT taken from a reference stack. Linux's answer for this chip is
    // QUIRK_NO_MSI (ohci.c:344) — fall back to level-triggered INTx — which
    // Apple Silicon does not offer, and Apple's own AppleFWOHCI predates it.
    // Treat the re-arm as an unproven, hardware-validated-only remedy: it is
    // cheap, it cannot lose latched events (IntEvent latches regardless of
    // mask), and the fatal below still backstops it.
    static constexpr uint32_t kIrqSilentFirstReArmKick = 2;
    static constexpr uint32_t kIrqSilentSecondReArmKick = 8;
    std::atomic<uint64_t> irqReArmAttempts_{0};
    std::atomic<uint64_t> irqReArmRecoveries_{0};
    bool irqReArmPendingOutcome_{false};

    // A callback is not DMA progress. This independent cursor monitor catches
    // stale/repeated IT events and watchdog refills whose CommandPtr retires no
    // packet. Thresholds are wall-clock durations converted once at Start().
    static constexpr uint64_t kProgressWakeAfterNanos = 4'000'000;
    static constexpr uint64_t kProgressSnapshotAfterNanos = 20'000'000;
    static constexpr uint64_t kProgressFatalAfterNanos = 100'000'000;
    Core::IsochProgressMonitor progressMonitor_{};
    std::atomic<uint64_t> progressSnapshots_{0};
    std::atomic<uint64_t> progressWakeAttempts_{0};
    std::atomic<uint64_t> progressWakeSuccesses_{0};
    std::atomic<uint64_t> progressFatalStops_{0};

    // Refill Latency Histogram (buckets: <50us, 50-200us, 200-500us, >500us)
    std::atomic<uint64_t> latencyBucket0_{0};
    std::atomic<uint64_t> latencyBucket1_{0};
    std::atomic<uint64_t> latencyBucket2_{0};
    std::atomic<uint64_t> latencyBucket3_{0};
    std::atomic<uint32_t> maxRefillLatencyUs_{0};
    std::atomic<uint64_t> irqWatchdogKicks_{0};
    TxPreparationCallback txPreparationCallback_{};
    TxTransportFaultCallback txTransportFaultCallback_{};

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
