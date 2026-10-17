// MpxMidiDemuxTests.cpp
// ASFW - WP-7 receive-side extraction tests
//
// Packets are built byte by byte against the AM824 layout rather than produced
// by our own transmit path, so a shared misunderstanding of the framing cannot
// pass.
//
// Layout cross-checked against Linux read_midi_messages (amdtp-am824.c:323-344).

#include <gtest/gtest.h>

#include <vector>

#include "Audio/Wire/AM824/MpxMidiDemux.hpp"
#include "Midi/Transport/MidiRingByteSink.hpp"

using namespace ASFW::Encoding;
using ASFW::Midi::MidiRingByteSink;
using ASFW::Midi::MidiTransportBlock;
using ASFW::Midi::kMidiPortsPerDirection;

namespace {

/// Records what the demux delivered, so a test can assert on ports and runs
/// rather than on ring side effects.
class RecordingSink final : public ASFW::Audio::Ports::IMidiByteSink {
public:
    struct Run {
        uint8_t port;
        std::vector<uint8_t> bytes;
    };
    std::vector<Run> runs;
    std::vector<uint8_t> discontinuities;

    void DeliverMidiBytes(uint8_t port, const uint8_t* bytes,
                          uint8_t count, uint64_t hostTicks) noexcept override {
        runs.push_back(Run{port, std::vector<uint8_t>(bytes, bytes + count)});
    }
    void MarkMidiDiscontinuity(uint8_t port) noexcept override {
        discontinuities.push_back(port);
    }
};

/// Build `blocks` data blocks of `dbs` slots, with the MIDI slot at
/// `midiSlotIndex` carrying the given big-endian quadlets.
std::vector<uint8_t> MakeBlocks(uint32_t blocks, uint8_t dbs,
                                uint8_t midiSlotIndex,
                                const std::vector<uint32_t>& midiQuadlets) {
    // Non-MIDI slots carry MBLA audio (label 0x40). Every MIDI slot the caller
    // does not specify carries 0x80000000, the IEC 61883-6 "MIDI conformant
    // data, zero valid bytes" filler -- which is what a real device sends in
    // every block it has no byte for, and what ASFW itself already transmits.
    std::vector<uint8_t> out(static_cast<size_t>(blocks) * dbs * 4, 0x40);
    for (uint32_t f = 0; f < blocks; ++f) {
        uint8_t* slot = out.data() + (static_cast<size_t>(f) * dbs + midiSlotIndex) * 4;
        const uint32_t q =
            f < midiQuadlets.size() ? midiQuadlets[f] : 0x8000'0000u;
        slot[0] = static_cast<uint8_t>(q >> 24);
        slot[1] = static_cast<uint8_t>(q >> 16);
        slot[2] = static_cast<uint8_t>(q >> 8);
        slot[3] = static_cast<uint8_t>(q);
    }
    return out;
}

MpxMidiGeometry Geometry(uint8_t dbs, uint8_t slot, uint8_t ports = 1,
                         bool aligned = true) {
    return MpxMidiGeometry{dbs, slot, ports, aligned};
}

} // namespace

//==============================================================================
// Framing
//==============================================================================

