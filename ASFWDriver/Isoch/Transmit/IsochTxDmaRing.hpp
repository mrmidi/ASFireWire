// IsochTxDmaRing.hpp
// ASFW - Low-level OHCI IT DMA ring engine (generic, no audio semantics).

#pragma once

#include "IsochTxDescriptorSlab.hpp"
#include "IsochTxLayout.hpp"
#include "TxPayloadDmaMap.hpp"

#include "../Core/IsochEventGroup.hpp"
#include "../Core/IsochTxQueue.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/BarrierUtils.hpp"
#include "../../Common/FWTypes.hpp"

#include <atomic>
#include <array>
#include <cstdint>

namespace ASFW::Isoch::Tx {

struct TxPacketRequest final {
    uint32_t transmitCycle{0};
    uint32_t packetIndex{0};
    uint16_t hwTimestamp{0};
};

class IsochTxDmaRing final {
public:
    using OHCIDescriptor = Async::HW::OHCIDescriptor;
    using OHCIDescriptorImmediate = Async::HW::OHCIDescriptorImmediate;

    struct Counters {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> exitNotRunning{0};
        std::atomic<uint64_t> exitDead{0};
        std::atomic<uint64_t> exitDecodeFail{0};
        std::atomic<uint64_t> exitHwOOB{0};
        std::atomic<uint64_t> refills{0};
        std::atomic<uint64_t> packetsRefilled{0};
        std::atomic<uint64_t> fatalPacketSize{0};
        std::atomic<uint64_t> fatalPayloadMapping{0};
        std::atomic<uint64_t> fatalDescriptorBounds{0};
        std::atomic<uint64_t> txUnderruns{0};

        // High-water mark of a single coalesced completion. Content consumers
        // own the policy that decides whether this is an unsafe cadence.
        std::atomic<uint32_t> maxDeltaConsumed{0};

        // DMA ring gap monitoring: mapped packets the hardware had not
        // completed yet when a refill observed it (its remaining runway).
        std::atomic<uint32_t> lastDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint32_t> minDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint64_t> criticalGapEvents{0};
        // The hardware finished every mapped packet before a refill ran: the
        // finite queue ran dry and the stream is stopped.
        std::atomic<uint64_t> mappedRegionExhausted{0};
    };

    struct PrimeStats {
        uint64_t packetsAssembled{0};
        uint32_t firstCommandPointer{0};
    };

    enum class RefillFailureReason : uint8_t {
        None = 0,
        InvalidSharedContract,
        DeadContext,
        ProducerFaultStatus,
        CommandPointerDecode,
        UncommittedSlot,
        InvalidPacketSize,
        InvalidOperation,
        PayloadMapping,
        /// The hardware finished every mapped packet before this refill: the
        /// finite queue ran dry, so its packets are no longer on their
        /// cycles. Only a restart recovers.
        MappedRegionExhausted,
    };

    [[nodiscard]] static const char* RefillFailureReasonName(
        RefillFailureReason reason) noexcept;

    struct RefillOutcome {
        bool ok{false};
        bool dead{false};
        bool decodeFailed{false};
        bool hwOOB{false};
        RefillFailureReason failureReason{RefillFailureReason::None};
        uint32_t contextControl{0};
        uint32_t streamStatus{0};
        uint32_t hwPacketIndex{0};
        uint32_t cmdPtr{0};
        uint32_t cmdAddr{0};
        uint64_t failurePacketAbs{0};
        uint32_t failureSlot{0};
        uint32_t failurePayloadLength{0};
        uint16_t hwTimestamp{0};
        uint32_t completedPacketIndex{0};
        uint32_t completedPacketCount{0};
        uint32_t firstRefillPacket{0};
        uint32_t refillPacketCount{0};
        uint64_t packetsFilled{0};
        uint64_t refillRequestGeneration{0};
    };

    IsochTxDmaRing() noexcept = default;

    void SetChannel(uint8_t channel) noexcept { channel_ = channel; }

    /// Wire speed for every packet this ring transmits. It is the speed the
    /// device link was charged for against BANDWIDTH_AVAILABLE, so the two must
    /// come from the same source.
    void SetSpeed(FW::FwSpeed speed) noexcept { speed_ = speed; }

