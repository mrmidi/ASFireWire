// IsochAudioTxPipeline.hpp
// ASFW - Audio semantics layer for IT transmit (CIP/AM824 + buffering policy).

#pragma once

#include "IsochTxDmaRing.hpp"
#include "IsochTxLayout.hpp"

#include "../Encoding/PacketAssembler.hpp"
#include "../Encoding/SYTGenerator.hpp"
#include "../Core/ExternalSyncBridge.hpp"
#include "../Core/ExternalSyncDiscipline48k.hpp"
#include "../Config/AudioTxProfiles.hpp"
#include "../../Shared/TxSharedQueue.hpp"
#include "../../Logging/Logging.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ASFW::Isoch {

/// Owns all "audio semantics" (PacketAssembler/CIP/AM824) and buffering policy.
/// Provides silent packets to the low-level DMA engine and injects real audio
/// into near-HW slots (latency control).
class IsochAudioTxPipeline final : public Tx::IIsochTxPacketProvider, public Tx::IIsochTxAudioInjector {
public:
    struct Counters {
        std::atomic<uint64_t> resyncApplied{0};
        std::atomic<uint64_t> staleFramesDropped{0};
        std::atomic<uint64_t> legacyPumpMovedFrames{0};
        std::atomic<uint64_t> legacyPumpSkipped{0};
        std::atomic<uint64_t> exitZeroRefill{0};
        std::atomic<uint64_t> underrunSilencedPackets{0};
        std::atomic<uint64_t> audioInjectCursorResets{0};
        std::atomic<uint64_t> audioInjectMissedPackets{0};

        // Fill level low-water alerts (with hysteresis)
        std::atomic<uint64_t> rbLowEvents{0};
        std::atomic<uint64_t> txqLowEvents{0};
    };

    IsochAudioTxPipeline() noexcept = default;

    // Public-ish facade methods (delegated by IsochTransmitContext)
    void SetSharedTxQueue(uint8_t* base, uint64_t bytes) noexcept;
    [[nodiscard]] uint32_t SharedTxFillLevelFrames() const noexcept;
    [[nodiscard]] uint32_t SharedTxCapacityFrames() const noexcept;
    [[nodiscard]] bool SharedTxQueueValid() const noexcept { return sharedTxQueue_.IsValid(); }

    void SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept;

    void SetZeroCopyOutputBuffer(const int32_t* base, uint64_t bytes,
                                 uint32_t frameCapacity) noexcept;
    [[nodiscard]] bool IsZeroCopyEnabled() const noexcept { return zeroCopyEnabled_; }

    [[nodiscard]] Encoding::StreamMode RequestedStreamMode() const noexcept { return requestedStreamMode_; }
    [[nodiscard]] Encoding::StreamMode EffectiveStreamMode() const noexcept { return effectiveStreamMode_; }
    [[nodiscard]] Encoding::AudioWireFormat WireFormat() const noexcept { return assembler_.audioWireFormat(); }

    [[nodiscard]] Encoding::AudioRingBuffer<>& RingBuffer() noexcept { return assembler_.ringBuffer(); }
    [[nodiscard]] uint64_t UnderrunCount() const noexcept { return assembler_.underrunCount(); }
    [[nodiscard]] uint32_t BufferFillLevel() const noexcept { return assembler_.bufferFillLevel(); }

    [[nodiscard]] uint32_t FramesPerDataPacket() const noexcept { return assembler_.samplesPerDataPacket(); }
    [[nodiscard]] uint32_t ChannelCount() const noexcept { return assembler_.channelCount(); } // PCM channels
    [[nodiscard]] uint32_t Am824SlotCount() const noexcept { return assembler_.am824SlotCount(); }
    [[nodiscard]] uint64_t DbcDiscontinuityCount() const noexcept { return dbcTracker_.discontinuityCount.load(std::memory_order_relaxed); }

    void ResetForStart() noexcept;
    void SetCycleTrackingValid(bool v) noexcept { cycleTrackingValid_ = v; }
    [[nodiscard]] bool PrimeSyncFromExternalBridge() noexcept;

    // Configure audio packetization from shared queue metadata.
    [[nodiscard]] kern_return_t Configure(uint8_t sid,
                                          uint32_t streamModeRaw,
                                          uint32_t requestedChannels,
                                          uint32_t requestedAm824Slots,
                                          Encoding::AudioWireFormat wireFormat) noexcept;

    // Start-time pre-prime: move some frames from shared queue into assembler ring.
    void PrePrimeFromSharedQueue() noexcept;

    // Called from refill path before touching HW ring: maintains legacy jitter-buffer policy.
    void OnRefillTickPreHW() noexcept;

    // Called from 1ms watchdog Poll() to update adaptive fill target.
    void OnPollTick1ms() noexcept;

    [[nodiscard]] const Counters& RTCounters() const noexcept { return counters_; }

    // -------------------------------------------------------------------------
    // Tx::IIsochTxPacketProvider
    // -------------------------------------------------------------------------
    // The returned payload pointer remains valid until the next
    // NextSilentPacket() call on this pipeline instance.
    [[nodiscard]] Tx::IsochTxPacket NextSilentPacket(uint32_t transmitCycle) noexcept override;

    // -------------------------------------------------------------------------
    // Tx::IIsochTxAudioInjector
    // -------------------------------------------------------------------------
    void InjectNearHw(uint32_t hwPacketIndex, Tx::IsochTxDescriptorSlab& slab) noexcept override;

private:
    struct LegacyPumpResult {
        uint32_t pumpedFrames{0};
        bool skipped{true};
    };

    struct ExternalSyncState {
        bool enabled{false};
        uint16_t rxSyt{Core::ExternalSyncBridge::kNoInfoSyt};
    };

    struct AudioInjectionPlan {
        bool zeroCopySync{false};
        uint32_t audioTarget{0};
        uint32_t packetsToInject{0};
        uint32_t framesPerPacket{0};
        uint32_t pcmChannels{0};
        uint32_t am824Slots{0};
    };

    struct PacketReadResult {
        uint32_t framesRead{0};
        bool leaveSilence{false};
    };

    void ApplyPendingSharedQueueResync() noexcept;
    [[nodiscard]] LegacyPumpResult PumpLegacyAssemblerRing(uint32_t targetRbFillFrames) noexcept;
    void RecordLegacyPumpResult(const LegacyPumpResult& result) noexcept;
    void UpdateLegacyFillLevelAlerts() noexcept;
    void AdvanceAdaptiveFillWindow() noexcept;
    void ApplyAdaptiveFillPolicy() noexcept;
    [[nodiscard]] uint16_t ComputeDataSyt(uint32_t transmitCycle) noexcept;
    [[nodiscard]] ExternalSyncState ReadExternalSyncState() noexcept;
    [[nodiscard]] bool HasFreshExternalSyncUpdate(const Core::ExternalSyncBridge& bridge) noexcept;
    [[nodiscard]] bool MaybeApplyExternalSyncDiscipline(uint16_t txSyt) noexcept;
    [[nodiscard]] AudioInjectionPlan BuildAudioInjectionPlan(uint32_t hwPacketIndex) noexcept;
    [[nodiscard]] bool PacketCarriesAudio(uint32_t packetIndex, Tx::IsochTxDescriptorSlab& slab) noexcept;
    [[nodiscard]] PacketReadResult ReadPacketSamples(const AudioInjectionPlan& plan, int32_t* samples) noexcept;
    [[nodiscard]] PacketReadResult ReadZeroCopyPacketSamples(const AudioInjectionPlan& plan,
                                                             int32_t* samples) noexcept;
    void CopyZeroCopyFrames(const AudioInjectionPlan& plan, int32_t* samples) noexcept;
    void PadPacketSamples(const AudioInjectionPlan& plan, uint32_t framesRead, int32_t* samples) noexcept;
    void EncodeInjectedPacket(uint32_t packetIndex,
                              Tx::IsochTxDescriptorSlab& slab,
                              const AudioInjectionPlan& plan,
                              const int32_t* samples) noexcept;

    // Fill-level alerts (with hysteresis)
    struct FillLevelAlert {
        bool rbLow{false};
        bool txqLow{false};
    } fillLevelAlert_;

    // Adaptive Fill Level Target
    struct AdaptiveFill {
        uint32_t currentTarget{0};
        uint32_t baseTarget{0};
        uint32_t maxTarget{0};
        uint32_t underrunsInWindow{0};
        uint32_t windowTickCount{0};
        uint32_t cleanWindows{0};
        uint64_t lastCombinedUnderruns{0};
    } adaptiveFill_;

    Encoding::PacketAssembler assembler_{};
    alignas(std::uint32_t) std::array<std::uint8_t, Encoding::kMaxAssembledPacketSize> silentPacketStorage_{};
    Shared::TxSharedQueueSPSC sharedTxQueue_{};

    // ZERO-COPY: Direct pointer to CoreAudio output buffer
    const int32_t* zeroCopyAudioBase_{nullptr};
    uint64_t zeroCopyAudioBytes_{0};
    uint32_t zeroCopyFrameCapacity_{0};
    bool zeroCopyEnabled_{false};

    Encoding::StreamMode requestedStreamMode_{Encoding::StreamMode::kNonBlocking};
    Encoding::StreamMode effectiveStreamMode_{Encoding::StreamMode::kNonBlocking};

    // SYT generation + external sync discipline
    Encoding::SYTGenerator sytGenerator_{};
    bool cycleTrackingValid_{false};
    Core::ExternalSyncBridge* externalSyncBridge_{nullptr};
    Core::ExternalSyncDiscipline48k externalSyncDiscipline_{};

    // Audio injection cursor (packet index)
    uint32_t audioWriteIndex_{0};

    // DBC continuity validation for produced packets (ignore NO-DATA).
    struct DbcTracker {
        uint8_t lastDbc{0};
        uint8_t lastDataBlockCount{0};
        bool firstPacket{true};
        std::atomic<uint64_t> discontinuityCount{0};
    } dbcTracker_{};

    Counters counters_{};
};

} // namespace ASFW::Isoch
