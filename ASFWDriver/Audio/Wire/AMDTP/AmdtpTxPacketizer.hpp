#pragma once

#include "AmdtpCadence.hpp"
#include "AmdtpPacketTimeline.hpp"
#include "AmdtpTypes.hpp"
#include "../../Ports/TxPresentationPlan.hpp"
#include "../IEC61883/CipHeader.hpp"
#include "../IEC61883/DbcCounter.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace ASFW::Protocols::Audio::AMDTP {

class AmdtpTxPacketizer final {
public:
    AmdtpTxPacketizer() noexcept = default;

    bool Configure(const AmdtpStreamConfig& streamConfig,
                   const AmdtpTxPolicy& txPolicy) noexcept;

    void BindTimeline(AmdtpPacketTimeline* timeline) noexcept;

    void Reset(uint8_t initialDbc = 0) noexcept;

    // Frames are not tracked here. The caller (DiceTxStreamEngine) owns the
    // one audio-frame cursor and states each packet's frames in the plan; the
    // packetizer frames it (cadence, DBC, CIP) and never remembers a frame.
    bool PrepareNextPacket(TxPacketSlotView slot,
                           const AmdtpTimingState& timing,
                           const TxPresentationPlan& plan,
                           PreparedTxPacket& outPacket) noexcept;

    // Called only after timing and provider publication succeed. Before this
    // step the RT writer cannot see the packet's mutable preparation bytes.
    [[nodiscard]] bool ExposePreparedPacket(TxPacketSlotView slot,
                                           const PreparedTxPacket& packet) noexcept;

    void RevertToNoData(TxPacketSlotView slot, PreparedTxPacket& packet) noexcept;

    [[nodiscard]] const AmdtpStreamConfig& StreamConfig() const noexcept;
    [[nodiscard]] const AmdtpTxPolicy& TxPolicy() const noexcept;
    [[nodiscard]] bool NextPacketWouldCarryData() const noexcept;
    [[nodiscard]] uint8_t CurrentCycleDataFrames() const noexcept;

private:
    // Builds the per-frame slot defaults from the stream config and TX
    // policy. Nothing in them changes between packets, so the hot path only
    // copies them.
    void BuildFrameDefaults() noexcept;

    void WriteDataPacketDefaults(uint8_t* packetBytes,
                                 uint32_t packetCapacityBytes,
                                 uint32_t payloadBytes) noexcept;

    void WriteCadencePacketFill(uint8_t* packetBytes,
                                uint32_t payloadBytes) noexcept;

    void WriteCipHeader(uint8_t* packetBytes,
                        const IEC61883::CipHeaderWords& header) noexcept;

    [[nodiscard]] uint32_t DataPacketBytes() const noexcept;
    [[nodiscard]] uint32_t PayloadBytes() const noexcept;

    AmdtpStreamConfig streamConfig_{};
    AmdtpTxPolicy txPolicy_{};

    // One data block's default contents, wire order (big-endian), built by
    // BuildFrameDefaults: encoded silence in the slots the playback map gives
    // to PCM, defaultNonAudioSlotWord in the slots it leaves free.
    static constexpr uint32_t kMaxSlots = 255;  // dbs is 8 bits
    std::array<uint8_t, kMaxSlots * 4> frameDefaults_{};
    // The non-audio slots, for policies that keep PCM bytes and only arm
    // the non-audio slots.
    std::array<uint8_t, kMaxSlots> nonAudioSlots_{};
    uint32_t nonAudioSlotCount_{0};

    IEC61883::CipHeaderBuilder cipBuilder_{};
    IEC61883::DbcCounter dbcCounter_{};

    BlockingCadence blockingCadence_{};
    NonBlockingCadence nonBlockingCadence_{};
    IAmdtpCadence* cadence_{nullptr};

    AmdtpPacketTimeline* timeline_{nullptr};
};

} // namespace ASFW::Protocols::Audio::AMDTP
