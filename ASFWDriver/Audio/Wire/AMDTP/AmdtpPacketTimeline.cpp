#include "AmdtpPacketTimeline.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

void AmdtpPacketTimeline::Reset() noexcept {
    finalizedFrameEnd_.store(0, std::memory_order_relaxed);
    if (!slots_) {
        return;
    }
    for (uint32_t index = 0; index < slotCount_; ++index) {
        auto& slot = slots_[index];
        slot.packetIndex = 0;
        slot.packetSizeBytes = 0;
        slot.isData = false;
        slot.firstAudioFrame = 0;
        slot.framesInPacket = 0;
        slot.dbs = 0;
        slot.state.store(PacketSlotState::Empty, std::memory_order_relaxed);
    }
}

bool AmdtpPacketTimeline::AttachSlots(PacketTimelineSlot* slots,
                                      uint32_t slotCount) noexcept {
    if (!slots || slotCount == 0) {
        return false;
    }
    slots_ = slots;
    slotCount_ = slotCount;
    Reset();
    return true;
}

bool AmdtpPacketTimeline::MarkDataPacketFinalized(
    const PreparedTxPacket& packet) noexcept {
    if (!slots_ || !packet.isData || !packet.pcmFinalized ||
        packet.framesInPacket == 0) {
        return false;
    }
    auto& slot = slots_[packet.packetIndex % slotCount_];
    slot.packetIndex = packet.packetIndex;
    slot.packetSizeBytes = packet.byteCount;
    slot.isData = true;
    slot.firstAudioFrame = packet.firstAudioFrame;
    slot.framesInPacket = packet.framesInPacket;
    slot.dbs = packet.dbs;
    slot.state.store(PacketSlotState::Finalized, std::memory_order_release);

    const uint64_t frameEnd =
        packet.firstAudioFrame + packet.framesInPacket;
    const uint64_t previous =
        finalizedFrameEnd_.load(std::memory_order_relaxed);
    if (frameEnd > previous) {
        finalizedFrameEnd_.store(frameEnd, std::memory_order_release);
    }
    return true;
}

void AmdtpPacketTimeline::MarkNoDataPacket(uint32_t packetIndex) noexcept {
    if (!slots_) {
        return;
    }
    auto& slot = slots_[packetIndex % slotCount_];
    slot.packetIndex = packetIndex;
    slot.packetSizeBytes = 0;
    slot.isData = false;
    slot.firstAudioFrame = 0;
    slot.framesInPacket = 0;
    slot.dbs = 0;
    slot.state.store(PacketSlotState::Finalized, std::memory_order_release);
}

void AmdtpPacketTimeline::MarkPublished(uint32_t packetIndex) noexcept {
    auto* slot = SlotByIndex(packetIndex);
    if (slot && slot->state.load(std::memory_order_acquire) ==
                    PacketSlotState::Finalized) {
        slot->state.store(PacketSlotState::Published,
                          std::memory_order_release);
    }
}

const PacketTimelineSlot* AmdtpPacketTimeline::SlotByIndex(
    uint32_t packetIndex) const noexcept {
    if (!slots_) {
        return nullptr;
    }
    const auto& slot = slots_[packetIndex % slotCount_];
    if (slot.state.load(std::memory_order_acquire) == PacketSlotState::Empty ||
        slot.packetIndex != packetIndex) {
        return nullptr;
    }
    return &slot;
}

PacketTimelineSlot* AmdtpPacketTimeline::SlotByIndex(
    uint32_t packetIndex) noexcept {
    return const_cast<PacketTimelineSlot*>(
        static_cast<const AmdtpPacketTimeline*>(this)->SlotByIndex(
            packetIndex));
}

uint32_t AmdtpPacketTimeline::SlotCount() const noexcept {
    return slotCount_;
}

uint64_t AmdtpPacketTimeline::FinalizedFrameEnd() const noexcept {
    return finalizedFrameEnd_.load(std::memory_order_acquire);
}

} // namespace ASFW::Protocols::Audio::AMDTP
