// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuMidiTests.cpp - MIDI in MOTU data blocks (MotuMidi.hpp), and its receive
// path through the production packet processor.
//
// Offsets and per-model slots cross-validated with Linux sound/firewire/motu
// motu-stream.c:117-131, motu-protocol-v2.c:274-318, motu-protocol-v3.c:272-338;
// pacing with amdtp-motu.c:211-235.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Engine/Direct/DirectInputWriter.hpp"
#include "Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "Isoch/Receive/IsochRxTiming.hpp"
#include "Midi/Transport/MidiTransportBlock.hpp"
#include "Audio/Wire/MOTU/MotuMidi.hpp"
#include "Audio/Wire/MOTU/MotuPayloadCodec.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace ASFW::Encoding::Motu;

struct RecordingSink final : ASFW::Audio::Ports::IMidiByteSink {
    struct Byte { uint8_t port, value; uint64_t hostTicks; };
    std::vector<Byte> bytes;
    std::vector<uint8_t> gaps;
    void DeliverMidiBytes(uint8_t port, const uint8_t* data, uint8_t count, uint64_t hostTicks) noexcept override {
        for (uint8_t i = 0; i < count; ++i) bytes.push_back({port, data[i], hostTicks});
    }
    void MarkMidiDiscontinuity(uint8_t port) noexcept override { gaps.push_back(port); }
};

constexpr MidiSlotOffsets k3rd{kMidiFlagByteOffset3rdQ, kMidiByteOffset3rdQ};

// UltraLite mk3 capture at 1x: 18 PCM chunks plus 2 message chunks.
constexpr uint32_t kCaptureChunks = 18;
constexpr uint32_t kStrideQuadlets = DataBlockQuadlets(kCaptureChunks);
constexpr size_t kStrideBytes = kStrideQuadlets * 4u;
static_assert(kStrideQuadlets == 16);

} // namespace

TEST(MotuMidiSlot, ModelsCarryTheSlotLinuxDescribes) {
    EXPECT_EQ(MidiSlotFor(0x19, true), MidiSlot::ThirdQuadlet);   // UltraLite mk3
    EXPECT_EQ(MidiSlotFor(0x19, false), MidiSlot::ThirdQuadlet);
    EXPECT_EQ(MidiSlotFor(0x03, true), MidiSlot::SecondQuadlet);  // 828mk2
    EXPECT_EQ(MidiSlotFor(0x0d, false), MidiSlot::SecondQuadlet); // UltraLite
    EXPECT_EQ(MidiSlotFor(0x33, true), MidiSlot::ThirdQuadlet);   // Audio Express in
    EXPECT_EQ(MidiSlotFor(0x33, false), MidiSlot::SecondQuadlet); // Audio Express out
    for (const uint32_t none : {0x01U, 0x02U, 0x05U, 0x17U, 0x37U, 0x45U})
        EXPECT_EQ(MidiSlotFor(none, true), MidiSlot::None) << none;
    EXPECT_FALSE(OffsetsOf(MidiSlot::None).has_value());
    EXPECT_EQ(OffsetsOf(MidiSlot::SecondQuadlet)->flag, 4U);
    EXPECT_EQ(OffsetsOf(MidiSlot::SecondQuadlet)->byte, 6U);
    EXPECT_EQ(OffsetsOf(MidiSlot::ThirdQuadlet)->flag, 8U);
    EXPECT_EQ(OffsetsOf(MidiSlot::ThirdQuadlet)->byte, 7U);
}

TEST(MotuMidiDemux, DeliversFlaggedBytesOnPortZeroOnly) {
    std::vector<uint8_t> blocks(3 * kStrideBytes, 0);
    blocks[0 * kStrideBytes + 8] = 0x01; blocks[0 * kStrideBytes + 7] = 0x90;
    blocks[1 * kStrideBytes + 7] = 0x3c;  // no flag: not a byte
    blocks[2 * kStrideBytes + 8] = 0x01; blocks[2 * kStrideBytes + 7] = 0x7f;
    RecordingSink sink;
    MotuMidiDemuxCounters counters;
    DemuxMotuMidi(blocks.data(), 3, kStrideBytes, k3rd, 1234, sink, counters);
    ASSERT_EQ(sink.bytes.size(), 2U);
    EXPECT_EQ(sink.bytes[0].value, 0x90);
    EXPECT_EQ(sink.bytes[1].value, 0x7f);
    EXPECT_EQ(sink.bytes[0].port, 0);
    EXPECT_EQ(sink.bytes[0].hostTicks, 1234U);
    EXPECT_EQ(counters.blocksInspected, 3U);
    EXPECT_EQ(counters.bytesDelivered, 2U);
}

TEST(MotuMidiDemux, OnlyTheFlagBitCounts) {
    std::vector<uint8_t> blocks(2 * kStrideBytes, 0);
    blocks[8] = 0xfe;                       // other bits set, flag clear
    blocks[kStrideBytes + 8] = 0x81;        // flag set among other bits
    blocks[kStrideBytes + 7] = 0xf8;
    RecordingSink sink;
    MotuMidiDemuxCounters counters;
    DemuxMotuMidi(blocks.data(), 2, kStrideBytes, k3rd, 0, sink, counters);
    ASSERT_EQ(sink.bytes.size(), 1U);
    EXPECT_EQ(sink.bytes[0].value, 0xf8);
}

TEST(MotuMidiDemux, ABlockTooShortForTheSlotIsNeverRead) {
    std::array<uint8_t, 8> tooShort{};
    tooShort.fill(0x01);
    RecordingSink sink;
    MotuMidiDemuxCounters counters;
    DemuxMotuMidi(tooShort.data(), 1, tooShort.size(), k3rd, 0, sink, counters);
    EXPECT_TRUE(sink.bytes.empty());
    EXPECT_EQ(counters.blocksInspected, 0U);
}

TEST(MotuMidiTxPacer, SendsOneBytePerIntervalAndClearsEveryOtherSlot) {
    MotuMidiTxPacer pacer;
    pacer.Configure(48000);  // 48000 / 3093 = 15: a byte every 16 blocks
    std::vector<uint8_t> blocks(40 * kStrideBytes, 0xaa);  // a recycled buffer
    std::vector<uint8_t> pending{0x90, 0x3c, 0x7f, 0x80};
    size_t next = 0;
    uint32_t asked = 0;
    const auto written = pacer.Compose(blocks.data(), 40, kStrideBytes, k3rd, [&](uint8_t& out) {
        ++asked;
        if (next == pending.size()) return false;
        out = pending[next++];
        return true;
    });
    EXPECT_EQ(written, 3U);  // blocks 0, 16, 32
    for (uint32_t b = 0; b < 40; ++b) {
        const uint8_t* p = blocks.data() + b * kStrideBytes;
        const bool carries = b == 0 || b == 16 || b == 32;
        EXPECT_EQ(p[8], carries ? 0x01 : 0x00) << b;
        if (!carries) EXPECT_EQ(p[7], 0x00) << b;
        EXPECT_EQ(p[0], 0xaa) << b;  // nothing outside the slot is touched
        EXPECT_EQ(p[9], 0xaa) << b;
    }
    EXPECT_EQ(blocks[7], 0x90);
    EXPECT_EQ(blocks[16 * kStrideBytes + 7], 0x3c);
    EXPECT_EQ(blocks[32 * kStrideBytes + 7], 0x7f);
    EXPECT_EQ(asked, 3U);  // asked only on blocks allowed to carry a byte
}

