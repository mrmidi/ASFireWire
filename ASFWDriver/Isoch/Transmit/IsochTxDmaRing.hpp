// IsochTxDmaRing.hpp
// ASFW - Low-level OHCI IT DMA ring engine (generic, no audio semantics).

#pragma once

#include "IsochTxDescriptorSlab.hpp"
#include "IsochTxLayout.hpp"

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/BarrierUtils.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Isoch::Tx {

struct IsochTxPacket final {
    const uint32_t* words{nullptr};   // Host-order words as stored in DMA payload buffer
    uint32_t sizeBytes{0};
    bool isData{false};
    uint8_t dbc{0};
};

class IIsochTxPacketProvider {
public:
    virtual ~IIsochTxPacketProvider() = default;
    [[nodiscard]] virtual IsochTxPacket NextSilentPacket(uint32_t transmitCycle) noexcept = 0;
};

class IIsochTxAudioInjector {
public:
    virtual ~IIsochTxAudioInjector() = default;
    virtual void InjectNearHw(uint32_t hwPacketIndex,
                              IsochTxDescriptorSlab& slab) noexcept = 0;
};

class IsochTxCaptureHook {
public:
    virtual ~IsochTxCaptureHook() = default;
    virtual void CaptureBeforeOverwrite(uint32_t packetIndex,
                                        uint32_t hwPacketIndexCmdPtr,
                                        uint32_t cmdPtr,
                                        const Async::HW::OHCIDescriptor* lastDesc,
                                        const uint32_t* payload32) noexcept = 0;
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
        uint64_t dataPackets{0};
        uint64_t noDataPackets{0};
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
        uint64_t packetsFilled{0};
        uint64_t dataPackets{0};
        uint64_t noDataPackets{0};
    };

    IsochTxDmaRing() noexcept = default;

    void SetChannel(uint8_t channel) noexcept { channel_ = channel; }

    [[nodiscard]] bool HasRings() const noexcept { return slab_.IsValid(); }

    [[nodiscard]] kern_return_t SetupRings(Memory::IIsochDMAMemory& dmaMemory) noexcept {
        return slab_.AllocateAndInitialize(dmaMemory);
    }

    void ResetForStart() noexcept;

    void SeedCycleTracking(Driver::HardwareInterface& hw) noexcept;

    void DebugFillDescriptorSlab(uint8_t pattern) noexcept { slab_.DebugFillDescriptorSlab(pattern); }

    [[nodiscard]] PrimeStats Prime(IIsochTxPacketProvider& provider) noexcept;

    [[nodiscard]] RefillOutcome Refill(Driver::HardwareInterface& hw,
                                       uint8_t contextIndex,
                                       IIsochTxPacketProvider& provider,
                                       IsochTxCaptureHook* captureHook,
                                       IIsochTxAudioInjector* injector) noexcept;

    void WakeHardwareIfIdle(Driver::HardwareInterface& hw, uint8_t contextIndex) noexcept;

    // Debug helpers (delegated by IsochTransmitContext)
    void DumpAtCmdPtr(Driver::HardwareInterface& hw, uint8_t contextIndex) const noexcept;
    void DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept;
    void DumpPayloadBuffers(uint32_t numPackets) const noexcept;

    [[nodiscard]] const Counters& RTCounters() const noexcept { return counters_; }
    [[nodiscard]] uint32_t LastHwTimestamp() const noexcept { return lastHwTimestamp_; }

    // Expose slab for audio injection.
    [[nodiscard]] IsochTxDescriptorSlab& Slab() noexcept { return slab_; }
    [[nodiscard]] const IsochTxDescriptorSlab& Slab() const noexcept { return slab_; }

private:
    [[nodiscard]] static uint32_t BuildIsochHeaderQ0(uint8_t channel) noexcept;

    uint8_t channel_{0};
    IsochTxDescriptorSlab slab_{};

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

