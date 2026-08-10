#pragma once

#include "AmdtpTypes.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

enum class PacketSlotState : uint8_t {
    Empty = 0,
    Finalized = 1,
    Published = 2,
};

struct PacketTimelineSlot final {
    uint32_t packetIndex{0};

    uint32_t packetSizeBytes{0};

    bool isData{false};
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint32_t dbs{0};

    std::atomic<PacketSlotState> state{PacketSlotState::Empty};
};

class AmdtpPacketTimeline final {
public:
    AmdtpPacketTimeline() noexcept = default;

    void Reset() noexcept;

    bool AttachSlots(PacketTimelineSlot* slots,
                     uint32_t slotCount) noexcept;

    bool MarkDataPacketFinalized(const PreparedTxPacket& packet) noexcept;

    void MarkNoDataPacket(uint32_t packetIndex) noexcept;
    void MarkPublished(uint32_t packetIndex) noexcept;

    PacketTimelineSlot* SlotByIndex(uint32_t packetIndex) noexcept;
    const PacketTimelineSlot* SlotByIndex(uint32_t packetIndex) const noexcept;

    [[nodiscard]] uint32_t SlotCount() const noexcept;

    [[nodiscard]] uint64_t FinalizedFrameEnd() const noexcept;

private:
    PacketTimelineSlot* slots_{nullptr};
    uint32_t slotCount_{0};
    // The only cross-queue timeline field. Packet-slot detail remains owned by
    // the TX preparation queue; diagnostics consume this atomic high-water.
    std::atomic<uint64_t> finalizedFrameEnd_{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP
