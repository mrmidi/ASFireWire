// IsochAudioTxPipeline.hpp
// ASFW - Audio semantics layer for IT transmit (CIP/AM824 + direct ADK memory).

#pragma once

#include "../../Isoch/Transmit/IsochTxDmaRing.hpp"
#include "../../Isoch/Transmit/IsochTxLayout.hpp"

#include "../../AudioWire/AMDTP/PacketAssembler.hpp"
#include "../../AudioWire/AMDTP/SYTGenerator.hpp"
#include "../Direct/Tx/DirectTxTypes.hpp"
#include "../Direct/Tx/TxAudioPacketWriter.hpp"
#include "../Direct/Tx/OutputCursorDiscipline.hpp"
#include "../Direct/DirectOutputReader.hpp"
#include "../../Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "../../Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../Isoch/Core/ExternalSyncBridge.hpp"
#include "../../Isoch/Core/ExternalSyncDiscipline48k.hpp"
#include "../../Isoch/Config/AudioTxProfiles.hpp"
#include "../../Logging/Logging.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ASFW::Isoch {

/// Owns all "audio semantics" (PacketAssembler/CIP/AM824) and direct mapping policy.
/// Provides silent packets to the low-level DMA engine and injects real audio
/// into near-HW slots by reading directly from ADK stream memory.
class IsochAudioTxPipeline final : public Tx::IIsochTxPacketProvider, public Tx::IIsochTxAudioInjector {
public:
    static constexpr bool kEnableDirectTxHardwarePath = true;

    struct Counters {
        std::atomic<uint64_t> resyncApplied{0};
        std::atomic<uint64_t> audioInjectCursorResets{0};
        std::atomic<uint64_t> audioInjectMissedPackets{0};

        std::atomic<uint64_t> directTxPackets{0};
        std::atomic<uint64_t> directTxUnderrunSilencedPackets{0};
        std::atomic<uint64_t> directTxInvalidPackets{0};
        std::atomic<uint64_t> directTxCursorResyncs{0};
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

    void SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept;
    void SetDirectTxRuntimeBinding(const DirectTxRuntimeBinding& binding) noexcept;

    [[nodiscard]] Encoding::StreamMode RequestedStreamMode() const noexcept { return requestedStreamMode_; }
    [[nodiscard]] Encoding::StreamMode EffectiveStreamMode() const noexcept { return effectiveStreamMode_; }
    [[nodiscard]] Encoding::AudioWireFormat WireFormat() const noexcept { return assembler_.audioWireFormat(); }

    [[nodiscard]] uint32_t FramesPerDataPacket() const noexcept { return assembler_.samplesPerDataPacket(); }
    [[nodiscard]] uint32_t ChannelCount() const noexcept { return assembler_.channelCount(); } 
    [[nodiscard]] uint32_t Am824SlotCount() const noexcept { return assembler_.am824SlotCount(); }

    void ResetForStart() noexcept;
    void SetCycleTrackingValid(bool v) noexcept { cycleTrackingValid_ = v; }
    [[nodiscard]] bool PrimeSyncFromExternalBridge() noexcept;

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

    // -------------------------------------------------------------------------
    // Tx::IIsochTxAudioInjector
    // -------------------------------------------------------------------------
    void InjectNearHw(uint32_t hwPacketIndex, Tx::IsochTxDescriptorSlab& slab) noexcept override;

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
        uint16_t rxSyt{Core::ExternalSyncBridge::kNoInfoSyt};
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

    struct AudioInjectionPlan {
        uint32_t audioTarget{0};
        uint32_t packetsToInject{0};
        uint32_t framesPerPacket{0};
        uint32_t pcmChannels{0};
        uint32_t am824Slots{0};
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
        Encoding::AudioWireFormat wireFormat{Encoding::AudioWireFormat::kAM824};
        PacketCipFields cip{};
    };

    [[nodiscard]] uint16_t ComputeDataSyt(uint32_t transmitCycle) noexcept;
    [[nodiscard]] ExternalSyncState ReadExternalSyncState(bool allowStartupQualifiedOnly) noexcept;
    [[nodiscard]] bool MaybeSeedFromExternalSync(const ExternalSyncState& state) noexcept;
    [[nodiscard]] bool MaybeApplyExternalSyncDiscipline(uint16_t txSyt,
                                                        const ExternalSyncState& state) noexcept;
    [[nodiscard]] AudioInjectionPlan BuildAudioInjectionPlan(uint32_t hwPacketIndex) noexcept;
    [[nodiscard]] bool PacketCarriesAudio(uint32_t packetIndex, Tx::IsochTxDescriptorSlab& slab) noexcept;
    [[nodiscard]] uint32_t PacketPayloadByteCount(uint32_t packetIndex,
                                                  Tx::IsochTxDescriptorSlab& slab) noexcept;
    [[nodiscard]] bool IsDirectTxHardwarePathReady(const AudioInjectionPlan& plan) const noexcept;
    [[nodiscard]] bool TryWriteDirectTxPacket(uint32_t packetIndex,
                                              Tx::IsochTxDescriptorSlab& slab,
                                              const AudioInjectionPlan& plan) noexcept;

    [[nodiscard]] bool InitializeDirectOutputCursor(const AudioInjectionPlan& plan) noexcept;
    void PublishDirectTxConsumedEndFrame(uint64_t consumedEndFrame) noexcept;
    void LogTxCursorDiagnostic(const char* source,
                               uint32_t packetIndex,
                               const ProducedPacketMetadata& metadata,
                               const PacketCipFields& cip,
                               uint64_t readFrame,
                               uint64_t consumedEndFrame,
                               ASFW::AudioEngine::Direct::Tx::DirectTxReadStatus readStatus,
                               uint32_t bytesWritten,
                               uint32_t framesEncoded,
                               bool usedSilence) noexcept;

    Encoding::PacketAssembler assembler_{};
    alignas(std::uint32_t) std::array<std::uint8_t, Encoding::kMaxAssembledPacketSize> silentPacketStorage_{};
    std::array<ProducedPacketMetadata, Tx::Layout::kNumPackets> producedPacketMetadata_{};
    uint8_t sid_{0};

    DirectTxRuntimeBinding directTxBinding_{};
    ASFW::Audio::Runtime::AudioGraphBinding directOutputView_{};
    ASFW::AudioEngine::Direct::DirectOutputReader directOutputReader_{};
    uint64_t directOutputFrameCursor_{0};
    bool directCursorInitialized_{false};

    Encoding::StreamMode requestedStreamMode_{Encoding::StreamMode::kNonBlocking};
    Encoding::StreamMode effectiveStreamMode_{Encoding::StreamMode::kNonBlocking};

    // SYT generation + external sync discipline
    Encoding::SYTGenerator sytGenerator_{};
    bool cycleTrackingValid_{false};
    Core::ExternalSyncBridge* externalSyncBridge_{nullptr};
    Core::ExternalSyncDiscipline48k externalSyncDiscipline_{};
    bool externalSyncSeeded_{false};
    uint32_t externalSyncSeedSeq_{0};

    // Audio injection cursor (packet index)
    uint32_t audioWriteIndex_{0};

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
    uint64_t debugInjectionAttempts_{0};
    uint64_t debugInjectionSuccesses_{0};
    uint64_t debugInjectionSkips_{0};

    Counters counters_{};
};

} // namespace ASFW::Isoch
