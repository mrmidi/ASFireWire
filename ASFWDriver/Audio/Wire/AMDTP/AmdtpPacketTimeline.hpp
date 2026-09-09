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

struct ImageProvenance final {
    std::atomic<uint64_t> commitGeneration{0};
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint8_t pcmCopyResult{0};
};

struct PacketTimelineSlot final {
    uint32_t packetIndex{0};

    uint32_t packetSizeBytes{0};

    bool isData{false};
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint32_t plannedFrameCount{0};
    uint32_t dbs{0};
    uint64_t epoch{0};
    uint64_t cycleOrdinal{0};
    uint64_t presentationBusTicks{0};

    std::atomic<PacketSlotState> state{PacketSlotState::Empty};

    ImageProvenance images[2]{};
};

class AmdtpPacketTimeline final {
public:
    AmdtpPacketTimeline() noexcept = default;

    void Reset() noexcept;

    bool AttachSlots(PacketTimelineSlot* slots,
                     uint32_t slotCount) noexcept;

    bool MarkDataPacketFinalized(const PreparedTxPacket& packet) noexcept;

    void MarkNoDataPacket(const PreparedTxPacket& packet) noexcept;
    void MarkPublished(uint32_t packetIndex) noexcept;

    void SetImageProvenance(uint32_t packetIndex, uint8_t imageIndex,
                            uint64_t commitGeneration, uint64_t firstFrame,
                            uint32_t frameCount, uint8_t pcmCopyResult) noexcept;

    [[nodiscard]] bool ReadImageProvenance(uint32_t packetIndex, uint8_t imageIndex,
                                           uint64_t expectedCommitGen,
                                           ImageProvenance& out) const noexcept;

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
