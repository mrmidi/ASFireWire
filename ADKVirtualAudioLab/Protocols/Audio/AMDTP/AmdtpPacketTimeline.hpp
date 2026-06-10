#pragma once

#include "AmdtpTypes.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

enum class PacketSlotState : uint8_t {
    Empty = 0,
    ExposedForAudio = 1,
    Published = 2,
    Completed = 3,
};

struct PacketTimelineSlot final {
    uint32_t packetIndex{0};

    uint8_t* packetBytes{nullptr};
    uint32_t packetCapacityBytes{0};
    uint32_t packetSizeBytes{0};

    bool isData{false};
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint32_t dbs{0};

    std::atomic<uint32_t> generation{0};
    std::atomic<PacketSlotState> state{PacketSlotState::Empty};
};

class AmdtpPacketTimeline final {
public:
    AmdtpPacketTimeline() noexcept = default;

    void Reset() noexcept;

    bool AttachSlots(PacketTimelineSlot* slots,
                     uint32_t slotCount) noexcept;

    bool ExposeDataPacket(const PreparedTxPacket& packet,
                          uint8_t* packetBytes,
                          uint32_t packetCapacityBytes) noexcept;

    void MarkNoDataPacket(uint32_t packetIndex) noexcept;

    PacketTimelineSlot* FindSlotForAudioFrame(uint64_t absoluteFrame) noexcept;
    const PacketTimelineSlot* FindSlotForAudioFrame(uint64_t absoluteFrame) const noexcept;

    PacketTimelineSlot* SlotByIndex(uint32_t packetIndex) noexcept;
    const PacketTimelineSlot* SlotByIndex(uint32_t packetIndex) const noexcept;

    [[nodiscard]] uint32_t SlotCount() const noexcept;

    [[nodiscard]] uint64_t ExposedFrameEnd() const noexcept;

private:
    PacketTimelineSlot* slots_{nullptr};
    uint32_t slotCount_{0};
    uint64_t exposedFrameEnd_{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP