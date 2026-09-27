#pragma once

#include "../../../DriverKit/Config/AudioStreamProfile.hpp"
#include "../../../Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "../../../Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../../../Ports/IAmdtpTxSlotProvider.hpp"
#include "../../../Ports/IWirePayloadCodec.hpp"
#include "../../../../Shared/Isoch/AudioTimingGeometry.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace ASFW::Protocols::Audio::DICE {

struct DiceTxEngineCounters final {
    std::atomic<uint64_t> packetsPrepared{0};
    std::atomic<uint64_t> dataPacketsPrepared{0};
    std::atomic<uint64_t> noDataPacketsPrepared{0};
    std::atomic<uint64_t> slotAcquireFailures{0};
    std::atomic<uint64_t> timingUnavailableReverts{0};
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
    using TimingLossCallback = std::function<bool()>; // true when notification was sent; false allows retry

    static constexpr uint32_t kMaxConsecutiveTimingReverts = 16;

    DiceTxStreamEngine() noexcept = default;

    /// `devicePlaybackMap`: where this device wants each PCM channel in the data
    /// block, when the device reported it (BeBoB channel positions). Empty =
    /// use the profile's map (identity for every static profile today).
    bool Configure(const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
                   const ASFW::Isoch::Audio::AudioStreamConfig& txConfig,
                   const ::ASFW::Audio::Wire::PcmSlotMap& devicePlaybackMap = {}) noexcept;

    void BindSlotProvider(AMDTP::IAmdtpTxSlotProvider* slotProvider) noexcept;

    void SetPayloadWriter(::ASFW::Audio::ITxPayloadWriter* writer) noexcept {
        activePayloadWriter_ = writer ? writer : &payloadWriter_;
    }

    void BindTimingStamper(::ASFW::Audio::ITxDeviceTimingStamper* stamper) noexcept {
        timingStamper_ = stamper;
    }

    void SetTimingLossCallback(TimingLossCallback callback) noexcept {
        timingLossCallback_ = std::move(callback);
    }

    void ResetForStart(uint8_t initialDbc,
                       uint64_t initialAudioFrame) noexcept;

    [[nodiscard]] bool AlignFrameCursorOnce(uint64_t frameIndex) noexcept;

    // Re-arm the one-shot frame-cursor alignment after an RX replay stall so the
    // next DATA packet re-projects the cursor to the live frame.
    void ReArmFrameCursorAlignment() noexcept;

    [[nodiscard]] bool IsFrameCursorAligned() const noexcept;
    [[nodiscard]] uint64_t NextAudioFrame() const noexcept { return nextAudioFrame_; }
    [[nodiscard]] uint64_t CursorEpoch() const noexcept { return cursorEpoch_; }

    /// True for families that carry no presentation time in the CIP SYT field -- Linux's
    /// CIP_UNAWARE_SYT (e.g. SPH-based timing).
    [[nodiscard]] bool IsSytUnaware() const noexcept {
        return timingStamper_ != nullptr && timingStamper_->IsSytUnaware();
    }

    [[nodiscard]] TxSlotPrepareResult PrepareNextTransmitSlot(
        uint64_t packetIndex,
        const AMDTP::AmdtpTimingState& timing) noexcept;
    [[nodiscard]] bool NextPacketWouldCarryData() const noexcept;

    // Copies host output into the packets that carry those frames (the TX
    // fill, run by the producer; documentation/TX_OWNERSHIP.md). Frames whose
    // packet is below firstWritablePacket keep their armed silence.
    void FillFromHostOutput(const AMDTP::HostAudioBufferView& hostBuffer,
                            uint64_t firstWritablePacket) noexcept;

    [[nodiscard]] AMDTP::AmdtpPacketTimeline& Timeline() noexcept;
    [[nodiscard]] const AMDTP::AmdtpPacketTimeline& Timeline() const noexcept;

    [[nodiscard]] const AMDTP::AmdtpStreamConfig& StreamConfig() const noexcept;

    [[nodiscard]] const DiceTxEngineCounters& Counters() const noexcept;
    // The built-in AMDTP writer's counters (fill health; a MOTU stream uses
    // its own writer and counters).
    [[nodiscard]] const AMDTP::AmdtpPayloadWriterCounters& PayloadWriterCounters() const noexcept {
        return payloadWriter_.Counters();
    }
    // S_out headroom since the last call, in packets (INT64_MAX: nothing
    // written); the interval restarts.
    [[nodiscard]] int64_t TakeMinFinalityMarginPackets() noexcept {
        return payloadWriter_.TakeMinFinalityMarginPackets();
    }

    AMDTP::AmdtpTxPolicy BuildTxPolicy(
        const ASFW::Isoch::Audio::AudioStreamTxPolicy& policy) const noexcept;

    const ASFW::Isoch::Audio::IAudioStreamProfile* profile_{nullptr};

    ASFW::Isoch::Audio::AudioStreamConfig streamConfig_{};
    ASFW::Isoch::Audio::AudioStreamTxPolicy txPolicy_{};

    AMDTP::AmdtpTxPacketizer packetizer_{};
    AMDTP::AmdtpPayloadWriter payloadWriter_{};
    ::ASFW::Audio::ITxPayloadWriter* activePayloadWriter_{&payloadWriter_};
    ::ASFW::Audio::ITxDeviceTimingStamper* timingStamper_{nullptr};

    uint64_t nextAudioFrame_{0};
    uint64_t cursorEpoch_{1};
    bool frameCursorAligned_{false};

    AMDTP::PacketTimelineSlot
        timelineSlots_[ASFW::IsochTransport::AudioTimingGeometry::kTimelineSlots]{};
    AMDTP::AmdtpPacketTimeline timeline_{};

    AMDTP::IAmdtpTxSlotProvider* slotProvider_{nullptr};

    DiceTxEngineCounters counters_{};

    uint32_t consecutiveTimingReverts_{0};
    TimingLossCallback timingLossCallback_{};
    bool timingLossReported_{false};
};

} // namespace ASFW::Protocols::Audio::DICE