TEST(MpxMidiDemux, LiftsASingleByteFromTheTrailingSlot) {
    // 8 PCM + 1 MIDI slot; one byte in block 0, DBC 0 so it belongs to port 0.
    const auto blocks = MakeBlocks(8, 9, 8, {0x8190'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 8, 0, Geometry(9, 8), /*packetHostTicks=*/0, sink, counters);

    ASSERT_EQ(sink.runs.size(), 1u);
    EXPECT_EQ(sink.runs[0].port, 0);
    EXPECT_EQ(sink.runs[0].bytes, (std::vector<uint8_t>{0x90}));
    EXPECT_EQ(counters.bytesDelivered, 1u);
    EXPECT_EQ(counters.emptyQuadlets, 7u);
}

TEST(MpxMidiDemux, EmptyQuadletsAreNormalNotErrors) {
    const auto blocks = MakeBlocks(8, 9, 8, {0x8000'0000, 0x8000'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 8, 0, Geometry(9, 8), /*packetHostTicks=*/0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
    EXPECT_EQ(counters.emptyQuadlets, 8u);
    EXPECT_EQ(counters.invalidLabels, 0u);
}

TEST(MpxMidiDemux, AcceptsTwoAndThreeByteQuadlets) {
    // A whole three-byte Note On can arrive in one quadlet. Both references
    // only ever send one byte per quadlet, which is not a reason to assume it
    // on receive.
    const auto blocks = MakeBlocks(2, 9, 8, {0x8390'3C40, 0x82B0'0700});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 2, 0, Geometry(9, 8, 8), /*packetHostTicks=*/0, sink, counters);

    ASSERT_EQ(sink.runs.size(), 2u);
    EXPECT_EQ(sink.runs[0].bytes, (std::vector<uint8_t>{0x90, 0x3C, 0x40}));
    EXPECT_EQ(sink.runs[1].bytes, (std::vector<uint8_t>{0xB0, 0x07}));
    EXPECT_EQ(counters.bytesDelivered, 5u);
}

TEST(MpxMidiDemux, RejectsLabelsOutsideTheMidiRange) {
    // 0x40 is MBLA audio. Masking label & 3 the way Focusrite does would accept
    // it as MIDI; the explicit range check does not.
    const auto blocks = MakeBlocks(2, 9, 8, {0x4090'0000, 0x8490'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 2, 0, Geometry(9, 8), /*packetHostTicks=*/0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
    EXPECT_EQ(counters.invalidLabels, 2u);
}

//==============================================================================
// Port multiplexing
//==============================================================================

TEST(MpxMidiDemux, RotatesPortsFromTheDbc) {
    // One byte in every block, DBC 0: ports 0..7 in order.
    std::vector<uint32_t> quadlets;
    for (uint32_t i = 0; i < 8; ++i) {
        quadlets.push_back(0x8100'0000u | (uint32_t(0x10 + i) << 16));
    }
    const auto blocks = MakeBlocks(8, 9, 8, quadlets);
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 8, 0, Geometry(9, 8, 8), /*packetHostTicks=*/0, sink, counters);

    ASSERT_EQ(sink.runs.size(), 8u);
    for (uint8_t i = 0; i < 8; ++i) {
        EXPECT_EQ(sink.runs[i].port, i);
        EXPECT_EQ(sink.runs[i].bytes[0], 0x10 + i);
    }
}

TEST(MpxMidiDemux, DbcOffsetsTheRotation) {
    const auto blocks = MakeBlocks(2, 9, 8, {0x8111'0000, 0x8122'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 2, /*dbc=*/5, Geometry(9, 8, 8), /*packetHostTicks=*/0, sink, counters);
    ASSERT_EQ(sink.runs.size(), 2u);
    EXPECT_EQ(sink.runs[0].port, 5);
    EXPECT_EQ(sink.runs[1].port, 6);
}

TEST(MpxMidiDemux, DbcWrapsModuloEightPorts) {
    const auto blocks = MakeBlocks(3, 9, 8, {0x8111'0000, 0x8122'0000, 0x8133'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 3, /*dbc=*/7, Geometry(9, 8, 8), /*packetHostTicks=*/0, sink, counters);
    ASSERT_EQ(sink.runs.size(), 3u);
    EXPECT_EQ(sink.runs[0].port, 7);
    EXPECT_EQ(sink.runs[1].port, 0) << "the rotation wraps, it does not stop";
    EXPECT_EQ(sink.runs[2].port, 1);
}

TEST(MpxMidiDemux, DbcAt255WrapsLikeTheEightBitCounterItIs) {
    const auto blocks = MakeBlocks(2, 9, 8, {0x8111'0000, 0x8122'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 2, /*dbc=*/255, Geometry(9, 8, 8), /*packetHostTicks=*/0, sink, counters);
    ASSERT_EQ(sink.runs.size(), 2u);
    EXPECT_EQ(sink.runs[0].port, 255 % 8);
    EXPECT_EQ(sink.runs[1].port, (255 + 1) % 8);
}

TEST(MpxMidiDemux, UnalignedModeIgnoresTheDbc) {
    const auto blocks = MakeBlocks(2, 9, 8, {0x8111'0000, 0x8122'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 2, /*dbc=*/5,
                 Geometry(9, 8, 8, /*aligned=*/false), /*packetHostTicks=*/0, sink, counters);
    ASSERT_EQ(sink.runs.size(), 2u);
    EXPECT_EQ(sink.runs[0].port, 0);
    EXPECT_EQ(sink.runs[1].port, 1);
}

TEST(MpxMidiDemux, BytesForAPortTheDeviceDoesNotHaveAreDropped) {
    // The slot multiplexes 8 ports whether or not the device has 8 jacks.
    // Delivering port 3 to a device with one jack would invent an endpoint.
    std::vector<uint32_t> quadlets;
    for (uint32_t i = 0; i < 8; ++i) quadlets.push_back(0x8155'0000);
    const auto blocks = MakeBlocks(8, 9, 8, quadlets);
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 8, 0, Geometry(9, 8, /*ports=*/1), /*packetHostTicks=*/0, sink, counters);

    ASSERT_EQ(sink.runs.size(), 1u) << "only port 0 exists";
    EXPECT_EQ(sink.runs[0].port, 0);
    EXPECT_EQ(counters.droppedForUnknownPort, 7u);
}

//==============================================================================
// Geometry and bounds
//==============================================================================

TEST(MpxMidiDemux, FindsTheSlotAtANonTrailingIndex) {
    const auto blocks = MakeBlocks(1, 4, /*midiSlotIndex=*/1, {0x8190'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 1, 0, Geometry(4, 1), /*packetHostTicks=*/0, sink, counters);
    ASSERT_EQ(sink.runs.size(), 1u);
    EXPECT_EQ(sink.runs[0].bytes[0], 0x90);
}

TEST(MpxMidiDemux, RefusesASlotIndexOutsideTheDataBlock) {
    const auto blocks = MakeBlocks(4, 9, 8, {0x8190'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    // Slot 9 in a 9-slot block would read the first slot of the NEXT block.
    DemuxMpxMidi(blocks.data(), 4, 0, Geometry(9, 9), /*packetHostTicks=*/0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
    EXPECT_EQ(counters.blocksInspected, 0u);
}

TEST(MpxMidiDemux, RefusesZeroDbsAndZeroPorts) {
    const auto blocks = MakeBlocks(4, 9, 8, {0x8190'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 4, 0, Geometry(0, 0), /*packetHostTicks=*/0, sink, counters);
    DemuxMpxMidi(blocks.data(), 4, 0, Geometry(9, 8, /*ports=*/0), /*packetHostTicks=*/0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
}

TEST(MpxMidiDemux, ZeroEventsDeliversNothing) {
    const auto blocks = MakeBlocks(4, 9, 8, {0x8190'0000});
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 0, 0, Geometry(9, 8), /*packetHostTicks=*/0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
    EXPECT_EQ(counters.blocksInspected, 0u);
}

TEST(MpxMidiDemux, NullBlocksDeliversNothing) {
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(nullptr, 8, 0, Geometry(9, 8), /*packetHostTicks=*/0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
}

TEST(MpxMidiDemux, ReadsExactlyWithinTheSuppliedBlocks) {
    // The last inspected byte must be the final quadlet of the last block.
    constexpr uint32_t kBlocks = 8;
    constexpr uint8_t kDbs = 9;
    auto blocks = MakeBlocks(kBlocks, kDbs, 8, {});
    // Poison one byte past the end; a read there shows up as an invalid label.
    blocks.push_back(0x81);
    RecordingSink sink;
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), kBlocks, 0, Geometry(kDbs, 8, 8), /*packetHostTicks=*/0, sink, counters);
    EXPECT_EQ(counters.blocksInspected, kBlocks);
    EXPECT_TRUE(sink.runs.empty()) << "must not read past the last data block";
}

//==============================================================================
// Ring sink
//==============================================================================

TEST(MidiRingByteSinkTest, WritesDeliveredRunsIntoTheRightPortRing) {
    MidiTransportBlock block;
    block.Arm(3);
    MidiRingByteSink sink;
    sink.Bind(&block, 3);

    const auto blocks = MakeBlocks(2, 9, 8, {0x8390'3C40, 0x81F8'0000});
    MpxMidiDemuxCounters counters{};
    DemuxMpxMidi(blocks.data(), 2, 0, Geometry(9, 8, 8), /*packetHostTicks=*/0, sink, counters);

    uint8_t out[8]{};
    ASSERT_EQ(block.deviceToHost[0].Peek(out), 3u);
    EXPECT_EQ(out[0], 0x90);
    EXPECT_EQ(out[1], 0x3C);
    EXPECT_EQ(out[2], 0x40);
    ASSERT_EQ(block.deviceToHost[1].Peek(out), 1u);
    EXPECT_EQ(out[0], 0xF8);
}

TEST(MidiRingByteSinkTest, AStaleEpochDeliversNothing) {
    MidiTransportBlock block;
    block.Arm(3);
    MidiRingByteSink sink;
    sink.Bind(&block, 2);   // bound to the previous stream

    const uint8_t run[1] = {0x90};
    sink.DeliverMidiBytes(0, run, 1, 0);
    EXPECT_TRUE(block.deviceToHost[0].Empty())
        << "a restarted stream must not receive its predecessor's bytes";
}

TEST(MidiRingByteSinkTest, AFullRingCountsTheOverflowAndMarksTheGap) {
    MidiTransportBlock block;
    block.Arm(1);
    MidiRingByteSink sink;
    sink.Bind(&block, 1);

    std::vector<uint8_t> filler(ASFW::Midi::kMidiRingCapacityBytes, 0x7F);
    ASSERT_TRUE((block.deviceToHost[0].TryWrite(filler, 0u)));

    const uint8_t run[1] = {0x90};
    sink.DeliverMidiBytes(0, run, 1, 0);
    EXPECT_EQ(sink.OverflowRuns(), 1u);
    EXPECT_EQ(block.deviceToHost[0].droppedBytes.load(), 1u);
    EXPECT_GE(block.deviceToHost[0].discontinuities.load(), 1u)
        << "the consumer must learn its stream is broken here";
}

TEST(MidiRingByteSinkTest, UnboundDeliversNothingWithoutFaulting) {
    MidiRingByteSink sink;
    const uint8_t run[1] = {0x90};
    sink.DeliverMidiBytes(0, run, 1, 0);
    sink.MarkMidiDiscontinuity(0);
    EXPECT_FALSE(sink.Bound());
}
