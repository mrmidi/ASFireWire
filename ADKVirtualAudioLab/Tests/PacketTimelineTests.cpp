#include "TestHarness.hpp"

#include "../Protocols/Audio/AMDTP/AmdtpPacketTimeline.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpTypes.hpp"

namespace ASFW::LabTests {

using namespace Protocols::Audio::AMDTP;

namespace {

// Canonical blocking-48k stereo data packet: 8 frames, dbs=2,
// 8 bytes CIP + 8*2*4 payload = 72 bytes.
PreparedTxPacket MakeDataPacket(uint32_t packetIndex, uint64_t firstFrame) {
    PreparedTxPacket packet{};
    packet.packetIndex = packetIndex;
    packet.byteCount = 72;
    packet.isData = true;
    packet.firstAudioFrame = firstFrame;
    packet.framesInPacket = 8;
    packet.dbs = 2;
    return packet;
}

} // namespace

void RunPacketTimelineTests(TestContext& ctx) {
    // --- AttachSlots validation ---
    {
        AmdtpPacketTimeline timeline;
        PacketTimelineSlot slots[4]{};
        CHECK(ctx, !timeline.AttachSlots(nullptr, 4));
        CHECK(ctx, !timeline.AttachSlots(slots, 0));
        CHECK(ctx, timeline.AttachSlots(slots, 4));
        CHECK_EQ_U32(ctx, timeline.SlotCount(), 4);
        // Not attached → no crash, no result
        AmdtpPacketTimeline empty;
        CHECK(ctx, empty.FindSlotForAudioFrame(0) == nullptr);
        CHECK(ctx, empty.SlotByIndex(0) == nullptr);
        CHECK(ctx, !empty.ExposeDataPacket(MakeDataPacket(0, 0), nullptr, 0));
    }

    // --- Expose validation ---
    {
        AmdtpPacketTimeline timeline;
        PacketTimelineSlot slots[4]{};
        uint8_t bytes[512]{};
        timeline.AttachSlots(slots, 4);

        CHECK(ctx, !timeline.ExposeDataPacket(MakeDataPacket(0, 0), nullptr, 512));
        // Capacity below byteCount rejected
        CHECK(ctx, !timeline.ExposeDataPacket(MakeDataPacket(0, 0), bytes, 71));
        // Non-data packet rejected
        PreparedTxPacket noData = MakeDataPacket(0, 0);
        noData.isData = false;
        CHECK(ctx, !timeline.ExposeDataPacket(noData, bytes, 512));
        // Zero frames rejected
        PreparedTxPacket zeroFrames = MakeDataPacket(0, 0);
        zeroFrames.framesInPacket = 0;
        CHECK(ctx, !timeline.ExposeDataPacket(zeroFrames, bytes, 512));
        // Valid expose accepted
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(0, 1000), bytes, 512));
    }

    // --- Frame lookup boundaries ---
    {
        AmdtpPacketTimeline timeline;
        PacketTimelineSlot slots[8]{};
        uint8_t bytes[8][512]{};
        timeline.AttachSlots(slots, 8);

        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(0, 1000), bytes[0], 512));

        const PacketTimelineSlot* slot = timeline.FindSlotForAudioFrame(1000);
        CHECK(ctx, slot != nullptr);
        CHECK(ctx, slot == timeline.FindSlotForAudioFrame(1007)); // last frame
        CHECK(ctx, timeline.FindSlotForAudioFrame(999) == nullptr);  // before
        CHECK(ctx, timeline.FindSlotForAudioFrame(1008) == nullptr); // after

        // Adjacent packets: each frame resolves to exactly its packet
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(1, 1008), bytes[1], 512));
        const PacketTimelineSlot* second = timeline.FindSlotForAudioFrame(1008);
        CHECK(ctx, second != nullptr && second != slot);
        CHECK_EQ_U32(ctx, second->packetIndex, 1);
        CHECK_EQ_U64(ctx, second->firstAudioFrame, 1008);
        CHECK(ctx, timeline.FindSlotForAudioFrame(1007) == slot);
    }

    // --- Mixed data / no-data: blocking N,D,D,D shape ---
    {
        AmdtpPacketTimeline timeline;
        PacketTimelineSlot slots[8]{};
        uint8_t bytes[8][512]{};
        timeline.AttachSlots(slots, 8);

        timeline.MarkNoDataPacket(0); // cycle 0: no-data
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(1, 0), bytes[1], 512));
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(2, 8), bytes[2], 512));
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(3, 16), bytes[3], 512));

        // No-data position visible by index, invisible to frame lookup
        const PacketTimelineSlot* noDataSlot = timeline.SlotByIndex(0);
        CHECK(ctx, noDataSlot != nullptr);
        CHECK(ctx, !noDataSlot->isData);
        CHECK(ctx, noDataSlot->state.load() == PacketSlotState::Completed);

        const PacketTimelineSlot* frame0 = timeline.FindSlotForAudioFrame(0);
        CHECK(ctx, frame0 != nullptr);
        CHECK_EQ_U32(ctx, frame0->packetIndex, 1); // not the no-data slot
        CHECK_EQ_U32(ctx, timeline.FindSlotForAudioFrame(20)->packetIndex, 3);
    }

    // --- Ring reuse: generation bump + stale index invalidation ---
    {
        AmdtpPacketTimeline timeline;
        PacketTimelineSlot slots[4]{};
        uint8_t bytes[8][512]{};
        timeline.AttachSlots(slots, 4);

        for (uint32_t i = 0; i < 4; ++i) {
            CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(i, i * 8ull),
                                                 bytes[i], 512));
        }
        const uint32_t gen0 = slots[0].generation.load();

        // packetIndex 4 lands on ring position 0, evicting packetIndex 0.
        // Seqlock protocol: one mutation = two increments (odd while in
        // flight, back to even when stable).
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(4, 32), bytes[4], 512));
        CHECK(ctx, slots[0].generation.load() == gen0 + 2);
        CHECK(ctx, (slots[0].generation.load() & 1u) == 0);

        // Old occupant fully invalidated
        CHECK(ctx, timeline.SlotByIndex(0) == nullptr);
        CHECK(ctx, timeline.FindSlotForAudioFrame(0) == nullptr); // frames 0..7 gone
        // New occupant findable by index and by frame
        const PacketTimelineSlot* reused = timeline.SlotByIndex(4);
        CHECK(ctx, reused != nullptr);
        CHECK_EQ_U64(ctx, reused->firstAudioFrame, 32);
        CHECK(ctx, timeline.FindSlotForAudioFrame(35) == reused);

        // No-data reuse also bumps generation and evicts
        timeline.MarkNoDataPacket(5); // ring position 1, evicting packetIndex 1
        CHECK(ctx, timeline.SlotByIndex(1) == nullptr);
        CHECK(ctx, timeline.FindSlotForAudioFrame(8) == nullptr);
        CHECK(ctx, timeline.SlotByIndex(5) != nullptr);
    }

    // --- Seqlock snapshot: the RT-side lookup ---
    {
        AmdtpPacketTimeline timeline;
        PacketTimelineSlot slots[4]{};
        uint8_t bytes[2][512]{};
        timeline.AttachSlots(slots, 4);

        timeline.MarkNoDataPacket(0);
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(1, 1000), bytes[0], 512));

        // Hit: fields match the exposed packet and the sequence is even.
        PacketSlotSnapshot snap{};
        CHECK(ctx, timeline.SnapshotSlotForAudioFrame(1003, snap));
        CHECK(ctx, snap.slot == &slots[1]);
        CHECK(ctx, snap.packetBytes == bytes[0]);
        CHECK_EQ_U64(ctx, snap.firstAudioFrame, 1000);
        CHECK_EQ_U32(ctx, snap.framesInPacket, 8);
        CHECK_EQ_U32(ctx, snap.dbs, 2);
        CHECK_EQ_U32(ctx, snap.packetSizeBytes, 72);
        CHECK(ctx, (snap.generation & 1u) == 0);

        // Boundary misses mirror FindSlotForAudioFrame.
        CHECK(ctx, !timeline.SnapshotSlotForAudioFrame(999, snap));
        CHECK(ctx, !timeline.SnapshotSlotForAudioFrame(1008, snap));
        // No-data / Completed positions stay invisible.
        CHECK(ctx, !timeline.SnapshotSlotForAudioFrame(0, snap));

        // Mid-write guard: an odd sequence (mutation in flight) makes the
        // slot unsnapshotable; restoring even parity makes it visible again.
        slots[1].generation.fetch_add(1);
        CHECK(ctx, !timeline.SnapshotSlotForAudioFrame(1003, snap));
        slots[1].generation.fetch_add(1);
        CHECK(ctx, timeline.SnapshotSlotForAudioFrame(1003, snap));

        // Post-write reuse detection seam: the live slot's sequence moves on
        // when the position is reused, so a writer holding the snapshot can
        // see that its payload landed in a newer packet image.
        const uint32_t heldGen = snap.generation;
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(5, 2000), bytes[1], 512));
        CHECK(ctx, snap.slot->generation.load() != heldGen);
        CHECK(ctx, !timeline.SnapshotSlotForAudioFrame(1003, snap)); // evicted
    }

    // --- Reset clears everything ---
    {
        AmdtpPacketTimeline timeline;
        PacketTimelineSlot slots[4]{};
        uint8_t bytes[512]{};
        timeline.AttachSlots(slots, 4);
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(0, 100), bytes, 512));
        CHECK(ctx, timeline.FindSlotForAudioFrame(100) != nullptr);

        timeline.Reset();
        CHECK(ctx, timeline.FindSlotForAudioFrame(100) == nullptr);
        CHECK(ctx, timeline.SlotByIndex(0) == nullptr);
        CHECK_EQ_U32(ctx, timeline.SlotCount(), 4); // attachment survives reset
        // Reusable after reset
        CHECK(ctx, timeline.ExposeDataPacket(MakeDataPacket(0, 100), bytes, 512));
        CHECK(ctx, timeline.FindSlotForAudioFrame(100) != nullptr);
    }
}

} // namespace ASFW::LabTests
