// IsochTxVerifier.hpp
// ASFW - Dev-only IT TX verifier (off-RT analysis + hot-path capture).

#pragma once

#include "IsochTxDmaRing.hpp"
#include "IsochTxRecoveryController.hpp"
#include "TxVerifierDecode.hpp"

#include "../Encoding/AM824Encoder.hpp"
#include "../Encoding/CIPHeaderBuilder.hpp"
#include "../Encoding/PacketAssembler.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#include <array>
#include <atomic>
#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW::Isoch {

class IsochTxVerifier final : public Tx::IsochTxCaptureHook {
public:
    struct Inputs {
        uint32_t framesPerPacket{0};
        uint32_t pcmChannels{0};
        uint32_t am824Slots{0};
        Encoding::AudioWireFormat audioWireFormat{Encoding::AudioWireFormat::kAM824};
        bool zeroCopyEnabled{false};
        bool sharedTxQueueValid{false};
        uint32_t sharedTxQueueFillFrames{0};

        uint64_t audioInjectCursorResets{0};
        uint64_t audioInjectMissedPackets{0};
        uint64_t underrunSilencedPackets{0};
        uint64_t criticalGapEvents{0};
        uint64_t dbcDiscontinuities{0};
    };

    IsochTxVerifier() noexcept = default;

    void BindRecovery(IsochTxRecoveryController* recovery) noexcept { recovery_ = recovery; }

    void ResetForStart(uint8_t blocksPerData) noexcept;
    void Shutdown() noexcept;

    void Kick(const Inputs& inputs) noexcept;

    // Tx::IsochTxCaptureHook
    void CaptureBeforeOverwrite(uint32_t packetIndex,
                               uint32_t hwPacketIndexCmdPtr,
                               uint32_t cmdPtr,
                               const Async::HW::OHCIDescriptor* lastDesc,
                               const uint32_t* payload32) noexcept override;

    [[nodiscard]] uint64_t DroppedTrace() const noexcept { return trace_.dropped.load(std::memory_order_relaxed); }

private:
    using ParsedCIP = decltype(TxVerify::ParseCIPFromHostWords(uint32_t{}, uint32_t{}));

    struct TraceEntry {
        uint32_t packetIndex{0};
        uint32_t hwPacketIndexCmdPtr{0};
        uint32_t cmdPtr{0};
        uint32_t lastDescControl{0};
        uint32_t lastDescStatus{0};
        uint32_t cipQ0Host{0};
        uint32_t cipQ1Host{0};
        uint16_t reqCount{0};
        uint16_t audioQuadletCount{0};
        std::array<uint32_t,
                   static_cast<size_t>(Tx::Layout::kAudioWriteAhead) * Config::kMaxAmdtpDbs>
            audioHost{};
    };

    static constexpr uint32_t kTraceCapacity = 1024;
    static_assert((kTraceCapacity & (kTraceCapacity - 1)) == 0, "capacity must be power-of-two");

    struct TraceRing {
        std::array<TraceEntry, kTraceCapacity> entries{};
        std::atomic<uint32_t> writeIndex{0};
        std::atomic<uint32_t> readIndex{0};
        std::atomic<uint64_t> dropped{0};
    };

    struct State {
        bool haveLastDataDbc{false};
        uint8_t lastDataDbc{0};
        uint8_t blocksPerData{0};
        uint32_t silentDataRun{0};
        uint32_t injectMissConsecutiveTicks{0};

        uint64_t lastInjectCursorResets{0};
        uint64_t lastInjectMissedPackets{0};
        uint64_t lastUnderrunSilencedPackets{0};
        uint64_t lastCriticalGapEvents{0};
        uint64_t lastDbcDiscontinuities{0};
        uint64_t lastDroppedTrace{0};
    };

    struct CounterDeltaSnapshot {
        uint64_t curInjectResets{0};
        uint64_t curInjectMissed{0};
        uint64_t curUnderrunSilenced{0};
        uint64_t curCriticalGap{0};
        uint64_t curDbcDisc{0};
        uint64_t curDroppedTrace{0};

        uint64_t deltaResets{0};
        uint64_t deltaMissed{0};
        uint64_t deltaUnderrunSilenced{0};
        uint64_t deltaCriticalGap{0};
        uint64_t deltaDbcDisc{0};
        uint64_t deltaDropped{0};
    };

    struct PacketExpectations {
        uint16_t expectedNoDataReq{0};
        uint16_t expectedDataReq{0};
        uint32_t expectedAm824Slots{0};
        uint32_t slotsPerFrame{1};
        uint32_t pcmSlots{0};
        Encoding::AudioWireFormat audioWireFormat{Encoding::AudioWireFormat::kAM824};
    };

    struct AudioPayloadScan {
        bool allSilence{true};
        bool sawAllZero{false};
        bool sawInvalidLabel{false};
        bool sawInvalidLabelNonZero{false};
        uint8_t badLabel{0};
        uint32_t badWord{0};
    };

    [[nodiscard]] bool Pop(TraceEntry& out) noexcept;
    void DrainTrace() noexcept;
    [[nodiscard]] CounterDeltaSnapshot CaptureCounterDeltas() const noexcept;
    void LogCounterDeltas(const CounterDeltaSnapshot& deltas) const noexcept;
    [[nodiscard]] uint32_t UpdateCounterState(const CounterDeltaSnapshot& deltas) noexcept;
    [[nodiscard]] PacketExpectations BuildPacketExpectations() const noexcept;
    [[nodiscard]] static uint8_t ExpectedAM824Label(
        uint32_t slotInFrame,
        const PacketExpectations& expectations) noexcept;
    static void RecordInvalidLabel(AudioPayloadScan& scan, uint32_t q) noexcept;
    [[nodiscard]] static AudioPayloadScan ScanAudioPayload(
        const TraceEntry& entry,
        const PacketExpectations& expectations) noexcept;
    void ProcessTraceEntries(const PacketExpectations& expectations, uint64_t deltaMissed,
                             uint32_t& restartReasons) noexcept;
    void ProcessTraceEntry(const TraceEntry& entry, const PacketExpectations& expectations,
                           uint64_t deltaMissed, uint32_t& restartReasons) noexcept;
    void CheckCompletionAndPacketShape(const TraceEntry& entry,
                                       const PacketExpectations& expectations,
                                       const ParsedCIP& cip,
                                       bool isNoData,
                                       bool isData,
                                       uint32_t& restartReasons) const noexcept;
    void CheckDbcContinuity(const TraceEntry& entry,
                            const ParsedCIP& cip,
                            bool isData,
                            uint32_t& restartReasons) noexcept;
    void CheckAudioPayload(const TraceEntry& entry,
                           const PacketExpectations& expectations,
                           uint64_t deltaMissed,
                           uint32_t& restartReasons) noexcept;
    void RunWork() noexcept;

    Inputs inputs_{};
    IsochTxRecoveryController* recovery_{nullptr};

    std::atomic_flag queued_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> shuttingDown_{false};

    TraceRing trace_{};
    State state_{};

#ifndef ASFW_HOST_TEST
    OSSharedPtr<IODispatchQueue> queue_{nullptr};
#endif
};

} // namespace ASFW::Isoch
