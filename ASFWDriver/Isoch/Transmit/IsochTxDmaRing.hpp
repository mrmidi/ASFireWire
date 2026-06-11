// IsochTxDmaRing.hpp
// ASFW - Low-level OHCI IT DMA ring engine (generic, no audio semantics).

#pragma once

#include "IsochTxDescriptorSlab.hpp"
#include "IsochTxLayout.hpp"

#include "../Core/IsochEventGroup.hpp"
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
        std::atomic<uint64_t> fatalDescriptorBounds{0};

        // DMA ring gap monitoring
        std::atomic<uint32_t> lastDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint32_t> minDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint64_t> criticalGapEvents{0};
    };

    struct PrimeStats {
        uint64_t packetsAssembled{0};
    };

    struct RefillOutcome {
        bool ok{false};
        bool dead{false};
        bool decodeFailed{false};
        bool hwOOB{false};
        uint32_t hwPacketIndex{0};
        uint32_t cmdPtr{0};
        uint32_t cmdAddr{0};
        uint16_t hwTimestamp{0};
        uint32_t completedPacketIndex{0};
        uint32_t completedPacketCount{0};
        uint32_t firstRefillPacket{0};
        uint32_t refillPacketCount{0};
        uint64_t packetsFilled{0};
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

    [[nodiscard]] PrimeStats Prime() noexcept;

    [[nodiscard]] RefillOutcome Refill(Driver::HardwareInterface& hw,
                                       uint8_t contextIndex) noexcept;

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
    [[nodiscard]] static uint32_t BuildIsochHeaderQ0(uint8_t channel) noexcept;
    [[nodiscard]] uint32_t ComputeDeltaConsumed(uint32_t hwPacketIndex) noexcept;
    void UpdateGapCounters(uint32_t gap) noexcept;
    void ResyncCycleTracking(Driver::HardwareInterface& hw,
                             uint32_t hwPacketIndex,
                             uint32_t deltaConsumed,
                             RefillOutcome& out) noexcept;
    void CommitRefill(uint32_t toFill) noexcept;
    [[nodiscard]] bool DecodeHardwarePacketIndex(Driver::HardwareInterface& hw,
                                                 uint8_t contextIndex,
                                                 uint32_t& outPacketIndex,
                                                 uint32_t& outCmdPtr) noexcept;

    uint8_t channel_{0};
    IsochTxDescriptorSlab slab_{};
    Memory::IIsochDMAMemory* dmaMemory_{nullptr};

    // Fill-ahead tracking
    uint32_t softwareFillIndex_{0};
    uint32_t lastHwPacketIndex_{0};
    uint32_t ringPacketsAhead_{0};

    // Cycle tracking for SYT generation
    uint32_t nextTransmitCycle_{0};
    bool cycleTrackingValid_{false};
    uint32_t lastHwTimestamp_{0};

    Counters counters_{};
};

} // namespace ASFW::Isoch::Tx
