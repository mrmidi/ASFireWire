#pragma once

#include "../../../DriverKit/Config/AudioStreamProfile.hpp"
#include "../../../Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../../../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../../Ports/IAmdtpTxSlotProvider.hpp"
#include "../../../Ports/ITxPcmSource.hpp"
#include "../../../Shared/AudioHalBufferProfiles.hpp"
#include "../../../Shared/AudioTimingGeometry.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

struct DiceTxEngineCounters final {
    std::atomic<uint64_t> packetsPrepared{0};
    std::atomic<uint64_t> dataPacketsPrepared{0};
    std::atomic<uint64_t> noDataPacketsPrepared{0};
    std::atomic<uint64_t> slotAcquireFailures{0};
    std::atomic<uint64_t> pcmReadsReady{0};
    std::atomic<uint64_t> pcmReadsNotYetWritten{0};
    std::atomic<uint64_t> pcmReadsStaleOverwritten{0};
    std::atomic<uint64_t> pcmReadsSnapshotBusy{0};
    std::atomic<uint64_t> pcmReadsInvalid{0};
};

enum class TxSlotPrepareResult : uint8_t {
    kPrepared = 0,
    kSlotProviderUnavailable,
    kPcmSourceUnavailable,
    kPcmNotYetWritten,
    kPcmStaleOverwritten,
    kPcmSnapshotBusy,
    kPcmInvalidRequest,
    kSlotAcquireFailed,
    kPacketizerRejected,
    kSlotPublishFailed,
};

class DiceStreamConfigMapper final {
public:
    [[nodiscard]] static AMDTP::AmdtpStreamConfig ToAmdtpConfig(
        const ASFW::Isoch::Audio::AudioStreamConfig& streamConfig) noexcept;
};

class DiceTxStreamEngine final {
public:
    DiceTxStreamEngine() noexcept = default;

    bool Configure(const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
                   const ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept;

    void BindSlotProvider(AMDTP::IAmdtpTxSlotProvider* slotProvider) noexcept;
    void BindPcmSource(ASFW::Audio::Ports::ITxPcmSource* pcmSource) noexcept;

    void ResetForStart(uint8_t initialDbc,
                       uint64_t initialAudioFrame) noexcept;

    [[nodiscard]] bool AlignFrameCursorOnce(uint64_t frameIndex) noexcept;

    // Re-arm after a genuine RX timing-domain loss or an unrecoverably stale
    // PCM range so the next DATA packet re-projects to retained host content.
    // A future/busy PCM snapshot holds the cursor instead (see
    // AmdtpTxPacketizer::ReArmFrameCursorAlignment).
    void ReArmFrameCursorAlignment() noexcept;

    [[nodiscard]] bool IsFrameCursorAligned() const noexcept;

    [[nodiscard]] TxSlotPrepareResult PrepareNextTransmitSlot(
        uint32_t packetIndex,
        const AMDTP::AmdtpTimingState& timing) noexcept;
    [[nodiscard]] bool NextPacketWouldCarryData() const noexcept;

    [[nodiscard]] AMDTP::AmdtpPacketTimeline& Timeline() noexcept;
    [[nodiscard]] const AMDTP::AmdtpPacketTimeline& Timeline() const noexcept;

    [[nodiscard]] const AMDTP::AmdtpStreamConfig& StreamConfig() const noexcept;

    [[nodiscard]] AMDTP::AmdtpTxPacketizerTelemetrySnapshot
    PacketizerTelemetrySnapshot() const noexcept;

    [[nodiscard]] const DiceTxEngineCounters& Counters() const noexcept;

private:
    static constexpr uint32_t kMaxPcmSnapshotSamples =
        ASFW::Encoding::kMaxPcmChannels *
        ASFW::Audio::Shared::kMaxBlockingFramesPerDataPacket;
    AMDTP::AmdtpTxPolicy BuildTxPolicy(
        const ASFW::Isoch::Audio::AudioStreamTxPolicy& policy) const noexcept;

    const ASFW::Isoch::Audio::IAudioStreamProfile* profile_{nullptr};

    ASFW::Isoch::Audio::AudioStreamConfig streamConfig_{};
    ASFW::Isoch::Audio::AudioStreamTxPolicy txPolicy_{};

    AMDTP::AmdtpTxPacketizer packetizer_{};

    AMDTP::PacketTimelineSlot
        timelineSlots_[ASFW::Audio::Shared::AudioTimingGeometry::kTimelineSlots]{};
    AMDTP::AmdtpPacketTimeline timeline_{};

    AMDTP::IAmdtpTxSlotProvider* slotProvider_{nullptr};
    ASFW::Audio::Ports::ITxPcmSource* pcmSource_{nullptr};
    std::array<float, kMaxPcmSnapshotSamples> pcmScratch_{};

    DiceTxEngineCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::DICE
