// IsochTxDmaRing.hpp
// ASFW - Low-level OHCI IT DMA ring engine (generic, no audio semantics).

#pragma once

#include "IsochTxDescriptorSlab.hpp"
#include "IsochTxLayout.hpp"
#include "TxPayloadHash.hpp"

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

struct IsochTxPacket final {
    const uint32_t* words{nullptr};   // Host-order words as stored in DMA payload buffer
    uint32_t sizeBytes{0};
    bool isData{false};
    uint8_t dbc{0};
    uint16_t syt{0xFFFF};
    uint32_t framesPerPacket{0};
    uint64_t timelineFirstFrame{0};
};

struct TxPacketRequest final {
    uint32_t transmitCycle{0};
    uint32_t packetIndex{0};
    uint16_t hwTimestamp{0};
    uint64_t slotGeneration{0};
};

class IIsochTxPacketProvider {
public:
    virtual ~IIsochTxPacketProvider() = default;
    [[nodiscard]] virtual IsochTxPacket NextTransmitPacket(const TxPacketRequest& request) noexcept = 0;
};

class IIsochTxAudioInjector {
public:
    virtual ~IIsochTxAudioInjector() = default;
    virtual void InjectNearHw(uint32_t hwPacketIndex,
                              IsochTxDescriptorSlab& slab) noexcept = 0;
};

enum class PreparedTxSlotState : uint8_t {
    InitialSilence = 0,
    PendingSource,
    PcmPrepared,
    RetiredEpochSilence,
    Completed,
};

enum class PreparedTxAction : uint8_t {
    NoChange = 0,
    Pending,
    Prepared,
    RetiredSilence,
    Fatal,
};

struct PreparedTxSlotMetadata final {
    uint64_t generation{0};
    uint64_t timelineFirstFrame{0};
    uint64_t sourceFirstFrame{0};
    uint64_t sourceEndFrame{0};
    uint64_t epoch{0};
    uint64_t preparationHostTicks{0};
    uint64_t preparedPayloadHash{0};
    uint32_t sizeBytes{0};
    uint32_t framesPerPacket{0};
    uint32_t preparationDistance{0};
    int32_t firstSourceSamples[2]{};
    uint32_t firstEncodedWords[2]{};
    uint8_t dbc{0};
    uint16_t syt{0xFFFF};
    bool valid{false};
    bool isData{false};
    PreparedTxSlotState state{PreparedTxSlotState::InitialSilence};
};

struct PreparedTxPayloadRequest final {
    uint32_t packetIndex{0};
    uint32_t hwPacketIndex{0};
    uint32_t distanceToHardware{0};
    bool writable{false};
    bool deadline{false};
    bool hardwareOwned{false};
    PreparedTxSlotMetadata metadata{};
    uint8_t* payloadBytes{nullptr};
    uint32_t payloadCapacityBytes{0};
};

struct PreparedTxPayloadResult final {
    PreparedTxAction action{PreparedTxAction::NoChange};
    uint64_t sourceFirstFrame{0};
    uint64_t sourceEndFrame{0};
    uint64_t epoch{0};
    int32_t firstSourceSamples[2]{};
    uint32_t firstEncodedWords[2]{};
};

struct CompletedTxSlot final {
    PreparedTxSlotMetadata metadata{};
    uint64_t completedPayloadHash{0};
    uint32_t packetIndex{0};
    uint32_t hwPacketIndex{0};
    bool payloadHashMatches{true};
};

class IIsochTxCompletionObserver {
public:
    virtual ~IIsochTxCompletionObserver() = default;
    [[nodiscard]] virtual bool OnTransmitSlotCompleted(
        const CompletedTxSlot& completed) noexcept = 0;
};

class IIsochTxPayloadPreparer {
public:
    virtual ~IIsochTxPayloadPreparer() = default;
    [[nodiscard]] virtual PreparedTxPayloadResult PreparePayload(
        const PreparedTxPayloadRequest& request) noexcept = 0;
};

