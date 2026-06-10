#include "AmdtpPacketTimeline.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

// Design decisions (see ../../../README.md, Step 2):
//
// 1. packetIndex is absolute and monotonic; the ring mapping
//    (packetIndex % slotCount_) is internal to the timeline.
// 2. FindSlotForAudioFrame is a bounded scan: the frame→slot mapping is not
//    arithmetic in blocking mode (no-data ring positions carry zero frames).
// 3. Reusing a ring position bumps the slot's generation counter so a stale
//    reader that raced the wrap can detect the mismatch. Single-queue use
//    today makes this nearly moot; it becomes load-bearing across the
//    audio↔isoch boundary after graduation.
// 4. State ownership: the timeline owns Empty→ExposedForAudio (expose) and
//    retirement; the provider path owns →Published. No-data ring positions go
//    straight to Completed and are invisible to frame lookup.
// 5. ExposedFrameEnd() is a monotonic high-water mark: one past the highest
//    audio frame ever exposed. It survives publication, retirement, and ring
//    eviction, so a frame-lookup miss can be classified as "not produced yet"
//    (at/beyond the mark) versus "no longer writable" (below it). Cleared
//    only by Reset()/AttachSlots().
//
// Ordering contract: slot fields are plain data, published by the release
// store of `state` after the fields are written, and observed under the
// acquire load of `state` before the fields are read.

void AmdtpPacketTimeline::Reset() noexcept {
    exposedFrameEnd_ = 0;
    if (slots_ == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < slotCount_; ++i) {
        PacketTimelineSlot& slot = slots_[i];
        slot.packetIndex = 0;
        slot.packetBytes = nullptr;
        slot.packetCapacityBytes = 0;
        slot.packetSizeBytes = 0;
        slot.isData = false;
        slot.firstAudioFrame = 0;
        slot.framesInPacket = 0;
        slot.dbs = 0;
        slot.generation.store(0, std::memory_order_relaxed);
        slot.state.store(PacketSlotState::Empty, std::memory_order_release);
    }
}

bool AmdtpPacketTimeline::AttachSlots(PacketTimelineSlot* slots,
                                      uint32_t slotCount) noexcept {
    if (slots == nullptr || slotCount == 0) {
        return false;
    }
    slots_ = slots;
    slotCount_ = slotCount;
    Reset();
    return true;
}

bool AmdtpPacketTimeline::ExposeDataPacket(const PreparedTxPacket& packet,
                                           uint8_t* packetBytes,
                                           uint32_t packetCapacityBytes) noexcept {
    if (slots_ == nullptr || packetBytes == nullptr) {
        return false;
    }
    if (!packet.isData || packet.framesInPacket == 0) {
        return false;
    }
    if (packetCapacityBytes < packet.byteCount) {
        return false;
    }

    PacketTimelineSlot& slot = slots_[packet.packetIndex % slotCount_];

    // Ring-position reuse: invalidate the previous occupant first.
    if (slot.state.load(std::memory_order_relaxed) != PacketSlotState::Empty) {
        slot.generation.fetch_add(1, std::memory_order_relaxed);
        slot.state.store(PacketSlotState::Empty, std::memory_order_release);
    }

    slot.packetIndex = packet.packetIndex;
    slot.packetBytes = packetBytes;
    slot.packetCapacityBytes = packetCapacityBytes;
    slot.packetSizeBytes = packet.byteCount;
    slot.isData = true;
    slot.firstAudioFrame = packet.firstAudioFrame;
    slot.framesInPacket = packet.framesInPacket;
    slot.dbs = packet.dbs;

    slot.state.store(PacketSlotState::ExposedForAudio, std::memory_order_release);

    const uint64_t frameEnd = packet.firstAudioFrame + packet.framesInPacket;
    if (frameEnd > exposedFrameEnd_) {
        exposedFrameEnd_ = frameEnd;
    }
    return true;
}

void AmdtpPacketTimeline::MarkNoDataPacket(uint32_t packetIndex) noexcept {
    if (slots_ == nullptr) {
        return;
    }
    PacketTimelineSlot& slot = slots_[packetIndex % slotCount_];

    if (slot.state.load(std::memory_order_relaxed) != PacketSlotState::Empty) {
        slot.generation.fetch_add(1, std::memory_order_relaxed);
        slot.state.store(PacketSlotState::Empty, std::memory_order_release);
    }

    slot.packetIndex = packetIndex;
    slot.packetBytes = nullptr;
    slot.packetCapacityBytes = 0;
    slot.packetSizeBytes = 0;
    slot.isData = false;
    slot.firstAudioFrame = 0;
    slot.framesInPacket = 0;
    slot.dbs = 0;

    // Retired immediately: never enters frame lookup.
    slot.state.store(PacketSlotState::Completed, std::memory_order_release);
}

const PacketTimelineSlot*
AmdtpPacketTimeline::FindSlotForAudioFrame(uint64_t absoluteFrame) const noexcept {
    if (slots_ == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < slotCount_; ++i) {
        const PacketTimelineSlot& slot = slots_[i];
        if (slot.state.load(std::memory_order_acquire) !=
            PacketSlotState::ExposedForAudio) {
            continue;
        }
        if (!slot.isData) {
            continue;
        }
        if (absoluteFrame >= slot.firstAudioFrame &&
            absoluteFrame < slot.firstAudioFrame + slot.framesInPacket) {
            return &slot;
        }
    }
    return nullptr;
}

PacketTimelineSlot*
AmdtpPacketTimeline::FindSlotForAudioFrame(uint64_t absoluteFrame) noexcept {
    return const_cast<PacketTimelineSlot*>(
        static_cast<const AmdtpPacketTimeline*>(this)->FindSlotForAudioFrame(
            absoluteFrame));
}

const PacketTimelineSlot*
AmdtpPacketTimeline::SlotByIndex(uint32_t packetIndex) const noexcept {
    if (slots_ == nullptr) {
        return nullptr;
    }
    const PacketTimelineSlot& slot = slots_[packetIndex % slotCount_];
    if (slot.state.load(std::memory_order_acquire) == PacketSlotState::Empty) {
        return nullptr;
    }
    // The ring position may already belong to a different absolute index.
    if (slot.packetIndex != packetIndex) {
        return nullptr;
    }
    return &slot;
}

PacketTimelineSlot* AmdtpPacketTimeline::SlotByIndex(uint32_t packetIndex) noexcept {
    return const_cast<PacketTimelineSlot*>(
        static_cast<const AmdtpPacketTimeline*>(this)->SlotByIndex(packetIndex));
}

uint32_t AmdtpPacketTimeline::SlotCount() const noexcept {
    return slotCount_;
}

uint64_t AmdtpPacketTimeline::ExposedFrameEnd() const noexcept {
    return exposedFrameEnd_;
}

} // namespace ASFW::Protocols::Audio::AMDTP