TEST(MotuMidiTxPacer, PacingCarriesAcrossPackets) {
    MotuMidiTxPacer pacer;
    pacer.Configure(48000);
    std::vector<uint8_t> packet(8 * kStrideBytes, 0);
    uint32_t total = 0;
    for (int p = 0; p < 4; ++p)  // 32 blocks in packets of 8
        total += pacer.Compose(packet.data(), 8, kStrideBytes, k3rd, [](uint8_t& out) { out = 0xf8; return true; });
    EXPECT_EQ(total, 2U);  // blocks 0 and 16, not one per packet
}

TEST(MotuMidiTxPacer, HigherRatesSpreadBytesOverMoreBlocks) {
    MotuMidiTxPacer pacer;
    pacer.Configure(192000);  // 192000 / 3093 = 62: a byte every 63 blocks
    std::vector<uint8_t> blocks(126 * kStrideBytes, 0);
    const auto written = pacer.Compose(blocks.data(), 126, kStrideBytes, k3rd,
                                       [](uint8_t& out) { out = 0xf8; return true; });
    EXPECT_EQ(written, 2U);
    EXPECT_EQ(blocks[63 * kStrideBytes + 8], 0x01);
}

//==============================================================================
// Receive path: a UltraLite mk3 capture packet through RxAudioPacketProcessor.
//==============================================================================

