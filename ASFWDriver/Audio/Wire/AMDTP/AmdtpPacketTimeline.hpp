#pragma once

#include "AmdtpTypes.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace ASFW::Protocols::Audio::AMDTP {

enum class PacketSlotState : uint8_t {
    Empty = 0,
    ExposedForAudio = 1,
    Completed = 3,
};

static_assert(std::atomic<uint64_t>::is_always_lock_free &&
              std::atomic<uint32_t>::is_always_lock_free &&
              std::atomic<uint8_t*>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free &&
              std::atomic<PacketSlotState>::is_always_lock_free);

struct PacketTimelineSlot final {
    // Atomic fields make an overlapping snapshot/rewrite legal in C++.
    // The generation sequence validates that they all belong to one version.
    std::atomic<uint64_t> packetIndex{0};
    std::atomic<uint8_t*> packetBytes{nullptr};
    std::atomic<uint32_t> packetCapacityBytes{0};
    std::atomic<uint32_t> packetSizeBytes{0};
    std::atomic<bool> isData{false};
    std::atomic<uint64_t> firstAudioFrame{0};
    std::atomic<uint32_t> framesInPacket{0};
    std::atomic<uint32_t> dbs{0};

    // Odd during a rewrite; even when stable. Fields use relaxed accesses
    // inside the sequence's release/acquire fences.
    std::atomic<uint32_t> generation{0};
    std::atomic<PacketSlotState> state{PacketSlotState::Empty};
};

// Consistent copy of one slot, taken under the generation seqlock. The only
// live pointer is `slot`, kept for the post-write reuse recheck; all field
// reads must go through the snapshot, never back through the slot.
struct PacketSlotSnapshot final {
    const PacketTimelineSlot* slot{nullptr};
    uint32_t generation{0};
    uint64_t packetIndex{0};
    uint8_t* packetBytes{nullptr};
    uint32_t packetSizeBytes{0};
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint32_t dbs{0};
};

class AmdtpPacketTimeline final {
public:
    AmdtpPacketTimeline() noexcept = default;

    // Attach/destroy require a quiescent reader. Reset invalidates all mappings
    // and cached generations; it does not free the shared packet storage.
    void Reset() noexcept;
    // Invalidate audio mappings on realignment while retaining final packet
    // metadata for producer-side DMA publication, MIDI and clock reporting.
    void BeginFrameEpoch() noexcept;

    bool AttachSlots(PacketTimelineSlot* slots,
                     uint32_t slotCount,
                     uint32_t maxFramesPerPacket = 32) noexcept;

    // Publish only after all packet-byte preparation (including timing) is
    // final. DATA frames are contiguous within an epoch; a discontinuity
    // starts a new epoch and invalidates the previous mappings.
    bool ExposeDataPacket(const PreparedTxPacket& packet,
                          uint8_t* packetBytes,
                          uint32_t packetCapacityBytes) noexcept;

    void MarkNoDataPacket(uint64_t packetIndex) noexcept;
    // One indexed candidate, validated by packet identity, frame range and
    // generation. No scan, allocation, lock or retry on the RT thread.
    [[nodiscard]] bool SnapshotSlotForAudioFrame(uint64_t absoluteFrame,
                                                 PacketSlotSnapshot& out) const noexcept;

    // Producer-queue lookup only; RT consumers use a validated snapshot.
    PacketTimelineSlot* SlotByIndex(uint64_t packetIndex) noexcept;
    const PacketTimelineSlot* SlotByIndex(uint64_t packetIndex) const noexcept;

    [[nodiscard]] uint32_t SlotCount() const noexcept;

    [[nodiscard]] uint64_t ExposedFrameEnd() const noexcept;

private:
    static void BeginSlotWrite(PacketTimelineSlot& slot) noexcept;
    static void EndSlotWrite(PacketTimelineSlot& slot) noexcept;

    // packetIndex+1, with zero meaning absent. No payload pointers or second
    // packet metadata copy. Capacity covers slotCount * maxFramesPerPacket.
    std::unique_ptr<std::atomic<uint64_t>[]> frameIndex_{};
    uint32_t indexCapacity_{0};
    uint32_t maxFramesPerPacket_{0};
    PacketTimelineSlot* slots_{nullptr};
    uint32_t slotCount_{0};
    // Written by the pump, read by the RT writer for miss classification.
    std::atomic<uint64_t> exposedFrameEnd_{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP
