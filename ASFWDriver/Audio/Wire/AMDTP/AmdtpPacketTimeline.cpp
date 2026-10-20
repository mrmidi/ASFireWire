#include "AmdtpPacketTimeline.hpp"

#include <bit>
#include <new>

namespace ASFW::Protocols::Audio::AMDTP {

// The producer publishes a packet token for each DATA frame after preparing
// its bytes. The index spans at least one full packet ring's frame capacity.
// Continuous DATA frames therefore cannot alias another still-live packet;
// NO-DATA consumes packet positions but no frame-index entries. A frame jump
// starts a fresh epoch instead of relying on that continuity assumption.
//
// Entries may outlive their packet. Readers validate the absolute packet ID,
// absolute frame range and the slot generation; stale/wrapped tokens miss.
// Reset changes slot generations as well as clearing entries, invalidating
// the payload writer's per-packet cache. No raw payload pointer lives here.
//
// Snapshot fields are atomic: a generation check alone does not make racing
// plain C++ reads/writes legal. Release/acquire fences around relaxed field
// accesses detect a mixed version without a lock or an unbounded retry loop.
// This protects metadata, not payload ownership: the existing finality and
// shared-buffer lifetime contracts still apply to the caller's PCM writes.

void AmdtpPacketTimeline::BeginSlotWrite(PacketTimelineSlot& slot) noexcept {
    slot.generation.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

void AmdtpPacketTimeline::EndSlotWrite(PacketTimelineSlot& slot) noexcept {
    slot.generation.fetch_add(1, std::memory_order_release);
}

void AmdtpPacketTimeline::BeginFrameEpoch() noexcept {
    for (uint32_t i = 0; i < indexCapacity_; ++i) {
        frameIndex_[i].store(0, std::memory_order_relaxed);
    }
    exposedFrameEnd_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < slotCount_; ++i) {
        auto& slot = slots_[i];
        BeginSlotWrite(slot);
        if (slot.state.load(std::memory_order_relaxed) == PacketSlotState::ExposedForAudio) {
            slot.state.store(PacketSlotState::Completed, std::memory_order_relaxed);
        }
        EndSlotWrite(slot);
    }
}

void AmdtpPacketTimeline::Reset() noexcept {
    for (uint32_t i = 0; i < indexCapacity_; ++i) {
        frameIndex_[i].store(0, std::memory_order_relaxed);
    }
    exposedFrameEnd_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < slotCount_; ++i) {
        auto& slot = slots_[i];
        BeginSlotWrite(slot);
        slot.packetIndex.store(0, std::memory_order_relaxed);
        slot.packetBytes.store(nullptr, std::memory_order_relaxed);
        slot.packetCapacityBytes.store(0, std::memory_order_relaxed);
        slot.packetSizeBytes.store(0, std::memory_order_relaxed);
        slot.isData.store(false, std::memory_order_relaxed);
        slot.firstAudioFrame.store(0, std::memory_order_relaxed);
        slot.framesInPacket.store(0, std::memory_order_relaxed);
        slot.dbs.store(0, std::memory_order_relaxed);
        slot.state.store(PacketSlotState::Empty, std::memory_order_relaxed);
        EndSlotWrite(slot);
    }
}

bool AmdtpPacketTimeline::AttachSlots(PacketTimelineSlot* slots,
                                     uint32_t slotCount,
                                     uint32_t maxFramesPerPacket) noexcept {
    const uint64_t required = uint64_t{slotCount} * maxFramesPerPacket;
    if (slots == nullptr || slotCount == 0 || maxFramesPerPacket == 0 ||
        maxFramesPerPacket > UINT8_MAX || required > (uint64_t{1} << 31)) {
        return false;
    }
    const uint32_t capacity = std::bit_ceil(static_cast<uint32_t>(required));
    std::unique_ptr<std::atomic<uint64_t>[]> index{
        new (std::nothrow) std::atomic<uint64_t>[capacity]{}};
    if (!index) return false;
    frameIndex_ = std::move(index);
    indexCapacity_ = capacity;
    maxFramesPerPacket_ = maxFramesPerPacket;
    slots_ = slots;
    slotCount_ = slotCount;
    Reset();
    return true;
}