namespace {

void WriteBE32(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

/// Isoch prefix, CIP header (FMT `fmt`, FDF 0x22 for MOTU), `midi.size()` data
/// blocks; a nonzero entry is a MIDI byte in that block's 3rd-quadlet slot.
std::vector<uint8_t> MakeMotuPacket(const std::vector<uint8_t>& midi, uint8_t fmt = 0x02) {
    std::vector<uint8_t> packet(8 + 8 + midi.size() * kStrideBytes, 0);
    WriteBE32(packet.data() + 8, (kStrideQuadlets << 16) | 0);
    WriteBE32(packet.data() + 12, (1u << 31) | (uint32_t{fmt} << 24) | (0x22u << 16) | 0xffffu);
    for (size_t b = 0; b < midi.size(); ++b) {
        uint8_t* block = packet.data() + 16 + b * kStrideBytes;
        if (midi[b] != 0) { block[8] = 0x01; block[7] = midi[b]; }
    }
    return packet;
}

struct ProcessorRig {
    ASFW::AudioEngine::Direct::DirectInputWriter writer;  // unbound: no CoreAudio client
    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor{writer};
    ASFW::Audio::Wire::MotuRxPayloadCodec codec{kCaptureChunks, {}, /*v3=*/true};
    RecordingSink sink;
    MotuMidiDemuxCounters counters;

    ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessorResult Feed(const std::vector<uint8_t>& packet) {
        ASFW::AudioEngine::Direct::Rx::RxMidiExtraction midi{};
        midi.sink = &sink;
        midi.motuSlot = OffsetsOf(MidiSlotFor(0x19, true));
        midi.motuCounters = &counters;
        midi.drainHostTicks = 777;
        return processor.ProcessPacket(packet.data(), packet.size(), 0, kCaptureChunks, codec,
                                       0, true, {}, false,
                                       ASFW::Encoding::AudioPacketFraming::kCip, midi);
    }
};

} // namespace

TEST(MotuMidiReceivePath, BytesComeOffTheWireWithNoCaptureBufferBound) {
    ProcessorRig rig;
    const auto result = rig.Feed(MakeMotuPacket({0x90, 0, 0, 0, 0x3c, 0, 0, 0x7f}));
    EXPECT_EQ(result.midiBytesDelivered, 3U);
    EXPECT_FALSE(result.midiGap);
    ASSERT_EQ(rig.sink.bytes.size(), 3U);
    EXPECT_EQ(rig.sink.bytes[0].value, 0x90);
    EXPECT_EQ(rig.sink.bytes[1].value, 0x3c);
    EXPECT_EQ(rig.sink.bytes[2].value, 0x7f);
    EXPECT_EQ(rig.sink.bytes[0].hostTicks, 777U);  // no receive stamp: the drain's time
    EXPECT_EQ(rig.counters.blocksInspected, 8U);
}

TEST(MotuMidiReceivePath, ARaggedPacketIsAGapNotBytes) {
    ProcessorRig rig;
    auto packet = MakeMotuPacket({0x90, 0, 0, 0});
    packet.resize(packet.size() - 4);  // not whole data blocks
    const auto result = rig.Feed(packet);
    EXPECT_TRUE(result.midiGap);
    EXPECT_TRUE(rig.sink.bytes.empty());
    ASSERT_EQ(rig.sink.gaps.size(), 1U);
    EXPECT_EQ(rig.sink.gaps[0], 0);
}

TEST(MotuMidiReceivePath, AnAm824PacketIsNotReadAsMotu) {
    ProcessorRig rig;
    const auto result = rig.Feed(MakeMotuPacket({0x90, 0, 0, 0}, /*fmt=*/0x10));
    EXPECT_TRUE(rig.sink.bytes.empty());
    EXPECT_TRUE(result.midiGap);
}

TEST(MotuMidiReceivePath, AnEmptyPacketCarriesNothingAndIsNoGap) {
    ProcessorRig rig;
    const auto result = rig.Feed(MakeMotuPacket({}));
    EXPECT_TRUE(rig.sink.bytes.empty());
    EXPECT_FALSE(result.midiGap);
}

//==============================================================================
// The receive consumer, bound the way a MOTU endpoint's MIDI service binds it:
// bytes reach the device -> host ring with no CoreAudio client.
//==============================================================================

namespace {

class MotuMidiBindingSource final : public ASFW::Audio::Runtime::IDirectAudioBindingSource {
public:
    explicit MotuMidiBindingSource(ASFW::Midi::MidiTransportBlock* block) {
        snapshot_.generation = 1;
        snapshot_.valid = true;
        snapshot_.midiRx.generation = 1;
        snapshot_.midiRx.endpointId = 0x19;
        snapshot_.midiRx.block = block;
        snapshot_.midiRx.motuSlot = OffsetsOf(MidiSlotFor(0x19, true));
    }
    bool CopyDirectAudioBinding(ASFW::Audio::Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        out = snapshot_;
        return true;
    }

private:
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot_{};
};

} // namespace

TEST(MotuMidiReceivePath, TheConsumerPutsMotuBytesInTheRing) {
    auto block = std::make_unique<ASFW::Midi::MidiTransportBlock>();
    block->Arm(1);
    MotuMidiBindingSource source{block.get()};
    ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer consumer{
        &source, {.wireFormat = ASFW::Encoding::AudioWireFormat::kMotuPacked,
                  .am824Slots = kStrideQuadlets,
                  .streamChannels = kCaptureChunks}};
    ASFW::Audio::Wire::MotuRxPayloadCodec codec{kCaptureChunks, {}, /*v3=*/true};
    consumer.SetPayloadCodec(&codec);
    consumer.OnReceiveActivated();

    const ASFW::Isoch::IsochReceiveBatch batch{.drainCycleTimer = 0, .drainHostTicks = 1000};
    const auto packet = MakeMotuPacket({0x90, 0, 0, 0, 0x3c, 0, 0, 0x7f});
    consumer.BeginReceiveBatch(batch);
    consumer.ConsumePacket(batch, {.descriptorIndex = 0, .payload = {packet.data(), packet.size()}});
    consumer.EndReceiveBatch(batch);

    EXPECT_TRUE(consumer.MidiBound());
    std::array<uint8_t, 4> bytes{};
    std::array<uint64_t, 4> stamps{};
    ASSERT_EQ(block->deviceToHost[0].Peek(bytes, stamps), 3u);
    EXPECT_EQ(bytes[0], 0x90);
    EXPECT_EQ(bytes[1], 0x3c);
    EXPECT_EQ(bytes[2], 0x7f);
    EXPECT_EQ(block->deviceToHost[1].Available(), 0u);  // one port only
}
