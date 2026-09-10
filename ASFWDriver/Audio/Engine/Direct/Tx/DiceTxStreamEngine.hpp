#pragma once

#include "../../../DriverKit/Config/AudioStreamProfile.hpp"
#include "../../../Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "../../../Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../../../Wire/MOTU/MotuEventOffsetCache.hpp"
#include "../../../Wire/MOTU/MotuPayloadWriter.hpp"
#include "../../../Wire/MOTU/MotuTxTiming.hpp"
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

    /// MOTU only: the receive side's per-data-block SPH offsets, drained one run per
    /// transmitted data packet. Without it a MOTU stream cannot be stamped and its
    /// packets are published unstamped rather than with invented timing.
    void BindMotuOffsetCache(::ASFW::Encoding::Motu::MotuEventOffsetCache* cache) noexcept;

    void ResetForStart(uint8_t initialDbc,
                       uint64_t initialAudioFrame) noexcept;

    [[nodiscard]] bool AlignFrameCursorOnce(uint64_t frameIndex) noexcept;

    // Re-arm the one-shot frame-cursor alignment after an RX replay stall so the
    // next DATA packet re-projects the cursor to the live frame (see
    // AmdtpTxPacketizer::ReArmFrameCursorAlignment).
    void ReArmFrameCursorAlignment() noexcept;

    [[nodiscard]] bool IsFrameCursorAligned() const noexcept;

    [[nodiscard]] TxSlotPrepareResult PrepareNextTransmitSlot(
        uint32_t packetIndex,
        const AMDTP::AmdtpTimingState& timing) noexcept;
    [[nodiscard]] bool NextPacketWouldCarryData() const noexcept;

    void WriteHostOutputFloat32(const AMDTP::HostAudioBufferView& hostBuffer,
                                uint64_t completionCursor) noexcept;

    [[nodiscard]] AMDTP::AmdtpPacketTimeline& Timeline() noexcept;
    [[nodiscard]] const AMDTP::AmdtpPacketTimeline& Timeline() const noexcept;

    [[nodiscard]] const AMDTP::AmdtpStreamConfig& StreamConfig() const noexcept;

    [[nodiscard]] AMDTP::AmdtpTxPacketizerTelemetrySnapshot
    PacketizerTelemetrySnapshot() const noexcept;

    [[nodiscard]] const DiceTxEngineCounters& Counters() const noexcept;

    [[nodiscard]] const AMDTP::AmdtpPayloadWriterCounters&
    PayloadWriterCounters() const noexcept;

private:
    /// Replay one cached SPH offset onto each data block of a prepared MOTU packet.
    void StampMotuSph(const AMDTP::TxPacketSlotView& slot,
                      const AMDTP::PreparedTxPacket& packet) noexcept;

    AMDTP::AmdtpTxPolicy BuildTxPolicy(
        const ASFW::Isoch::Audio::AudioStreamTxPolicy& policy) const noexcept;

    const ASFW::Isoch::Audio::IAudioStreamProfile* profile_{nullptr};

    ASFW::Isoch::Audio::AudioStreamConfig streamConfig_{};
    ASFW::Isoch::Audio::AudioStreamTxPolicy txPolicy_{};

    AMDTP::AmdtpTxPacketizer packetizer_{};
    AMDTP::AmdtpPayloadWriter payloadWriter_{};

    // MOTU's samples are 3-byte chunks behind a per-block SPH quadlet, so it needs its
    // own payload writer rather than a PcmSlotEncoding variant. Selected by
    // isMotu_ at Configure time; the AMDTP writer is left untouched for every other
    // family.
    ::ASFW::Encoding::Motu::MotuPayloadWriter motuPayloadWriter_{};
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* motuOffsetCache_{nullptr};
    bool isMotu_{false};
    uint32_t motuPcmChunks_{0};

    AMDTP::PacketTimelineSlot
        timelineSlots_[ASFW::IsochTransport::AudioTimingGeometry::kTimelineSlots]{};
    AMDTP::AmdtpPacketTimeline timeline_{};

    AMDTP::IAmdtpTxSlotProvider* slotProvider_{nullptr};

    DiceTxEngineCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::DICE
