// MotuMidiTxComposerTests.cpp
// Host -> device MIDI into armed MOTU packets.
//
// Packets are armed by the production packetizer into the production timeline,
// at the UltraLite mk3's playback data block size (13 quadlets: SPH, two message
// chunks, 14 PCM chunks). The composer reads only the timeline and the block
// bytes, so the AM824 framing the packetizer gives the PCM part does not matter
// here; the MIDI slot (flag byte 8, data byte 7) is in the message bytes, which
// the MOTU payload writer never writes.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "Audio/Engine/Direct/Tx/MotuMidiTxComposer.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../support/TxPacketizerTestSupport.hpp"

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::AudioEngine::Direct::Tx::MotuMidiTxComposer;
using ASFW::AudioEngine::Direct::Tx::MotuMidiTxComposerConfig;
using ASFW::Midi::MidiTransportBlock;

namespace {

constexpr uint32_t kDbs = ASFW::Encoding::Motu::DataBlockQuadlets(14);
static_assert(kDbs == 13);
constexpr uint32_t kStride = kDbs * 4;
constexpr uint32_t kSlotBytes = 512;
constexpr uint32_t kSlots = 512;
constexpr ASFW::Encoding::Motu::MidiSlotOffsets kSlot{8, 7};

class ArmedStream {
public:
    explicit ArmedStream(uint32_t sampleRate = 48000) {
        AmdtpStreamConfig config{};
        config.sampleRate = sampleRate;
        config.streamMode = StreamMode::Blocking;
        config.dbs = kDbs;
        config.pcmChannels = kDbs;
        config.framesPerDataPacket = sampleRate > 96000 ? 32 : sampleRate > 48000 ? 16 : 8;
        config.maxPacketBytes = kSlotBytes * 4;
        AmdtpTxPolicy policy{};
        slab_.assign(static_cast<size_t>(kSlots) * kSlotBytes * 4, 0);
        EXPECT_TRUE(timeline_.AttachSlots(slots_.data(), kSlots));
        packetizer_.BindTimeline(&timeline_);
        EXPECT_TRUE(packetizer_.Configure(config, policy));
        packetizer_.Reset(0);
        framesPerPacket_ = config.framesPerDataPacket;
    }

    void Arm(uint64_t count) {
        for (uint64_t i = 0; i < count; ++i, ++armedEnd_) {
            TxPacketSlotView slot{};
            slot.packetIndex = armedEnd_;
            slot.bytes = SlotBytes(armedEnd_);
            slot.capacityBytes = kSlotBytes * 4;
            AmdtpTimingState timing{};
            timing.disposition = AmdtpPacketDisposition::Data;
            PreparedTxPacket packet{};
            ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer_, slot, timing, nextFrame_, packet));
            // A MOTU packet's message bytes start clear; nothing else is ours.
            for (uint32_t f = 0; f < packet.framesInPacket; ++f) {
                SlotBytes(armedEnd_)[8 + f * kStride + kSlot.flag] = 0;
                SlotBytes(armedEnd_)[8 + f * kStride + kSlot.byte] = 0;
            }
        }
    }

    uint8_t* SlotBytes(uint64_t packet) { return slab_.data() + (packet % kSlots) * kSlotBytes * 4; }
    const PacketTimelineSlot* Slot(uint64_t packet) { return timeline_.SlotByIndex(packet); }

    /// Every MIDI byte in the stream, as (absolute data block, byte), in order.
    std::vector<std::pair<uint64_t, uint8_t>> MidiBytes(uint64_t from, uint64_t to) {
        std::vector<std::pair<uint64_t, uint8_t>> out;
        uint64_t blockBase = 0;
        for (uint64_t p = 0; p < to; ++p) {
            const auto* slot = Slot(p);
            const uint32_t blocks = (slot && slot->isData) ? slot->framesInPacket.load() : 0;
            if (p >= from) {
                for (uint32_t f = 0; f < blocks; ++f) {
                    const uint8_t* b = SlotBytes(p) + 8 + f * kStride;
                    if (b[kSlot.flag] & 0x01) out.emplace_back(blockBase + f, b[kSlot.byte]);
                }
            }
            blockBase += blocks;
        }
        return out;
    }

    AmdtpPacketTimeline& timeline() { return timeline_; }
    uint64_t armedEnd() const { return armedEnd_; }
    uint32_t framesPerPacket() const { return framesPerPacket_; }

private:
    std::vector<uint8_t> slab_;
    std::array<PacketTimelineSlot, kSlots> slots_{};
    AmdtpPacketTimeline timeline_{};
    AmdtpTxPacketizer packetizer_{};
    uint64_t nextFrame_{0};
    uint64_t armedEnd_{0};
    uint32_t framesPerPacket_{8};
};

MotuMidiTxComposerConfig Config(uint32_t rate = 48000, uint32_t fpp = 8, uint32_t window = 0) {
    MotuMidiTxComposerConfig config{};
    config.slot = kSlot;
    config.dbs = kDbs;
    config.sampleRateHz = rate;
    config.framesPerDataPacket = fpp;
    if (window) config.windowPackets = window;
    return config;
}

