#pragma once

#include "AmdtpPacketTimeline.hpp"
#include "AmdtpTypes.hpp"
#include "../IEC61883/CipHeader.hpp"
#include "../IEC61883/DbcCounter.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

// Pure wire encoder plus transactional DBC state. Absolute content time and
// cadence are supplied by TxPresentationPlan; this class owns neither.
class AmdtpTxPacketizer final {
public:
    AmdtpTxPacketizer() noexcept = default;

    [[nodiscard]] bool Configure(const AmdtpStreamConfig& streamConfig,
                                 const AmdtpTxPolicy& txPolicy) noexcept;
    void BindTimeline(AmdtpPacketTimeline* timeline) noexcept;
    void Reset(uint8_t initialDbc = 0) noexcept;

    // Encoding is side-effect-free. CommitPreparedPacket is called only after
    // the durable transport slot has been release-published.
    [[nodiscard]] bool PrepareDataPacket(
        TxPacketSlotView slot,
        const TxPresentationPlan& plan,
        uint8_t wireDataBlocks,
        uint16_t syt,
        const TxPcmSnapshotView& pcm,
        PreparedTxPacket& outPacket) noexcept;
    [[nodiscard]] bool CommitPreparedPacket(
        const PreparedTxPacket& packet,
        uint8_t wireDataBlocks) noexcept;

    [[nodiscard]] const AmdtpStreamConfig& StreamConfig() const noexcept;
    [[nodiscard]] const AmdtpTxPolicy& TxPolicy() const noexcept;

private:
    void WriteDataPacketDefaults(uint8_t* packetBytes,
                                 uint32_t packetCapacityBytes,
                                 uint32_t payloadBytes) noexcept;
    void WriteCadencePacketFill(uint8_t* packetBytes,
                                uint32_t payloadBytes) noexcept;
    void WritePcmSnapshot(uint8_t* packetBytes,
                          const PreparedTxPacket& packet,
                          const TxPcmSnapshotView& pcm) noexcept;
    void WriteCipHeader(uint8_t* packetBytes,
                        const IEC61883::CipHeaderWords& header) noexcept;

    AmdtpStreamConfig streamConfig_{};
    AmdtpTxPolicy txPolicy_{};
    IEC61883::CipHeaderBuilder cipBuilder_{};
    IEC61883::DbcCounter dbcCounter_{};
    AmdtpPacketTimeline* timeline_{nullptr};
};

} // namespace ASFW::Protocols::Audio::AMDTP
