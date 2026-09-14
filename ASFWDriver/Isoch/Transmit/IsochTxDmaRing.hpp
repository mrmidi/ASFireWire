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

        // DMA ring gap monitoring
        std::atomic<uint32_t> lastDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint32_t> minDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint64_t> criticalGapEvents{0};
        // Hardware-ring laps: the refill ran a full descriptor ring (or more)
        // late, the command-pointer delta aliased modulo the ring size, and
        // the ring re-sent stale descriptors. Each lap is realigned by
        // skipping kNumPackets producer packets so packet index and bus cycle
        // stay in step instead of the whole stream slipping a ring later.
        std::atomic<uint64_t> ringLaps{0};
        std::atomic<uint64_t> ringLapPacketsSkipped{0};
        // Cycles the hardware spent without advancing a packet (the OMI skip
        // address re-sending a packet after a lost cycle). Counted only.
        std::atomic<uint64_t> lostCycles{0};
        // Stamp readings the lap detector refused (see DetectRingLaps): not
        // fresh against the cycle timer, older than the packets counted since
        // the baseline, an implausible lap count, or a lap the producer had
        // not committed far enough to realign.
        std::atomic<uint64_t> staleStampReads{0};
        std::atomic<uint64_t> inconsistentStampReads{0};
        std::atomic<uint64_t> implausibleLapReads{0};
        std::atomic<uint64_t> unrealignableLaps{0};
        // The last completed descriptor's word was not fresh (still in
        // flight) and the reading came from the one before it instead.
        std::atomic<uint64_t> inFlightFallbacks{0};

    };

    struct PrimeStats {
        uint64_t packetsAssembled{0};
    };

    enum class RefillFailureReason : uint8_t {
        None = 0,
        InvalidSharedContract,
        DeadContext,
        ProducerFaultStatus,
        CommandPointerDecode,
        UncommittedSlot,
        InvalidPacketSize,
        PayloadMapping,
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
        uint32_t ringLaps{0};
        uint32_t lapPacketsSkipped{0};
        uint32_t lostCycles{0};
    };

    IsochTxDmaRing() noexcept = default;

    void SetChannel(uint8_t channel) noexcept { channel_ = channel; }

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

    void WakeHardwareIfIdle(Driver::HardwareInterface& hw, uint8_t contextIndex) noexcept;

    // Debug helpers (delegated by IsochTransmitContext)
    void DumpAtCmdPtr(Driver::HardwareInterface& hw, uint8_t contextIndex) const noexcept;
    void DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept;

    [[nodiscard]] const Counters& RTCounters() const noexcept { return counters_; }
    [[nodiscard]] uint32_t LastHwTimestamp() const noexcept { return lastHwTimestamp_; }

    // Expose slab for audio injection.
    [[nodiscard]] IsochTxDescriptorSlab& Slab() noexcept { return slab_; }
    [[nodiscard]] const IsochTxDescriptorSlab& Slab() const noexcept { return slab_; }

private:
    [[nodiscard]] uint32_t ComputeDeltaConsumed(uint32_t hwPacketIndex) noexcept;
    void UpdateGapCounters(uint32_t gap) noexcept;
    void ResyncCycleTracking(Driver::HardwareInterface& hw,
                             uint32_t hwPacketIndex,
                             uint32_t deltaConsumed,
                             RefillOutcome& out) noexcept;
    void CommitRefill(uint32_t toFill) noexcept;
    [[nodiscard]] uint32_t DetectRingLaps(uint32_t hwPacketIndex,
                                          uint64_t completedAbsBefore,
                                          uint32_t rawDeltaConsumed,
                                          uint32_t cycleTimer32,
                                          uint32_t maxRealignLaps,
                                          uint32_t& outLostCycles) noexcept;
    [[nodiscard]] bool ReadCompletionCycle(uint32_t slot, uint32_t& outCycle13) noexcept;
    [[nodiscard]] bool DecodeHardwarePacketIndex(uint32_t cmdPtr,
                                                 uint32_t& outPacketIndex) noexcept;

    uint8_t channel_{0};
    IsochTxDescriptorSlab slab_{};
    Memory::IIsochDMAMemory* dmaMemory_{nullptr};

    // Fill-ahead tracking
    uint64_t softwareFillAbsIdx_{0};
    uint32_t lastHwPacketIndex_{0};
    uint32_t ringPacketsAhead_{0};

    // Isoch cycle tracking for packet timing
    uint32_t nextTransmitCycle_{0};
    bool cycleTrackingValid_{false};
    uint32_t lastHwTimestamp_{0};
    // Lap-detection baseline: the 13-bit cycle in which a completed
    // descriptor went out, and that descriptor's absolute packet index as the
    // (aliased) completion cursor counts it. Invalid until the first
    // completed descriptor is seen.
    uint32_t lastCompletionCycle_{0};
    uint64_t lastCompletionAbs_{0};
    bool haveLastCompletionCycle_{false};
    // A lap seen once; acted on only when the next reading agrees.
    uint32_t pendingLaps_{0};

    Counters counters_{};
};

} // namespace ASFW::Isoch::Tx
