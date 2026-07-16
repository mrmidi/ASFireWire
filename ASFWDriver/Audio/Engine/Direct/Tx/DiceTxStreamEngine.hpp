#pragma once

#include "../../../DriverKit/Config/AudioStreamProfile.hpp"
#include "../../../Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "../../../Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../../../Ports/IAmdtpTxSlotProvider.hpp"
#include "../../../../Shared/Isoch/AudioTimingGeometry.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

struct DiceTxEngineCounters final {
    std::atomic<uint64_t> packetsPrepared{0};
    std::atomic<uint64_t> dataPacketsPrepared{0};
    std::atomic<uint64_t> noDataPacketsPrepared{0};
    std::atomic<uint64_t> slotAcquireFailures{0};
};

enum class TxSlotPrepareResult : uint8_t {
    kPrepared = 0,
    kSlotProviderUnavailable,
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

    void ResetForStart(uint8_t initialDbc,
                       uint64_t initialAudioFrame) noexcept;

    [[nodiscard]] bool AlignFrameCursorOnce(uint64_t frameIndex) noexcept;

    // Re-arm the one-shot frame-cursor alignment after an RX replay stall so the
    // next DATA packet re-projects the cursor to the live frame (see
    // AmdtpTxPacketizer::ReArmFrameCursorAlignment).
    void ReArmFrameCursorAlignment() noexcept;

    [[nodiscard]] TxSlotPrepareResult PrepareNextTransmitSlot(
        uint32_t packetIndex,
        const AMDTP::AmdtpTimingState& timing) noexcept;
    [[nodiscard]] bool NextPacketWouldCarryData() const noexcept;

    void WriteHostOutputFloat32(const AMDTP::HostAudioBufferView& hostBuffer,
                                uint64_t completionCursor) noexcept;

    [[nodiscard]] AMDTP::AmdtpPacketTimeline& Timeline() noexcept;
    [[nodiscard]] const AMDTP::AmdtpPacketTimeline& Timeline() const noexcept;

    [[nodiscard]] const AMDTP::AmdtpStreamConfig& StreamConfig() const noexcept;

    [[nodiscard]] const DiceTxEngineCounters& Counters() const noexcept;

    [[nodiscard]] const AMDTP::AmdtpPayloadWriterCounters&
    PayloadWriterCounters() const noexcept;

private:
    AMDTP::AmdtpTxPolicy BuildTxPolicy(
        const ASFW::Isoch::Audio::AudioStreamTxPolicy& policy) const noexcept;

    const ASFW::Isoch::Audio::IAudioStreamProfile* profile_{nullptr};

    ASFW::Isoch::Audio::AudioStreamConfig streamConfig_{};
    ASFW::Isoch::Audio::AudioStreamTxPolicy txPolicy_{};

    AMDTP::AmdtpTxPacketizer packetizer_{};
    AMDTP::AmdtpPayloadWriter payloadWriter_{};

    AMDTP::PacketTimelineSlot
        timelineSlots_[ASFW::IsochTransport::AudioTimingGeometry::kTimelineSlots]{};
    AMDTP::AmdtpPacketTimeline timeline_{};

    AMDTP::IAmdtpTxSlotProvider* slotProvider_{nullptr};

    DiceTxEngineCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::DICE
