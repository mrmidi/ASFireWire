#include "AmdtpPacketTimeline.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

// Design decisions (see ../../../README.md, Step 2):
//
// 1. packetIndex is absolute and monotonic; the ring mapping
//    (packetIndex % slotCount_) is internal to the timeline.
// 2. FindSlotForAudioFrame is a bounded scan: the frame→slot mapping is not
//    arithmetic in blocking mode (no-data ring positions carry zero frames).
// 3. The generation counter is a per-slot seqlock sequence (odd while a
//    mutation is in flight, even when stable). It is load-bearing NOW: the
//    ADK IO handler runs on a real-time thread while the ZTS-timer pump
//    rewrites slots on the work queue. The RT side must read slots only via
//    SnapshotSlotForAudioFrame, which validates the sequence around a field
//    copy — re-reading a live slot after lookup is exactly the race that
//    produced the M3 WriteBE32 wild-pointer crash (stale range check, then
//    a reused slot's newer firstAudioFrame underflowing frameInPacket).
// 4. State ownership: the timeline owns Empty→ExposedForAudio (expose) and
//    retirement; the provider path owns →Published. No-data ring positions go
//    straight to Completed and are invisible to frame lookup.
// 5. ExposedFrameEnd() is a monotonic high-water mark: one past the highest
//    audio frame ever exposed. It survives publication, retirement, and ring
//    eviction, so a frame-lookup miss can be classified as "not produced yet"
//    (at/beyond the mark) versus "no longer writable" (below it). Cleared
//    only by Reset()/AttachSlots().
//
// Ordering contract (Linux-style seqlock; the slot fields stay plain data):
// every mutation is bracketed by BeginSlotWrite (generation++ to odd, then a
// release fence) and EndSlotWrite (release store back to even). A concurrent
// snapshot reader loads generation (acquire), copies the fields, issues an
// acquire fence, and re-loads generation — equal-and-even means the copy is
// consistent. Torn intermediate field reads are possible during a race but
// are discarded by the recheck; they never reach the caller.

void AmdtpPacketTimeline::BeginSlotWrite(PacketTimelineSlot& slot) noexcept {
    slot.generation.fetch_add(1, std::memory_order_relaxed); // now odd
    std::atomic_thread_fence(std::memory_order_release);
}

void AmdtpPacketTimeline::EndSlotWrite(PacketTimelineSlot& slot) noexcept {
    slot.generation.fetch_add(1, std::memory_order_release); // even again
}

void AmdtpPacketTimeline::Reset() noexcept {
    exposedFrameEnd_.store(0, std::memory_order_relaxed);
    if (slots_ == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < slotCount_; ++i) {
        PacketTimelineSlot& slot = slots_[i];
        BeginSlotWrite(slot);
        slot.packetIndex = 0;
        slot.packetBytes = nullptr;
        slot.packetCapacityBytes = 0;
        slot.packetSizeBytes = 0;
        slot.isData = false;
        slot.firstAudioFrame = 0;
        slot.framesInPacket = 0;
        slot.dbs = 0;
        slot.state.store(PacketSlotState::Empty, std::memory_order_relaxed);
        EndSlotWrite(slot);
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

    BeginSlotWrite(slot);
    slot.packetIndex = packet.packetIndex;
    slot.packetBytes = packetBytes;
    slot.packetCapacityBytes = packetCapacityBytes;
    slot.packetSizeBytes = packet.byteCount;
    slot.isData = true;
    slot.firstAudioFrame = packet.firstAudioFrame;
    slot.framesInPacket = packet.framesInPacket;
    slot.dbs = packet.dbs;
    slot.state.store(PacketSlotState::ExposedForAudio, std::memory_order_relaxed);
    EndSlotWrite(slot);

    const uint64_t frameEnd = packet.firstAudioFrame + packet.framesInPacket;
    if (frameEnd > exposedFrameEnd_.load(std::memory_order_relaxed)) {
        exposedFrameEnd_.store(frameEnd, std::memory_order_release);
    }
    return true;
}

void AmdtpPacketTimeline::MarkNoDataPacket(uint32_t packetIndex) noexcept {
    if (slots_ == nullptr) {
        return;
    }
    PacketTimelineSlot& slot = slots_[packetIndex % slotCount_];

    BeginSlotWrite(slot);
    slot.packetIndex = packetIndex;
    slot.packetBytes = nullptr;
    slot.packetCapacityBytes = 0;
    slot.packetSizeBytes = 0;
    slot.isData = false;
    slot.firstAudioFrame = 0;
    slot.framesInPacket = 0;
    slot.dbs = 0;
    // Retired immediately: never enters frame lookup.
    slot.state.store(PacketSlotState::Completed, std::memory_order_relaxed);
    EndSlotWrite(slot);
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

bool AmdtpPacketTimeline::SnapshotSlotForAudioFrame(
    uint64_t absoluteFrame, PacketSlotSnapshot& out) const noexcept {
    if (slots_ == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < slotCount_; ++i) {
        const PacketTimelineSlot& slot = slots_[i];

        const uint32_t gen = slot.generation.load(std::memory_order_acquire);
        if ((gen & 1u) != 0) {
            continue; // mutation in flight: this slot is being rewritten
        }

        const PacketSlotState state = slot.state.load(std::memory_order_relaxed);
        const bool isData = slot.isData;
        uint8_t* packetBytes = slot.packetBytes;
        const uint32_t packetSizeBytes = slot.packetSizeBytes;
        const uint64_t firstAudioFrame = slot.firstAudioFrame;
        const uint32_t framesInPacket = slot.framesInPacket;
        const uint32_t dbs = slot.dbs;

        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.generation.load(std::memory_order_relaxed) != gen) {
            continue; // rewritten under us: the copy may be torn, discard it
        }

        if (state != PacketSlotState::ExposedForAudio || !isData) {
            continue;
        }
        if (absoluteFrame < firstAudioFrame ||
            absoluteFrame >= firstAudioFrame + framesInPacket) {
            continue;
        }
        if (packetBytes == nullptr || dbs == 0) {
            continue;
        }

        out.slot = &slot;
        out.generation = gen;
        out.packetBytes = packetBytes;
        out.packetSizeBytes = packetSizeBytes;
        out.firstAudioFrame = firstAudioFrame;
        out.framesInPacket = framesInPacket;
        out.dbs = dbs;
        return true;
    }
    return false;
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
    return exposedFrameEnd_.load(std::memory_order_relaxed);
}

} // namespace ASFW::Protocols::Audio::AMDTP