bool AmdtpPacketTimeline::ExposeDataPacket(const PreparedTxPacket& packet,
                                          uint8_t* packetBytes,
                                          uint32_t packetCapacityBytes) noexcept {
    if (slots_ == nullptr || packetBytes == nullptr || !packet.isData ||
        packet.framesInPacket == 0 || packet.framesInPacket > maxFramesPerPacket_ ||
        packetCapacityBytes < packet.byteCount || packet.dbs == 0 ||
        packet.packetIndex == UINT64_MAX ||
        packet.firstAudioFrame > UINT64_MAX - packet.framesInPacket) {
        return false;
    }
    const uint64_t previousEnd = exposedFrameEnd_.load(std::memory_order_relaxed);
    if (previousEnd != 0 && packet.firstAudioFrame != previousEnd) {
        BeginFrameEpoch(); // old and new frame windows must not alias
    }
    auto& slot = slots_[packet.packetIndex % slotCount_];
    BeginSlotWrite(slot);
    slot.packetIndex.store(packet.packetIndex, std::memory_order_relaxed);
    slot.packetBytes.store(packetBytes, std::memory_order_relaxed);
    slot.packetCapacityBytes.store(packetCapacityBytes, std::memory_order_relaxed);
    slot.packetSizeBytes.store(packet.byteCount, std::memory_order_relaxed);
    slot.isData.store(true, std::memory_order_relaxed);
    slot.firstAudioFrame.store(packet.firstAudioFrame, std::memory_order_relaxed);
    slot.framesInPacket.store(packet.framesInPacket, std::memory_order_relaxed);
    slot.dbs.store(packet.dbs, std::memory_order_relaxed);
    slot.state.store(PacketSlotState::ExposedForAudio, std::memory_order_relaxed);
    EndSlotWrite(slot);

    const uint64_t end = packet.firstAudioFrame + packet.framesInPacket;
    for (uint64_t frame = packet.firstAudioFrame; frame < end; ++frame) {
        frameIndex_[frame & (indexCapacity_ - 1)].store(
            packet.packetIndex + 1, std::memory_order_release);
    }
    exposedFrameEnd_.store(end, std::memory_order_release);
    return true;
}

void AmdtpPacketTimeline::MarkNoDataPacket(uint64_t packetIndex) noexcept {
    if (slots_ == nullptr) return;
    auto& slot = slots_[packetIndex % slotCount_];
    BeginSlotWrite(slot);
    slot.packetIndex.store(packetIndex, std::memory_order_relaxed);
    slot.packetBytes.store(nullptr, std::memory_order_relaxed);
    slot.packetCapacityBytes.store(0, std::memory_order_relaxed);
    slot.packetSizeBytes.store(0, std::memory_order_relaxed);
    slot.isData.store(false, std::memory_order_relaxed);
    slot.firstAudioFrame.store(0, std::memory_order_relaxed);
    slot.framesInPacket.store(0, std::memory_order_relaxed);
    slot.dbs.store(0, std::memory_order_relaxed);
    slot.state.store(PacketSlotState::Completed, std::memory_order_relaxed);
    EndSlotWrite(slot);
}

bool AmdtpPacketTimeline::SnapshotSlotForAudioFrame(
    uint64_t absoluteFrame, PacketSlotSnapshot& out) const noexcept {
    if (slots_ == nullptr) return false;
    const uint64_t token = frameIndex_[absoluteFrame & (indexCapacity_ - 1)].load(
        std::memory_order_acquire);
    if (token == 0) return false;
    const auto& slot = slots_[(token - 1) % slotCount_];
    const uint32_t gen = slot.generation.load(std::memory_order_acquire);
    if (gen & 1U) return false;

    const auto state = slot.state.load(std::memory_order_relaxed);
    PacketSlotSnapshot snap{
        .slot = &slot,
        .generation = gen,
        .packetIndex = slot.packetIndex.load(std::memory_order_relaxed),
        .packetBytes = slot.packetBytes.load(std::memory_order_relaxed),
        .packetSizeBytes = slot.packetSizeBytes.load(std::memory_order_relaxed),
        .firstAudioFrame = slot.firstAudioFrame.load(std::memory_order_relaxed),
        .framesInPacket = slot.framesInPacket.load(std::memory_order_relaxed),
        .dbs = slot.dbs.load(std::memory_order_relaxed),
    };
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.generation.load(std::memory_order_relaxed) != gen ||
        state != PacketSlotState::ExposedForAudio || snap.packetIndex != token - 1 ||
        absoluteFrame < snap.firstAudioFrame ||
        absoluteFrame - snap.firstAudioFrame >= snap.framesInPacket ||
        snap.packetBytes == nullptr || snap.dbs == 0) {
        return false;
    }
    out = snap;
    return true;
}

const PacketTimelineSlot* AmdtpPacketTimeline::SlotByIndex(uint64_t packetIndex) const noexcept {
    if (slots_ == nullptr) return nullptr;
    const auto& slot = slots_[packetIndex % slotCount_];
    if (slot.state.load(std::memory_order_relaxed) == PacketSlotState::Empty ||
        slot.packetIndex.load(std::memory_order_relaxed) != packetIndex) return nullptr;
    return &slot;
}

PacketTimelineSlot* AmdtpPacketTimeline::SlotByIndex(uint64_t packetIndex) noexcept {
    return const_cast<PacketTimelineSlot*>(
        static_cast<const AmdtpPacketTimeline*>(this)->SlotByIndex(packetIndex));
}

uint32_t AmdtpPacketTimeline::SlotCount() const noexcept { return slotCount_; }

uint64_t AmdtpPacketTimeline::ExposedFrameEnd() const noexcept {
    return exposedFrameEnd_.load(std::memory_order_acquire);
}

} // namespace ASFW::Protocols::Audio::AMDTP
