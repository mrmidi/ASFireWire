// IsochAudioTxPipeline.hpp
// ASFW - Audio semantics layer for IT transmit (CIP/AM824 + direct ADK memory).

#pragma once

#include "../../Isoch/Transmit/IsochTxDmaRing.hpp"
#include "../../Isoch/Transmit/IsochTxLayout.hpp"

#include "../../AudioWire/AMDTP/PacketAssembler.hpp"
#include "../../AudioWire/AMDTP/SYTGenerator.hpp"
#include "../Direct/AudioClockPublisher.hpp"
#include "../Direct/DirectOutputReader.hpp"
#include "../../Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "../../Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "Sync/ExternalSyncBridge.hpp"
#include "../../Isoch/Core/IsochEventGroup.hpp"
#include "Timing/TxOutputPhaseLoop.hpp"
#include "Timing/OutputPhaseToAudioMap.hpp"
#include "../../Isoch/Config/AudioTxProfiles.hpp"
#include "../../Isoch/Memory/IIsochDMAMemory.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Audio/Runtime/TimingCursorPolicy.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ASFW::Isoch {

/// Owns audio packet timing and maps CoreAudio ranges into writable future
/// payload slots owned by IsochTxDmaRing.
class IsochAudioTxPipeline final : public Tx::IIsochTxPacketProvider,
                                   public Tx::IIsochTxPayloadPreparer,
                                   public Tx::IIsochTxCompletionObserver {
public:
    static constexpr bool kEnableDirectTxHardwarePath = true;

    struct Counters {
        std::atomic<uint64_t> resyncApplied{0};

        std::atomic<uint64_t> directTxPackets{0};
        std::atomic<uint64_t> directTxUnderrunSilencedPackets{0};
        std::atomic<uint64_t> directTxInvalidPackets{0};
        std::atomic<uint64_t> directTxNoDataPackets{0};
        std::atomic<uint64_t> directTxSilenceFallback{0};
        std::atomic<uint64_t> directTxPhaseRebases{0};
    };

    /// Plain, RT-safe view of the ADK output stream memory + shared transport
    /// control block retrieved via DirectAudioBindingSource snapshot.
    struct DirectTxRuntimeBinding final {
        const int32_t* outputBase{nullptr};
        uint64_t outputBytes{0};
        uint32_t outputFrames{0};
        ASFW::Audio::Runtime::AudioTransportControlBlock* control{nullptr};

        bool enabled{false};
        uint32_t sampleRateHz{0};
        uint32_t streamModeRaw{0};
        uint32_t outputChannels{0};
        uint32_t am824Slots{0};
    };

    IsochAudioTxPipeline() noexcept = default;

    void SetExternalSyncBridge(ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge* bridge) noexcept;
    void SetDirectTxRuntimeBinding(const DirectTxRuntimeBinding& binding) noexcept;
    void SetDMAMemory(Memory::IIsochDMAMemory* dma) noexcept { dmaMemory_ = dma; }

    [[nodiscard]] Encoding::StreamMode RequestedStreamMode() const noexcept { return requestedStreamMode_; }
    [[nodiscard]] Encoding::StreamMode EffectiveStreamMode() const noexcept { return effectiveStreamMode_; }
    [[nodiscard]] Encoding::AudioWireFormat WireFormat() const noexcept { return assembler_.audioWireFormat(); }

    [[nodiscard]] uint32_t FramesPerDataPacket() const noexcept { return assembler_.samplesPerDataPacket(); }
    [[nodiscard]] uint32_t ChannelCount() const noexcept { return assembler_.channelCount(); } 
    [[nodiscard]] uint32_t Am824SlotCount() const noexcept { return assembler_.am824SlotCount(); }

    void ResetForStart() noexcept;
    void SetCycleTrackingValid(bool v) noexcept { cycleTrackingValid_ = v; }
    [[nodiscard]] bool PrimeSyncFromExternalBridge() noexcept;
    void OnIsochEventGroup(const Core::IsochEventGroup& group) noexcept;

    // Configure audio packetization.
    [[nodiscard]] kern_return_t Configure(uint8_t sid,
                                          uint32_t streamModeRaw,
                                          uint32_t requestedChannels,
                                          uint32_t requestedAm824Slots,
                                          Encoding::AudioWireFormat wireFormat) noexcept;

    [[nodiscard]] const Counters& RTCounters() const noexcept { return counters_; }

    // -------------------------------------------------------------------------
    // Tx::IIsochTxPacketProvider
    // -------------------------------------------------------------------------
    [[nodiscard]] Tx::IsochTxPacket NextTransmitPacket(const Tx::TxPacketRequest& request) noexcept override;
    [[nodiscard]] Tx::PreparedTxPayloadResult PreparePayload(
        const Tx::PreparedTxPayloadRequest& request) noexcept override;
    [[nodiscard]] bool OnTransmitSlotCompleted(
        const Tx::CompletedTxSlot& completed) noexcept override;
    [[nodiscard]] bool HasFatalFault() const noexcept;
    void RecordImmediateStop() noexcept;
    [[nodiscard]] uint64_t PreparationRequestGeneration() const noexcept;
    [[nodiscard]] uint64_t PreparationHandledGeneration() const noexcept;
    void MarkPreparationRequestHandled(uint64_t generation,
                                       uint64_t hostTicks) noexcept;
    void PopulateClipStyleTxRingFromWrittenRange() noexcept;

private:
    struct ExternalSyncState {
        enum class SeedStatus : uint8_t {
            Ok = 0,
            NoBridge,
            Inactive,
            NotEstablished,
            MissingTimestamp,
            Stale,
            InvalidSyt,
            UnsupportedFdf,
        };

        bool enabled{false};
        uint16_t rxSyt{ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge::kNoInfoSyt};
        uint8_t rxFdf{0};
        uint8_t rxDbs{0};
        uint32_t updateSeq{0};
        uint64_t ageUsec{0};
        uint64_t staleThresholdUsec{0};
        bool bridgePresent{false};
        bool active{false};
        bool clockEstablished{false};
        bool startupQualified{false};
        SeedStatus status{SeedStatus::NoBridge};
    };

    struct PacketCipFields {
        uint8_t sid{0};
        uint8_t dbc{0};
        uint16_t syt{0};
    };

    struct ProducedPacketMetadata {
        bool valid{false};
        bool isData{false};
        uint32_t sizeBytes{0};
        uint32_t framesPerPacket{0};
        uint32_t pcmChannels{0};
        uint32_t am824Slots{0};
        uint32_t packetIndex{0};
        Encoding::AudioWireFormat wireFormat{Encoding::AudioWireFormat::kAM824};
        uint64_t audioFrame{0};
        int64_t outputPhaseTicks{-1};
        Tx::PreparedTxSlotState preparationState{Tx::PreparedTxSlotState::InitialSilence};
        PacketCipFields cip{};
    };

    // Builds the 1-second-domain phase for this transmit cycle by grafting the
    // device's recovered intra-cycle sub-phase onto the local bus cycle base.
    // NOTE: This is NOT true recovered device phase — long-term whole-cycle device
    // drift is handled separately by SYTGenerator::nudgeOffsetTicks().
    [[nodiscard]] bool TryBuildTransmitCycleDeviceSubphase(uint32_t transmitCycle,
                                                           int64_t* outDevicePhaseTicks) noexcept;
    [[nodiscard]] ExternalSyncState ReadExternalSyncState(bool allowStartupQualifiedOnly) noexcept;
    void PublishDirectTxConsumedEndFrame(uint64_t consumedEndFrame) noexcept;
    void PublishFatalFault(ASFW::Audio::Runtime::FatalStreamReason reason,
                           const Tx::PreparedTxPayloadRequest& request,
                           uint64_t oldestValidFrame,
                           uint64_t writtenEndFrame,
                           uint64_t preparedPayloadHash = 0,
                           uint64_t completedPayloadHash = 0) noexcept;
    void MaybeLogTxFrameMatrix(uint64_t populatedBegin,
                               uint64_t populatedEnd) noexcept;

    Encoding::PacketAssembler assembler_{};
    alignas(std::uint32_t) std::array<std::uint8_t, Encoding::kMaxAssembledPacketSize> silentPacketStorage_{};
    std::array<ProducedPacketMetadata, Tx::Layout::kNumPackets> producedPacketMetadata_{};
    std::array<ProducedPacketMetadata, Tx::Layout::kNumPackets> previousPacketMetadata_{};
    uint8_t sid_{0};

    DirectTxRuntimeBinding directTxBinding_{};
    ASFW::Audio::Runtime::AudioGraphBinding directOutputView_{};
    ASFW::AudioEngine::Direct::DirectOutputReader directOutputReader_{};
    ASFW::AudioEngine::Direct::AudioClockPublisher txClockPublisher_{};
    uint64_t lastCompletedAudioFrame_{0};
    uint64_t txEventGroupCount_{0};

    Encoding::StreamMode requestedStreamMode_{Encoding::StreamMode::kNonBlocking};
    Encoding::StreamMode effectiveStreamMode_{Encoding::StreamMode::kNonBlocking};

    // SYT generation + external sync discipline
    Encoding::SYTGenerator sytGenerator_{};
    bool cycleTrackingValid_{false};
    ASFW::AudioEngine::DirectIsoch::ExternalSyncBridge* externalSyncBridge_{nullptr};
    ASFW::AudioEngine::DirectIsoch::TxOutputPhaseLoop txPhaseLoop_{};
    ASFW::AudioEngine::DirectIsoch::OutputPhaseToAudioMap txPhaseMap_{};
    ASFW::AudioEngine::DirectIsoch::TxOutputPhaseLoop::Decision lastPhaseResult_{};
    bool txPhaseReadIndexSeeded_{false};
    bool rxTimingUsable_{false};

    Memory::IIsochDMAMemory* dmaMemory_{nullptr};

    // DBC continuity validation for produced packets (ignore NO-DATA).
    struct DbcTracker {
        uint8_t lastDbc{0};
        uint8_t lastDataBlockCount{0};
        bool firstPacket{true};
        std::atomic<uint64_t> discontinuityCount{0};

        void Reset() noexcept {
            lastDbc = 0;
            lastDataBlockCount = 0;
            firstPacket = true;
            discontinuityCount.store(0, std::memory_order_relaxed);
        }
    } dbcTracker_{};

    // Temporary bring-up diagnostics: noisy by design, remove once TX is stable.
    uint64_t debugProducedPackets_{0};
    uint64_t debugTryBuildCount_{0};

    Counters counters_{};
    ASFW::Audio::TimingCursorPolicy timingPolicy_{ASFW::Audio::TimingCursorPolicy::MakeDice48kBlocking()};

    struct TxPcmPacketRing {
        static constexpr uint32_t kMaxOutputFrames = 4096;
        static constexpr uint32_t kMaxChannels = 16;

        struct SlotStamp {
            uint64_t begin{0};
            uint64_t end{0};
            bool hasAudioWrite{false};
        };

        uint32_t words[kMaxOutputFrames * kMaxChannels]{0};
        uint32_t silenceWords[Encoding::kSamplesPerPacket48k * kMaxChannels]{0};
        SlotStamp slotStamps[kMaxOutputFrames / Encoding::kSamplesPerPacket48k]{};

        uint64_t baseFrame{0};
        uint32_t outputFrameCapacity{0};
        uint32_t framesPerPacket{0};
        uint32_t packetSlots{0};
        uint32_t packetBase{0};
        uint32_t am824Slots{0};
        bool valid{false};

        uint64_t RelativeFrame(uint64_t absoluteFrame) const noexcept {
            if (outputFrameCapacity == 0) return 0;
            const uint64_t a = absoluteFrame % outputFrameCapacity;
            const uint64_t b = baseFrame % outputFrameCapacity;
            return (a + outputFrameCapacity - b) % outputFrameCapacity;
        }

        uint32_t SlotForFrame(uint64_t absoluteFrame) const noexcept {
            if (framesPerPacket == 0 || packetSlots == 0) return 0;
            return (packetBase + static_cast<uint32_t>(RelativeFrame(absoluteFrame) / framesPerPacket)) % packetSlots;
        }

        uint32_t FrameInsidePacket(uint64_t absoluteFrame) const noexcept {
            if (framesPerPacket == 0) return 0;
            return static_cast<uint32_t>(RelativeFrame(absoluteFrame) % framesPerPacket);
        }

        uint64_t PacketFirstFrame(uint64_t absoluteFrame) const noexcept {
            return absoluteFrame - FrameInsidePacket(absoluteFrame);
        }

        uint32_t* PayloadForAudioFrame(uint64_t absoluteFrame) noexcept {
            if (am824Slots == 0) return nullptr;
            return words + ((static_cast<size_t>(SlotForFrame(absoluteFrame)) * framesPerPacket + FrameInsidePacket(absoluteFrame)) * am824Slots);
        }

        const uint32_t* PacketPayloadForTimeline(uint64_t timelineFirstFrame) const noexcept {
            if (am824Slots == 0) return nullptr;
            return words + (static_cast<size_t>(SlotForFrame(timelineFirstFrame)) * framesPerPacket * am824Slots);
        }

        const uint32_t* SilencePayload() const noexcept {
            return am824Slots == 0 ? nullptr : silenceWords;
        }
    };

    TxPcmPacketRing txPcmPacketRing_{};
    uint64_t lastPopulatedWriteEnd_{0};
    uint64_t lastTxFrameMatrixLogNs_{0};
};

} // namespace ASFW::Isoch