void Queue(MidiTransportBlock& block, std::vector<uint8_t> bytes) {
    ASSERT_TRUE(block.hostToDevice[0].TryWrite(bytes, 0u));
}

class MotuMidiTxComposerTest : public ::testing::Test {
protected:
    void SetUp() override {
        block.Arm(1);
        ASSERT_TRUE(composer.Configure(Config()));
        composer.Compose(&block, stream.timeline(), 0, 0);  // observe the stream's epoch
    }
    MidiTransportBlock block;
    MotuMidiTxComposer composer;
    ArmedStream stream;
};

} // namespace

TEST_F(MotuMidiTxComposerTest, ANoteOnGoesOutInOrderAtTheSlotAndIsRetired) {
    stream.Arm(200);
    Queue(block, {0x90, 0x3C, 0x40});
    // Compose pass by pass, as the producer does, until the bytes are out.
    for (uint64_t frontier = 100; frontier < 200 && !block.hostToDevice[0].Empty(); frontier += 4)
        composer.Compose(&block, stream.timeline(), frontier, stream.armedEnd());
    EXPECT_TRUE(block.hostToDevice[0].Empty());
    const auto bytes = stream.MidiBytes(0, 200);
    ASSERT_EQ(bytes.size(), 3u);
    EXPECT_EQ(bytes[0].second, 0x90);
    EXPECT_EQ(bytes[1].second, 0x3C);
    EXPECT_EQ(bytes[2].second, 0x40);
    EXPECT_TRUE(stream.MidiBytes(0, 100).empty()) << "nothing below the first frontier";
}

TEST_F(MotuMidiTxComposerTest, BytesAreSpacedForTheUnitsMidiPort) {
    stream.Arm(400);
    Queue(block, std::vector<uint8_t>(20, 0xF8));
    for (uint64_t frontier = 0; frontier < 400; frontier += 4)
        composer.Compose(&block, stream.timeline(), frontier, stream.armedEnd());
    const auto bytes = stream.MidiBytes(0, 400);
    ASSERT_GE(bytes.size(), 10u);
    // 48000 / 3093 = 15: at least 16 data blocks from one byte to the next.
    for (size_t i = 1; i < bytes.size(); ++i)
        EXPECT_GE(bytes[i].first - bytes[i - 1].first, 16u) << "byte " << i;
}

TEST_F(MotuMidiTxComposerTest, NeverWritesBeyondTheWindowOrTheArmedEnd) {
    stream.Arm(104);
    Queue(block, std::vector<uint8_t>(50, 0x22));
    composer.Compose(&block, stream.timeline(), 100, /*armedEnd=*/104);
    EXPECT_LE(composer.Cursor(), 104u);
    EXPECT_EQ(stream.Slot(104), nullptr);

    MidiTransportBlock other;
    other.Arm(1);
    MotuMidiTxComposer narrow;
    ASSERT_TRUE(narrow.Configure(Config(48000, 8, /*window=*/2)));
    narrow.Compose(&other, stream.timeline(), 0, 0);
    ArmedStream wide;
    wide.Arm(300);
    ASSERT_TRUE(other.hostToDevice[0].TryWrite(std::vector<uint8_t>(50, 0x33), 0u));
    narrow.Compose(&other, wide.timeline(), 100, wide.armedEnd());
    EXPECT_TRUE(wide.MidiBytes(102, 300).empty()) << "only the two-packet window is written";
}

TEST_F(MotuMidiTxComposerTest, AnInactiveStreamWritesNothingAndKeepsNothing) {
    stream.Arm(50);
    Queue(block, {0x90});
    block.Quiesce();
    const auto result = composer.Compose(&block, stream.timeline(), 10, stream.armedEnd());
    EXPECT_EQ(result.bytesComposed, 0u);
    EXPECT_TRUE(stream.MidiBytes(0, 50).empty());
    EXPECT_EQ(composer.Counters().inactivePasses, 1u);
}

TEST_F(MotuMidiTxComposerTest, AStreamOfAnotherShapeIsNotWritten) {
    MotuMidiTxComposer composer13;
    auto config = Config();
    config.dbs = kDbs + 1;  // a different data block size than the armed packets
    ASSERT_TRUE(composer13.Configure(config));
    composer13.Compose(&block, stream.timeline(), 0, 0);
    stream.Arm(50);
    Queue(block, {0x90});
    composer13.Compose(&block, stream.timeline(), 10, stream.armedEnd());
    EXPECT_TRUE(stream.MidiBytes(0, 50).empty());
    EXPECT_FALSE(block.hostToDevice[0].Empty()) << "the byte waits for a packet it fits";
    EXPECT_GT(composer13.Counters().geometryMismatches, 0u);
}

TEST(MotuMidiTxComposerConfigTests, RefusesASlotOutsideTheBlock) {
    MotuMidiTxComposer composer;
    auto config = Config();
    config.dbs = 2;  // 8 bytes: the flag at byte 8 would be past the block
    EXPECT_FALSE(composer.Configure(config));
    EXPECT_FALSE(composer.Configured());
}
