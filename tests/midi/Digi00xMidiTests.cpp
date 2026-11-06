// Digi00xMidiTests.cpp
// ASFW - Digi 002 / 003 MIDI data channel (issue #249)
//
// Packets are built byte by byte from the documented layout (Digi00xMidi.hpp,
// checked against Linux amdtp-dot.c write/read_midi_messages) rather than by
// our own encoder, so a shared misunderstanding of the format cannot pass.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "Audio/Wire/Digi00x/Digi00xMidi.hpp"

using namespace ASFW::Encoding;
namespace Dg = ASFW::Encoding::Digi00x;

namespace {

constexpr uint8_t kDbs = 19;   // MIDI data channel + 18 PCM channels

class RecordingSink final : public ASFW::Audio::Ports::IMidiByteSink {
public:
    struct Run {
        uint8_t port;
        std::vector<uint8_t> bytes;
    };
    std::vector<Run> runs;

    void DeliverMidiBytes(uint8_t port, const uint8_t* bytes, uint8_t count,
                          uint64_t) noexcept override {
        runs.push_back(Run{port, std::vector<uint8_t>(bytes, bytes + count)});
    }
    void MarkMidiDiscontinuity(uint8_t) noexcept override {}
};

MpxMidiGeometry Geometry(uint8_t portCount) {
    return MpxMidiGeometry{.dbs = kDbs, .midiSlotIndex = 0, .portCount = portCount,
                           .dbcAligned = true};
}

/// `blocks` data blocks: slot 0 carries the given MIDI quadlets (idle after
/// them), the PCM slots carry 0x80 0x12 0x34 0x01 -- a valid-looking MIDI
/// quadlet, so reading the wrong slot would deliver bytes.
std::vector<uint8_t> MakeBlocks(uint32_t blocks, const std::vector<uint32_t>& midi) {
    std::vector<uint8_t> out(static_cast<size_t>(blocks) * kDbs * 4);
    for (uint32_t f = 0; f < blocks; ++f) {
        for (uint32_t s = 0; s < kDbs; ++s) {
            const uint32_t q = s == 0 ? (f < midi.size() ? midi[f] : 0x8000'0000u)
                                      : 0x8012'3401u;
            uint8_t* p = out.data() + (static_cast<size_t>(f) * kDbs + s) * 4;
            p[0] = static_cast<uint8_t>(q >> 24);
            p[1] = static_cast<uint8_t>(q >> 16);
            p[2] = static_cast<uint8_t>(q >> 8);
            p[3] = static_cast<uint8_t>(q);
        }
    }
    return out;
}

uint32_t SlotWord(const std::vector<uint8_t>& payload, uint32_t block, uint32_t slot) {
    const uint8_t* p = payload.data() + (static_cast<size_t>(block) * kDbs + slot) * 4;
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

} // namespace

// ---- Device to host -------------------------------------------------------

TEST(Digi00xMidiTests, Receive_IdleBlocksDeliverNothing) {
    auto blocks = MakeBlocks(6, {});
    RecordingSink sink;
    MpxMidiDemuxCounters counters;
    Dg::DemuxMidi(blocks.data(), 6, 0, Geometry(3), 0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
    EXPECT_EQ(counters.blocksInspected, 6u);
    EXPECT_EQ(counters.emptyQuadlets, 6u);
    EXPECT_EQ(counters.invalidLabels, 0u);
}

TEST(Digi00xMidiTests, Receive_PortComesFromByteThreeNotTheCounter) {
    // One and two bytes for MIDI port 1 (code 0x0), then two for the console
    // port (code 0xE), in blocks whose DBC rotation would say otherwise.
    auto blocks = MakeBlocks(4, {0x8090'0001u, 0x8000'0000u, 0x8090'3C02u, 0x80B0'07E2u});
    RecordingSink sink;
    MpxMidiDemuxCounters counters;
    Dg::DemuxMidi(blocks.data(), 4, 5, Geometry(3), 0, sink, counters);

    ASSERT_EQ(sink.runs.size(), 3u);
    EXPECT_EQ(sink.runs[0].port, Dg::kMidiPort1);
    EXPECT_EQ(sink.runs[0].bytes, (std::vector<uint8_t>{0x90}));
    EXPECT_EQ(sink.runs[1].port, Dg::kMidiPort1);
    EXPECT_EQ(sink.runs[1].bytes, (std::vector<uint8_t>{0x90, 0x3C}));
    EXPECT_EQ(sink.runs[2].port, Dg::kControlPort);
    EXPECT_EQ(sink.runs[2].bytes, (std::vector<uint8_t>{0xB0, 0x07}));
    EXPECT_EQ(counters.bytesDelivered, 5u);
}

TEST(Digi00xMidiTests, Receive_ConsolePortDroppedWhenTheModelHasNone) {
    auto blocks = MakeBlocks(2, {0x80B0'07E2u, 0x80F8'0001u});
    RecordingSink sink;
    MpxMidiDemuxCounters counters;
    Dg::DemuxMidi(blocks.data(), 2, 0, Geometry(1), 0, sink, counters);   // a Rack
    ASSERT_EQ(sink.runs.size(), 1u);
    EXPECT_EQ(sink.runs[0].port, Dg::kMidiPort1);
    EXPECT_EQ(counters.droppedForUnknownPort, 2u);
}

TEST(Digi00xMidiTests, Receive_MalformedQuadletsAreCountedNotDelivered) {
    // Wrong label; count 3 (more than the two data bytes); an undocumented
    // port code (0x2 is a host-to-device code only).
    auto blocks = MakeBlocks(3, {0x8190'0001u, 0x8090'3C03u, 0x8090'0021u});
    RecordingSink sink;
    MpxMidiDemuxCounters counters;
    Dg::DemuxMidi(blocks.data(), 3, 0, Geometry(3), 0, sink, counters);
    EXPECT_TRUE(sink.runs.empty());
    EXPECT_EQ(counters.invalidLabels, 3u);
}

// ---- Host to device -------------------------------------------------------

TEST(Digi00xMidiTests, Transmit_EachPortOnlyInItsOwnBlock) {
    // DBC 6, six blocks: rotation 6 7 0 1 2 3 -> ports -, -, 1, 2, console, -.
    auto payload = MakeBlocks(6, {0x1111'1111u, 0x1111'1111u, 0x1111'1111u,
                                  0x1111'1111u, 0x1111'1111u, 0x1111'1111u});
    MpxMidiPacketBytes bytes{};
    bytes.byteForPort[Dg::kMidiPort1] = 0x90;
    bytes.hasByteForPort[Dg::kMidiPort1] = true;
    bytes.byteForPort[Dg::kMidiPort2] = 0x3C;
    bytes.hasByteForPort[Dg::kMidiPort2] = true;
    bytes.byteForPort[Dg::kControlPort] = 0xF0;
    bytes.hasByteForPort[Dg::kControlPort] = true;

    Dg::WriteMidiSlot(payload.data(), 6, 6, Geometry(3), bytes);

    EXPECT_EQ(SlotWord(payload, 0, 0), 0x8000'0000u);
    EXPECT_EQ(SlotWord(payload, 1, 0), 0x8000'0000u);
    EXPECT_EQ(SlotWord(payload, 2, 0), 0x8090'0001u);   // MIDI 1, code 0x0
    EXPECT_EQ(SlotWord(payload, 3, 0), 0x803C'0021u);   // MIDI 2, code 0x2
    EXPECT_EQ(SlotWord(payload, 4, 0), 0x80F0'00E1u);   // console, code 0xE
    EXPECT_EQ(SlotWord(payload, 5, 0), 0x8000'0000u);
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t s = 1; s < kDbs; ++s) {
            ASSERT_EQ(SlotWord(payload, f, s), 0x8012'3401u) << "PCM slot touched";
        }
    }
}

TEST(Digi00xMidiTests, Transmit_PortTheModelLacksStaysIdle) {
    auto payload = MakeBlocks(3, {});
    MpxMidiPacketBytes bytes{};
    bytes.byteForPort[Dg::kControlPort] = 0xF0;
    bytes.hasByteForPort[Dg::kControlPort] = true;
    Dg::WriteMidiSlot(payload.data(), 3, 0, Geometry(2), bytes);   // a Rack
    EXPECT_EQ(SlotWord(payload, 2, 0), 0x8000'0000u);
}

TEST(Digi00xMidiTests, Transmit_PortsInPacketFollowTheCounter) {
    // Seven blocks from DBC 0 reach ports 0, 1, 2.
    EXPECT_EQ(Dg::PortsInPacket(7, 0, Geometry(3)), 0b111u);
    // Six blocks from DBC 3 cover rotation 3..8: only port 0 (at 8 % 8).
    EXPECT_EQ(Dg::PortsInPacket(6, 3, Geometry(3)), 0b001u);
    // A Rack has no console port.
    EXPECT_EQ(Dg::PortsInPacket(7, 0, Geometry(2)), 0b011u);
    EXPECT_EQ(Dg::PortsInPacket(0, 0, Geometry(3)), 0u);
}

TEST(Digi00xMidiTests, RoundTrip_PortOneAndConsole) {
    auto payload = MakeBlocks(8, {});
    MpxMidiPacketBytes bytes{};
    bytes.byteForPort[Dg::kMidiPort1] = 0x7F;
    bytes.hasByteForPort[Dg::kMidiPort1] = true;
    bytes.byteForPort[Dg::kControlPort] = 0xB0;
    bytes.hasByteForPort[Dg::kControlPort] = true;
    Dg::WriteMidiSlot(payload.data(), 8, 0, Geometry(3), bytes);

    RecordingSink sink;
    MpxMidiDemuxCounters counters;
    Dg::DemuxMidi(payload.data(), 8, 0, Geometry(3), 0, sink, counters);
    ASSERT_EQ(sink.runs.size(), 2u);
    EXPECT_EQ(sink.runs[0].port, Dg::kMidiPort1);
    EXPECT_EQ(sink.runs[0].bytes, (std::vector<uint8_t>{0x7F}));
    EXPECT_EQ(sink.runs[1].port, Dg::kControlPort);
    EXPECT_EQ(sink.runs[1].bytes, (std::vector<uint8_t>{0xB0}));
}
