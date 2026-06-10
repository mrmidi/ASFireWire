#include "TestHarness.hpp"

#include "../Lab/FakeIsochTxSlotProvider.hpp"
#include "../Lab/PacketDumpBlob.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpPacketTimeline.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpPayloadWriter.hpp"

#include <cstring>
#include <vector>

namespace ASFW::LabTests {

using namespace ASFW::Lab;
using namespace Protocols::Audio::AMDTP;

namespace {

// Publish a canonical 8-frame / dbs=2 data packet (72 B) or an 8 B no-data
// packet through the provider, mirroring the engine's publish-at-prepare
// path, and expose data packets on the timeline.
struct DumpFixture final {
    FakeIsochTxSlotProvider provider{};
    AmdtpPacketTimeline timeline{};
    PacketTimelineSlot slots[512]{};
    AmdtpPayloadWriterCounters payload{};
    uint64_t nextFrame{0};

    DumpFixture() { timeline.AttachSlots(slots, 512); }

    void PublishData(uint32_t packetIndex, uint8_t marker) {
        TxPacketSlotView view{};
        provider.AcquireWritableSlot(packetIndex, view);
        std::memset(view.bytes, marker, FakeIsochTxSlotProvider::kSlotCapacityBytes);

        PreparedTxPacket packet{};
        packet.packetIndex = packetIndex;
        packet.byteCount = 72;
        packet.isData = true;
        packet.dbc = static_cast<uint8_t>(packetIndex * 8);
        packet.syt = 0x1234;
        packet.firstAudioFrame = nextFrame;
        packet.framesInPacket = 8;
        packet.dbs = 2;
        nextFrame += 8;

        timeline.ExposeDataPacket(packet, view.bytes,
                                  FakeIsochTxSlotProvider::kSlotCapacityBytes);
        provider.PublishSlot(packet);
    }

    void PublishNoData(uint32_t packetIndex) {
        TxPacketSlotView view{};
        provider.AcquireWritableSlot(packetIndex, view);

        PreparedTxPacket packet{};
        packet.packetIndex = packetIndex;
        packet.byteCount = 8;
        packet.isData = false;
        timeline.MarkNoDataPacket(packetIndex);
        provider.PublishSlot(packet);
    }

