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

        // Full descriptor-ring laps of unaccounted hardware progress between
        // two refills (either free-running stale re-transmission or a brief
        // context stall). The completion cursor is reconciled forward so
        // producer pacing stays on true bus time instead of dilating.
        std::atomic<uint64_t> lapLossEvents{0};
        std::atomic<uint64_t> lapLossPacketsTotal{0};

        // Unaccounted progress exceeded the producer's committed lead: the
        // context stalled or wedged beyond anything reconciliation can cover
        // (2026-07-19 Saffire freeze). Fatal; the stream must stop honestly.
        std::atomic<uint64_t> contextStallFatals{0};

        // DMA ring gap monitoring
        std::atomic<uint32_t> lastDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint32_t> minDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint64_t> criticalGapEvents{0};

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
        ContextStalled,
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

    // The IT descriptor ring is circular and free-running: hardware keeps
    // executing it whether or not software refilled, so per-refill progress
    // measured from the command pointer is only known modulo kNumPackets.
    // The OUTPUT_LAST completion timestamps (3-bit seconds + 13-bit cycle,
    // OHCI §9.4.2) recover true progress: the context transmits exactly one
    // packet per cycle, so cycles elapsed between the last-processed packets
    // of two refills equals packets truly consumed. Any surplus over the
    // modulo delta is whole ring laps of unaccounted execution: free-running
    // stale re-transmission, or a stalled context whose sparse crawl stamps
    // late completion timestamps (2026-07-19 Saffire freeze — the wire showed
    // 13 packets while the timestamps implied 64k). The two are
    // indistinguishable here; the caller classifies by whether the producer's
    // committed lead can cover the jump. Rounding to whole laps absorbs
    // single-cycle jitter (missed cycle / cycle-master correction). Valid for
    // refill gaps below the 8-second timestamp wrap; the transmit context
    // faults out long before that horizon.
    [[nodiscard]] static constexpr uint32_t ComputeLostLapPackets(
        uint16_t previousTimestamp,
        uint16_t currentTimestamp,
        uint32_t deltaConsumed) noexcept {
        constexpr uint32_t kCyclesPerSecond = 8000u;
        constexpr uint32_t kTimestampDomainCycles = 8u * kCyclesPerSecond;
        const uint32_t previousOrdinal =
            ((previousTimestamp >> 13) & 0x7u) * kCyclesPerSecond +
            ((previousTimestamp & 0x1FFFu) % kCyclesPerSecond);
        const uint32_t currentOrdinal =
            ((currentTimestamp >> 13) & 0x7u) * kCyclesPerSecond +
            ((currentTimestamp & 0x1FFFu) % kCyclesPerSecond);
        const uint32_t elapsedCycles =
            (currentOrdinal + kTimestampDomainCycles - previousOrdinal) %
            kTimestampDomainCycles;
        if (elapsedCycles <= deltaConsumed) {
            return 0;
        }
        const uint32_t surplus = elapsedCycles - deltaConsumed;
        const uint32_t lostLaps =
            (surplus + Layout::kNumPackets / 2) / Layout::kNumPackets;
        return lostLaps * Layout::kNumPackets;
    }

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
    [[nodiscard]] uint32_t DetectLostLapPackets(uint32_t hwPacketIndex,
                                                uint32_t deltaConsumed) noexcept;
    [[nodiscard]] bool DecodeHardwarePacketIndex(Driver::HardwareInterface& hw,
                                                 uint8_t contextIndex,
                                                 uint32_t& outPacketIndex,
                                                 uint32_t& outCmdPtr) noexcept;

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

    // Previous refill's last-processed completion timestamp; anchors the
    // true-cycle lap-loss detection across refills.
    uint16_t lastLapTimestamp_{0};
    bool lapTimestampValid_{false};

    Counters counters_{};
};

} // namespace ASFW::Isoch::Tx
