#pragma once

#include "../../../DriverKit/Config/AudioStreamProfile.hpp"
#include "../../../Ports/IAmdtpTxSlotProvider.hpp"
#include "../../../Ports/ITxPcmSource.hpp"
#include "../../../Shared/AudioHalBufferProfiles.hpp"
#include "../../../Shared/AudioTimingGeometry.hpp"
#include "../../../Wire/AMDTP/AmdtpCadence.hpp"
#include "../../../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../../Wire/AMDTP/AmdtpTxPacketizer.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

struct DiceTxEngineCounters final {
    std::atomic<uint64_t> packetsPrepared{0};
    std::atomic<uint64_t> dataPacketsPrepared{0};
    std::atomic<uint64_t> noDataPacketsPrepared{0};
    std::atomic<uint64_t> slotAcquireFailures{0};
    std::atomic<uint64_t> pcmCopiesReady{0};
    std::atomic<uint64_t> pcmCopiesNotYetPublished{0};
    std::atomic<uint64_t> pcmCopiesExpired{0};
    std::atomic<uint64_t> pcmCopiesWrongEpoch{0};
    std::atomic<uint64_t> pcmCopiesConcurrentRewrite{0};
    std::atomic<uint64_t> pcmCopiesInvalid{0};
    /// DATA packets whose PCM range was unavailable and encoded as silence.
    std::atomic<uint64_t> pcmSilenceSubstitutions{0};
};

enum class TxSlotPrepareResult : uint8_t {
    Prepared = 0,
    SlotProviderUnavailable,
    PcmSourceUnavailable,
    PcmNotYetPublished,
    PcmExpired,
    PcmWrongEpoch,
    PcmConcurrentRewrite,
    PcmInvalidRequest,
    SlotAcquireFailed,
    PacketizerRejected,
    SlotPublishFailed,
    CommitRejected,
};

class DiceStreamConfigMapper final {
public:
    [[nodiscard]] static AMDTP::AmdtpStreamConfig ToAmdtpConfig(
        const ASFW::Isoch::Audio::AudioStreamConfig& streamConfig) noexcept;
};

// Backend cadence + wire encoder. The shared HardwareSampleTimeline supplies
// the absolute frame and presentation time for every plan.
class DiceTxStreamEngine final {
public:
    DiceTxStreamEngine() noexcept = default;

    [[nodiscard]] bool Configure(
        const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
        const ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept;
    void BindSlotProvider(AMDTP::IAmdtpTxSlotProvider* slotProvider) noexcept;
    void BindPcmSource(ASFW::Audio::Ports::ITxPcmSource* pcmSource) noexcept;
    void ResetForStart(uint8_t initialDbc) noexcept;

    // Preview never consumes cadence. A successful PrepareTransmitSlot commits
    // cadence/DBC only after the transport slot has been published.
    [[nodiscard]] bool PreviewPresentationPlan(
        uint64_t epoch,
        uint64_t cycleOrdinal,
        uint64_t firstAudioFrame,
        uint64_t presentationBusTicks,
        const AMDTP::AmdtpTimingState& timing,
        AMDTP::TxPresentationPlan& outPlan,
        uint8_t& outWireDataBlocks,
        uint16_t& outSyt) const noexcept;
    [[nodiscard]] TxSlotPrepareResult PrepareTransmitSlot(
        uint32_t packetIndex,
        const AMDTP::TxPresentationPlan& plan,
        uint8_t wireDataBlocks,
        uint16_t syt) noexcept;

    [[nodiscard]] AMDTP::AmdtpPacketTimeline& Timeline() noexcept;
    [[nodiscard]] const AMDTP::AmdtpPacketTimeline& Timeline() const noexcept;
    [[nodiscard]] const AMDTP::AmdtpStreamConfig& StreamConfig() const noexcept;
    [[nodiscard]] const DiceTxEngineCounters& Counters() const noexcept;

private:
    static constexpr uint32_t kMaxPcmSnapshotSamples =
        ASFW::Encoding::kMaxPcmChannels *
        ASFW::Audio::Shared::kMaxBlockingFramesPerDataPacket;
    [[nodiscard]] AMDTP::AmdtpTxPolicy BuildTxPolicy(
        const ASFW::Isoch::Audio::AudioStreamTxPolicy& policy) const noexcept;

    ASFW::Isoch::Audio::AudioStreamConfig streamConfig_{};
    ASFW::Isoch::Audio::AudioStreamTxPolicy txPolicy_{};
    AMDTP::AmdtpTxPacketizer packetizer_{};
    AMDTP::BlockingCadence blockingCadence_{};
    AMDTP::NonBlocking48kCadence nonBlockingCadence_{};
    AMDTP::IAmdtpCadence* cadence_{nullptr};
    AMDTP::PacketTimelineSlot timelineSlots_[
        ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots]{};
    AMDTP::AmdtpPacketTimeline timeline_{};
    AMDTP::IAmdtpTxSlotProvider* slotProvider_{nullptr};
    ASFW::Audio::Ports::ITxPcmSource* pcmSource_{nullptr};
    std::array<float, kMaxPcmSnapshotSamples> pcmScratch_{};
    DiceTxEngineCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::DICE