    PacketDumpContext Context(uint64_t nextPacketIndex) const {
        PacketDumpContext context{};
        context.hostTimeTicks = 0xABCDEF;
        context.periodIndex = 7;
        context.ztsPeriodFrames = 512;
        context.ioRunning = true;
        context.exposedFrames = nextFrame;
        context.nextPacketIndex = nextPacketIndex;
        context.prepareFailures = 0;
        context.writeEndCount = 42;
        return context;
    }
};

const PacketDumpHeader* HeaderOf(const std::vector<uint8_t>& blob) {
    return reinterpret_cast<const PacketDumpHeader*>(blob.data());
}

const PacketDumpRecord* RecordOf(const std::vector<uint8_t>& blob, uint32_t i) {
    return reinterpret_cast<const PacketDumpRecord*>(
        blob.data() + sizeof(PacketDumpHeader) + i * sizeof(PacketDumpRecord));
}

} // namespace

void RunPacketDumpBlobTests(TestContext& ctx) {
    // --- Layout is the wire contract shared with the Swift inspector ---
    CHECK_EQ_U32(ctx, sizeof(PacketDumpHeader), 128);
    CHECK_EQ_U32(ctx, sizeof(PacketDumpRecord), 576);
    CHECK_EQ_U64(ctx, PacketDumpBlobSize(6), 128 + 6 * 576);

    // --- Window resolution, ordering, and header context ---
    {
        DumpFixture f;
        // Cadence N,D,D,D for 20 packets (indices 0..19).
        for (uint32_t i = 0; i < 20; ++i) {
            if (i % 4 == 0) {
                f.PublishNoData(i);
            } else {
                f.PublishData(i, static_cast<uint8_t>(0x40 + i));
            }
        }

        std::vector<uint8_t> blob(PacketDumpBlobSize(6));
        const size_t written = BuildPacketDumpBlob(
            f.provider, f.timeline, f.payload, f.Context(20), 6,
            kPacketDumpAnchorLatest, blob.data(), blob.size());
        CHECK_EQ_U64(ctx, written, PacketDumpBlobSize(6));

        const PacketDumpHeader* header = HeaderOf(blob);
        CHECK_EQ_U32(ctx, header->magic, kPacketDumpMagic);
        CHECK_EQ_U32(ctx, header->version, kPacketDumpVersion);
        CHECK_EQ_U32(ctx, header->recordCount, 6);
        CHECK_EQ_U32(ctx, header->recordStride, sizeof(PacketDumpRecord));
        CHECK_EQ_U64(ctx, header->nextPacketIndex, 20);
        CHECK_EQ_U64(ctx, header->writeEndCount, 42);
        CHECK_EQ_U32(ctx, header->ioRunning, 1);

        // Oldest → newest, ending at the anchor (19).
        CHECK_EQ_U64(ctx, RecordOf(blob, 0)->packetIndex, 14);
        CHECK_EQ_U64(ctx, RecordOf(blob, 5)->packetIndex, 19);

        // No-data record: published, not data, 8 B, timeline Completed.
        const PacketDumpRecord* noData = RecordOf(blob, 2); // index 16
        CHECK_EQ_U64(ctx, noData->packetIndex, 16);
        CHECK(ctx, (noData->flags & kDumpFlagPublished) != 0);
        CHECK(ctx, (noData->flags & kDumpFlagIsData) == 0);
        CHECK_EQ_U32(ctx, noData->byteCount, 8);
        CHECK_EQ_U32(ctx, noData->slotState, 3); // Completed

        // Data record: flags, geometry, marker bytes, live (ExposedForAudio).
        const PacketDumpRecord* data = RecordOf(blob, 3); // index 17
        CHECK_EQ_U64(ctx, data->packetIndex, 17);
        CHECK(ctx, (data->flags & kDumpFlagIsData) != 0);
        CHECK(ctx, (data->flags & kDumpFlagTimelineLive) != 0);
        CHECK(ctx, (data->flags & kDumpFlagLiveBytes) != 0);
        CHECK_EQ_U32(ctx, data->byteCount, 72);
        CHECK_EQ_U32(ctx, data->framesInPacket, 8);
        CHECK_EQ_U32(ctx, data->dbs, 2);
        CHECK_EQ_U32(ctx, data->bytes[0], 0x40 + 17);
        CHECK_EQ_U32(ctx, data->bytes[511], 0x40 + 17);
        CHECK_EQ_U32(ctx, data->slotState, 1); // ExposedForAudio
    }

    // --- Fewer packets than requested: clamp to what exists ---
    {
        DumpFixture f;
        f.PublishData(0, 0x11);
        f.PublishData(1, 0x22);

        std::vector<uint8_t> blob(PacketDumpBlobSize(6));
        const size_t written = BuildPacketDumpBlob(
            f.provider, f.timeline, f.payload, f.Context(2), 6,
            kPacketDumpAnchorLatest, blob.data(), blob.size());
        CHECK_EQ_U64(ctx, written, PacketDumpBlobSize(2));
        CHECK_EQ_U32(ctx, HeaderOf(blob)->recordCount, 2);
        CHECK_EQ_U64(ctx, RecordOf(blob, 0)->packetIndex, 0);
        CHECK_EQ_U64(ctx, RecordOf(blob, 1)->packetIndex, 1);
    }

    // --- Nothing published yet: header-only blob ---
    {
        DumpFixture f;
        std::vector<uint8_t> blob(PacketDumpBlobSize(6));
        const size_t written = BuildPacketDumpBlob(
            f.provider, f.timeline, f.payload, f.Context(0), 6,
            kPacketDumpAnchorLatest, blob.data(), blob.size());
        CHECK_EQ_U64(ctx, written, PacketDumpBlobSize(0));
        CHECK_EQ_U32(ctx, HeaderOf(blob)->recordCount, 0);
    }

    // --- Anchor in the middle: window ends at the anchor ---
    {
        DumpFixture f;
        for (uint32_t i = 0; i < 12; ++i) {
            f.PublishData(i, static_cast<uint8_t>(i));
        }
        std::vector<uint8_t> blob(PacketDumpBlobSize(4));
        const size_t written = BuildPacketDumpBlob(
            f.provider, f.timeline, f.payload, f.Context(12), 4, 7,
            blob.data(), blob.size());
        CHECK_EQ_U64(ctx, written, PacketDumpBlobSize(4));
        CHECK_EQ_U64(ctx, RecordOf(blob, 0)->packetIndex, 4);
        CHECK_EQ_U64(ctx, RecordOf(blob, 3)->packetIndex, 7);
    }

    // --- Evicted index (ring wrapped): reported, not faked ---
    {
        DumpFixture f;
        // 520 publications wrap the 512-slot ring; index 3 is long gone.
        for (uint32_t i = 0; i < 520; ++i) {
            f.PublishData(i, static_cast<uint8_t>(i & 0xFF));
        }
        std::vector<uint8_t> blob(PacketDumpBlobSize(2));
        const size_t written = BuildPacketDumpBlob(
            f.provider, f.timeline, f.payload, f.Context(520), 2, 3,
            blob.data(), blob.size());
        CHECK_EQ_U64(ctx, written, PacketDumpBlobSize(2));
        const PacketDumpRecord* evicted = RecordOf(blob, 1); // index 3
        CHECK_EQ_U64(ctx, evicted->packetIndex, 3);
        CHECK(ctx, (evicted->flags & kDumpFlagPublished) == 0);
        CHECK_EQ_U32(ctx, evicted->byteCount, 0);
        CHECK_EQ_U32(ctx, evicted->slotState, 0xFFFFFFFFu);
    }

    // --- Capacity guard ---
    {
        DumpFixture f;
        f.PublishData(0, 0x55);
        std::vector<uint8_t> blob(PacketDumpBlobSize(1) - 1);
        const size_t written = BuildPacketDumpBlob(
            f.provider, f.timeline, f.payload, f.Context(1), 1,
            kPacketDumpAnchorLatest, blob.data(), blob.size());
        CHECK_EQ_U64(ctx, written, 0);
    }
}

} // namespace ASFW::LabTests