class IsochTxCaptureHook {
public:
    virtual ~IsochTxCaptureHook() = default;
    virtual void CaptureBeforeOverwrite(uint32_t packetIndex,
                                        uint32_t hwPacketIndexCmdPtr,
                                        uint32_t cmdPtr,
                                        const Async::HW::OHCIDescriptor* lastDesc,
                                        const uint32_t* payload32,
                                        const PreparedTxSlotMetadata& metadata) noexcept = 0;
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
        std::atomic<uint64_t> preparedPayloads{0};
        std::atomic<uint64_t> pendingPayloads{0};
        std::atomic<uint64_t> retiredSilencePayloads{0};
        std::atomic<uint64_t> preparationFaults{0};
        std::atomic<uint64_t> ownershipFaults{0};
        std::atomic<uint64_t> completedPayloadHashMatches{0};
        std::atomic<uint64_t> completedPayloadHashMismatches{0};

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
        uint32_t completedPacketIndex{0};
        uint32_t completedPacketCount{0};
        uint32_t firstRefillPacket{0};
        uint32_t refillPacketCount{0};
        uint64_t packetsFilled{0};
        uint64_t dataPackets{0};
        uint64_t noDataPackets{0};
        Core::IsochEventGroup eventGroup{};
    };

    struct PreparationOutcome {
        bool ok{false};
        bool fatal{false};
        bool decodeFailed{false};
        bool hwOOB{false};
        uint32_t hwPacketIndex{0};
        uint32_t preparedCount{0};
        uint32_t pendingCount{0};
        uint32_t startupSilenceCount{0};
        uint32_t retiredSilenceCount{0};
        uint32_t faultPacketIndex{0};
        uint32_t faultDistance{0};
        PreparedTxSlotMetadata faultMetadata{};
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

    [[nodiscard]] PrimeStats Prime(IIsochTxPacketProvider& provider) noexcept;

    [[nodiscard]] RefillOutcome Refill(Driver::HardwareInterface& hw,
                                       uint8_t contextIndex,
                                       IIsochTxPacketProvider& provider,
                                       IsochTxCaptureHook* captureHook,
                                       IIsochTxAudioInjector* injector,
                                       IIsochTxCompletionObserver* completionObserver = nullptr) noexcept;

    [[nodiscard]] PreparationOutcome PreparePayloads(
        Driver::HardwareInterface& hw,
        uint8_t contextIndex,
        IIsochTxPayloadPreparer& preparer) noexcept;

    void WakeHardwareIfIdle(Driver::HardwareInterface& hw, uint8_t contextIndex) noexcept;

    // Debug helpers (delegated by IsochTransmitContext)
    void DumpAtCmdPtr(Driver::HardwareInterface& hw, uint8_t contextIndex) const noexcept;
    void DumpDescriptorRing(uint32_t startPacket, uint32_t numPackets) const noexcept;
    void DumpPayloadBuffers(uint32_t numPackets) const noexcept;

    [[nodiscard]] const Counters& RTCounters() const noexcept { return counters_; }
    [[nodiscard]] uint32_t LastHwTimestamp() const noexcept { return lastHwTimestamp_; }
    [[nodiscard]] const PreparedTxSlotMetadata& SlotMetadata(uint32_t packetIndex) const noexcept {
        return slotMetadata_[packetIndex % Layout::kNumPackets];
    }

    // Expose slab for audio injection.
    [[nodiscard]] IsochTxDescriptorSlab& Slab() noexcept { return slab_; }
    [[nodiscard]] const IsochTxDescriptorSlab& Slab() const noexcept { return slab_; }

private:
    [[nodiscard]] static uint32_t BuildIsochHeaderQ0(uint8_t channel) noexcept;
    static void CopyPacketPayload(uint8_t* payloadVirt, const IsochTxPacket& pkt) noexcept;
    [[nodiscard]] uint32_t ComputeDeltaConsumed(uint32_t hwPacketIndex) noexcept;
    void UpdateGapCounters(uint32_t gap) noexcept;
    void ResyncCycleTracking(Driver::HardwareInterface& hw,
                             uint32_t hwPacketIndex,
                             uint32_t deltaConsumed,
                             RefillOutcome& out) noexcept;
    [[nodiscard]] bool ReportCompletedSlots(uint32_t hwPacketIndex,
                                            uint32_t completedPacketCount,
                                            IIsochTxCompletionObserver* observer,
                                            RefillOutcome& out) noexcept;
    [[nodiscard]] bool RefillPacket(uint32_t pktIdx,
                                    uint32_t hwPacketIndex,
                                    uint32_t cmdPtr,
                                    IIsochTxPacketProvider& provider,
                                    IsochTxCaptureHook* captureHook,
                                    RefillOutcome& out) noexcept;
    void CommitRefill(uint32_t toFill) noexcept;
    [[nodiscard]] bool DecodeHardwarePacketIndex(Driver::HardwareInterface& hw,
                                                 uint8_t contextIndex,
                                                 uint32_t& outPacketIndex,
                                                 uint32_t& outCmdPtr) noexcept;
    void InitializeSlotMetadata(uint32_t packetIndex,
                                uint64_t generation,
                                const IsochTxPacket& packet) noexcept;

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

    std::array<PreparedTxSlotMetadata, Layout::kNumPackets> slotMetadata_{};
    std::array<uint64_t, Layout::kNumPackets> slotGenerations_{};

    Counters counters_{};
};

} // namespace ASFW::Isoch::Tx