    [[nodiscard]] bool HasRings() const noexcept { return slab_.IsValid(); }

    [[nodiscard]] kern_return_t SetupRings(Memory::IIsochDMAMemory& dmaMemory) noexcept {
        dmaMemory_ = &dmaMemory;
        return slab_.AllocateAndInitialize(dmaMemory);
    }

    void ResetForStart() noexcept;

    void SeedCycleTracking(Driver::HardwareInterface& hw) noexcept;

    void DebugFillDescriptorSlab(uint8_t pattern) noexcept { slab_.DebugFillDescriptorSlab(pattern); }

    [[nodiscard]] PrimeStats Prime(const TxPayloadDmaMap& payloadDmaMap,
                                   uint32_t numSlots,
                                   uint32_t slotStrideBytes,
                                   const IsochTxPacketMeta* metadataRing,
                                   uint64_t preFillCount) noexcept;

    [[nodiscard]] RefillOutcome Refill(Driver::HardwareInterface& hw,
                                       uint8_t contextIndex,
                                       IsochTxPacketMeta* metadataRing,
                                       IsochTxQueueControl* controlBlock,
                                       uint32_t numSlots,
                                       uint8_t* payloadBase,
                                       const TxPayloadDmaMap& payloadDmaMap) noexcept;

    /// WAKE after the queue was extended, even when the context reads active.
    /// OHCI 1.2 draft §3.1.1.2: "If the Host Controller is not at the end of
    /// the list then no action is taken when ContextControl.wake is set."
    /// §3.1.1.3 advisory note (written for a receive context that ran out of
    /// buffers): "if software appends a new descriptor and sets the
    /// ContextControl.wake bit, the DMA will correctly process it regardless
    /// of the state of the ContextControl.active bit." Linux
    /// ohci_flush_queue_iso always wakes too (ohci.c:3471-3476).
    /// Never wakes a stopped or dead context.
    bool WakeHardware(Driver::HardwareInterface& hw, uint8_t contextIndex) noexcept;

    // Debug helpers (delegated by IsochTransmitContext)
    void DumpAtCmdPtr(Driver::HardwareInterface& hw, uint8_t contextIndex) const noexcept;
    void DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept;

    [[nodiscard]] const Counters& RTCounters() const noexcept { return counters_; }
    [[nodiscard]] uint32_t LastHwTimestamp() const noexcept { return lastHwTimestamp_; }

    // Expose slab for audio injection.
    [[nodiscard]] IsochTxDescriptorSlab& Slab() noexcept { return slab_; }
    [[nodiscard]] const IsochTxDescriptorSlab& Slab() const noexcept { return slab_; }

private:
    /// Packets the controller completed from `completedAbsIdx` on, read from
    /// OUTPUT_LAST xferStatus, never from the CommandPtr, and bounded by
    /// `mappedLimit` (packets mapped and not yet seen complete).
    [[nodiscard]] uint32_t CountCompletedPackets(uint64_t completedAbsIdx,
                                                 uint64_t mappedLimit) noexcept;
    void UpdateGapCounters(uint32_t gap) noexcept;
    void ResyncCycleTracking(uint32_t newestCompletedSlot,
                             uint32_t completedCount,
                             RefillOutcome& out) noexcept;
    void CommitRefill(uint32_t toFill) noexcept;
    [[nodiscard]] bool DecodeHardwarePacketIndex(uint32_t cmdPtr,
                                                 uint32_t& outPacketIndex) noexcept;

    uint8_t channel_{0};
    FW::FwSpeed speed_{FW::FwSpeed::S400};
    IsochTxDescriptorSlab slab_{};
    Memory::IIsochDMAMemory* dmaMemory_{nullptr};

    // Absolute index of the next packet to map; packets [completion,
    // softwareFillAbsIdx_) are mapped and not yet seen complete.
    uint64_t softwareFillAbsIdx_{0};

    // Isoch cycle tracking for packet timing
    uint32_t nextTransmitCycle_{0};
    bool cycleTrackingValid_{false};
    uint32_t lastHwTimestamp_{0};

    Counters counters_{};
};

} // namespace ASFW::Isoch::Tx
