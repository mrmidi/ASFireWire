// IsochTxDmaRing.hpp
// ASFW - Low-level OHCI IT DMA ring engine (generic, no audio semantics).

#pragma once

#include "IsochTxDescriptorSlab.hpp"
#include "IsochTxLayout.hpp"
#include "TxPayloadDmaMap.hpp"

#include "../Core/IsochEventGroup.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/BarrierUtils.hpp"
#include "../../Shared/Isoch/IsochAudioTransport.hpp"

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

        // High-water mark of a single refill's coalesced deltaConsumed. The
        // committed lead (kTxPreparationSlackPackets) must cover this or the
        // refill ISR holes (IT FATAL). Use it to size the slack empirically.
        std::atomic<uint32_t> maxDeltaConsumed{0};

        // DMA ring gap monitoring
        std::atomic<uint32_t> lastDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint32_t> minDmaGapPackets{Layout::kNumPackets};
        std::atomic<uint64_t> criticalGapEvents{0};

        // Wire-truth payload gauges, sampled at refill time — the last point
        // software sees the bytes the DMA engine will ship, after the
        // producer's commit. Independent of writer-side claims. An "info"
        // quadlet is any AM824 payload quadlet other than 0x00000000 and the
        // 0x80000000 idle MIDI/no-info slot word.
        std::atomic<uint64_t> wireDataPackets{0};
        std::atomic<uint64_t> wireZeroPcmPackets{0};
        std::atomic<uint64_t> wireInfoQuads{0};
        std::atomic<uint64_t> wirePcmDropouts{0};
        std::atomic<uint32_t> wireMaxAbs24{0};
        std::atomic<uint32_t> wireLastInfoQuad{0};
        std::atomic<uint64_t> wireFirstInfoAbsIdx{0};
    };

    struct PrimeStats {
        uint64_t packetsAssembled{0};
    };

    enum class RefillFailureReason : uint8_t {
        None = 0,
        InvalidSharedContract,
        DeadContext,
        ProducerFatalStatus,
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
        uint64_t preparationRequestGeneration{0};
        bool producerFailureAvailable{false};
        ASFW::IsochTransport::TxProducerFailureRecord producerFailure{};
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
                                   const ASFW::IsochTransport::TxPacketMeta* metadataRing,
                                   uint64_t preFillCount) noexcept;

    struct SingleSilencePacketProgram final {
        uint32_t packetSlot{0U};
        uint32_t producerSlot{0U};
        uint32_t commandPtr{0U};
        uint32_t payloadLength{0U};
    };

    struct SingleSilencePacketCompletion final {
        uint16_t transferStatus{0U};
        uint16_t timestamp{0U};
    };

    // Stage 5F safety seam: select one already-primed no-CIP FF800 data packet,
    // verify that its 1536-byte payload is entirely zero, terminate its normal
    // four-descriptor OHCI program, publish payload + descriptors, and return
    // the exact CommandPtr. This is the first preflight that can emit a packet.
    [[nodiscard]] bool ProgramSingleSilencePacketForRunPreflight(
        uint8_t* payloadBase,
        uint32_t numSlots,
        uint32_t slotStrideBytes,
        const ASFW::IsochTransport::TxPacketMeta* metadataRing,
        SingleSilencePacketProgram& outProgram) noexcept;
    [[nodiscard]] SingleSilencePacketCompletion
    FetchSingleSilencePacketCompletionForRunPreflight(
        uint32_t packetSlot) noexcept;

    struct FiniteSilenceCadenceProgram final {
        uint32_t commandPtr{0U};
        uint32_t startPacketSlot{0U};
        uint32_t descriptorCount{0U};
        uint32_t dataPacketCount{0U};
        uint32_t skipPacketCount{0U};
        uint32_t payloadLength{0U};
    };

    struct FiniteSilenceCadenceCompletion final {
        uint32_t completedDescriptors{0U};
        uint32_t completedDataDescriptors{0U};
        uint32_t actualBusPackets{0U};
        uint32_t eventErrors{0U};
        uint16_t lastTransferStatus{0U};
        uint16_t lastTimestamp{0U};
        bool cadenceValid{false};
        std::array<uint16_t, 36U> dataTimestamps{};
    };

    // Stage 5G safety seam: convert the already-primed 48-cycle FF800 ring
    // into one finite D-D-D-S cadence burst. All 36 data payloads must be
    // 1536-byte no-CIP silence packets; all 12 idle cycles remain true OHCI
    // skip programs. Queue branches are finite and the final skip terminates.
    [[nodiscard]] bool ProgramFiniteSilenceCadenceForRunPreflight(
        uint8_t* payloadBase,
        uint32_t numSlots,
        uint32_t slotStrideBytes,
        const ASFW::IsochTransport::TxPacketMeta* metadataRing,
        FiniteSilenceCadenceProgram& outProgram) noexcept;
    [[nodiscard]] FiniteSilenceCadenceCompletion
    FetchFiniteSilenceCadenceCompletionForRunPreflight(
        uint32_t startPacketSlot) noexcept;

    struct BoundedCircularSilenceCadenceProgram final {
        uint32_t commandPtr{0U};
        uint32_t startPacketSlot{0U};
        uint32_t descriptorCount{0U};
        uint32_t dataPacketCount{0U};
        uint32_t skipPacketCount{0U};
        uint32_t payloadLength{0U};
    };

    struct CadenceAnchorCompletion final {
        uint16_t transferStatus{0U};
        uint16_t timestamp{0U};
    };

    // Stage 5H safety seam: reuse the Stage 5G-validated 48-cycle D-D-D-S
    // silence program, then connect its final skip back to the first data
    // descriptor. The ring remains immutable while RUN is set; the caller
    // must stop it after a strictly bounded interval.
    [[nodiscard]] bool ProgramBoundedCircularSilenceCadenceForPreflight(
        uint8_t* payloadBase,
        uint32_t numSlots,
        uint32_t slotStrideBytes,
        const ASFW::IsochTransport::TxPacketMeta* metadataRing,
        BoundedCircularSilenceCadenceProgram& outProgram) noexcept;
    [[nodiscard]] CadenceAnchorCompletion
    FetchCadenceAnchorCompletionForPreflight(
        uint32_t startPacketSlot) noexcept;

    struct AllSkipCompletionSnapshot final {
        uint32_t completedDescriptors{0};
        uint16_t lastTransferStatus{0};
        uint16_t lastTimestamp{0};
    };

    // Stage 5E safety seam: replace every packet program with a true OHCI
    // transmit skip descriptor. The chain is deliberately finite: descriptors
    // 0..46 branch forward and descriptor 47 terminates with branchAddress=0.
    // This mirrors the normal OHCI queue model and lets completion status,
    // rather than the transient ACTIVE bit, prove that DMA executed.
    [[nodiscard]] bool ProgramAllSkipPacketsForRunPreflight() noexcept;
    [[nodiscard]] AllSkipCompletionSnapshot
    FetchAllSkipCompletionForRunPreflight() noexcept;

    [[nodiscard]] RefillOutcome Refill(Driver::HardwareInterface& hw,
                                       uint8_t contextIndex,
                                       ASFW::IsochTransport::TxPacketMeta* metadataRing,
                                       ASFW::IsochTransport::TxStreamControl* controlBlock,
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
    void ProgramSkipPacket(uint32_t packetSlot) noexcept;
    [[nodiscard]] bool IsSkipDescriptor(uint32_t packetSlot) const noexcept;
    [[nodiscard]] OHCIDescriptor* CompletionDescriptorForPacket(
        uint32_t packetSlot) noexcept;
    [[nodiscard]] bool DecodeHardwarePacketIndex(Driver::HardwareInterface& hw,
                                                 uint8_t contextIndex,
                                                 uint32_t& outPacketIndex,
                                                 uint32_t& outCmdPtr) noexcept;
    void GaugeWirePayload(uint64_t fillAbsIdx,
                          const uint8_t* packetBytes,
                          uint32_t payloadLength,
                          uint32_t immediateHeaderQ0LE) noexcept;

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

    // Wire-truth gauge state (refill is single-threaded)
    bool wireLastPacketHadInfo_{false};
    bool wireFirstInfoLogged_{false};

    Counters counters_{};
};

} // namespace ASFW::Isoch::Tx
