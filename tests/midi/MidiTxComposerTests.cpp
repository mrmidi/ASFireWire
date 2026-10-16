// MidiTxComposerTests.cpp
// MIDI composition into armed transmit packets.
//
// Packets are built by the production packetizer and exposed through the
// production timeline, not hand-placed, so the fixture has the same armed image
// the real producer leaves behind: PCM slots hold encoded silence, the MIDI slot
// holds the empty quadlet 0x80000000.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "Audio/Engine/Direct/Tx/MidiTxComposer.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../support/TxPacketizerTestSupport.hpp"

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::AudioEngine::Direct::Tx::MidiTxComposer;
using ASFW::AudioEngine::Direct::Tx::MidiTxComposerConfig;
using ASFW::Encoding::MpxMidiGeometry;
using ASFW::Midi::MidiTransportBlock;

namespace {

constexpr uint32_t kPcm = 2;
constexpr uint32_t kDbs = kPcm + 1;
constexpr uint32_t kMidiSlot = kPcm;
constexpr uint32_t kSlotBytes = 128;
constexpr uint32_t kSlots = 512;

uint32_t Quadlet(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

// A stream armed by the production packetizer into a slab of fixed slots.
class ArmedStream {
public:
    explicit ArmedStream(uint32_t sampleRate = 48000) {
        AmdtpStreamConfig config{};
        config.sampleRate = sampleRate;
        config.streamMode = StreamMode::Blocking;
        config.dbs = kDbs;
        config.pcmChannels = kPcm;
        config.midiSlots = 1;
        config.framesPerDataPacket = sampleRate > 48000 ? 16 : 8;
        config.maxPacketBytes = kSlotBytes * 2;
        AmdtpTxPolicy policy{};
        slab_.assign(static_cast<size_t>(kSlots) * kSlotBytes * 2, 0xEE);
        EXPECT_TRUE(timeline_.AttachSlots(slots_.data(), kSlots));
        packetizer_.BindTimeline(&timeline_);
        EXPECT_TRUE(packetizer_.Configure(config, policy));
        packetizer_.Reset(0);
    }

    // Arm `count` more packets with the production allocator.
    void Arm(uint64_t count) {
        for (uint64_t i = 0; i < count; ++i, ++armedEnd_) {
            TxPacketSlotView slot{};
            slot.packetIndex = armedEnd_;
            slot.bytes = SlotBytes(armedEnd_);
            slot.capacityBytes = kSlotBytes * 2;
            AmdtpTimingState timing{};
            timing.disposition = AmdtpPacketDisposition::Data;
            PreparedTxPacket packet{};
            ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(
                packetizer_, slot, timing, nextFrame_, packet));
        }
    }

    uint8_t* SlotBytes(uint64_t packet) {
        return slab_.data() + (packet % kSlots) * kSlotBytes * 2;
    }
    const PacketTimelineSlot* Slot(uint64_t packet) { return timeline_.SlotByIndex(packet); }

    // Quadlet of the MIDI slot in data block `f` of `packet`.
    uint32_t MidiQuadlet(uint64_t packet, uint32_t f) {
        return Quadlet(SlotBytes(packet) + 8 + (f * kDbs + kMidiSlot) * 4);
    }
    uint8_t Dbc(uint64_t packet) { return SlotBytes(packet)[3]; }

    // All MIDI quadlets in a packet that carry a byte: (block, byte).
    std::vector<std::pair<uint32_t, uint8_t>> MidiBytes(uint64_t packet) {
        std::vector<std::pair<uint32_t, uint8_t>> out;
        const auto* slot = Slot(packet);
        if (slot == nullptr || !slot->isData) return out;
        for (uint32_t f = 0; f < slot->framesInPacket; ++f) {
            const uint32_t q = MidiQuadlet(packet, f);
            if ((q >> 24) == 0x81) out.emplace_back(f, static_cast<uint8_t>(q >> 16));
        }
        return out;
    }

    AmdtpPacketTimeline& timeline() { return timeline_; }
    uint64_t armedEnd() const { return armedEnd_; }

private:
    std::vector<uint8_t> slab_;
    std::array<PacketTimelineSlot, kSlots> slots_{};
    AmdtpPacketTimeline timeline_{};
    AmdtpTxPacketizer packetizer_{};
    uint64_t nextFrame_{0};
    uint64_t armedEnd_{0};
};

MidiTxComposerConfig Config(uint8_t ports = 4, uint32_t rate = 48000, uint32_t syt = 8) {
    MidiTxComposerConfig config{};
    config.geometry = MpxMidiGeometry{
        .dbs = kDbs, .midiSlotIndex = kMidiSlot, .portCount = ports, .dbcAligned = true};
    config.sampleRateHz = rate;
    config.sytIntervalFrames = syt;
    return config;
}

void Queue(MidiTransportBlock& block, uint32_t port, std::vector<uint8_t> bytes) {
    ASSERT_TRUE(block.hostToDevice[port].TryWrite(bytes, 0u));
}

class MidiTxComposerTest : public ::testing::Test {
protected:
    void SetUp() override {
        block.Arm(1);
        ASSERT_TRUE(composer.Configure(Config()));
        // The first pass of a run observes the stream's epoch and discards any
        // backlog from before it (bytes left behind by the previous stream).
        // Establish that here so each test queues its bytes after it, the way a
        // MIDI client writing into a running stream does.
        composer.Compose(&block, stream.timeline(), 0, 0);
    }
    MidiTransportBlock block;
    MidiTxComposer composer;
    ArmedStream stream;
};

} // namespace

TEST_F(MidiTxComposerTest, ArmedPacketsStartWithEmptyMidiQuadlets) {
    stream.Arm(16);
    uint32_t dataPackets = 0;
    for (uint64_t p = 0; p < 16; ++p) {
        const auto* slot = stream.Slot(p);
        if (slot == nullptr || !slot->isData) continue;   // 48 kHz blocking has NO-DATA cycles
        ++dataPackets;
        for (uint32_t f = 0; f < 8; ++f) {
            ASSERT_EQ(stream.MidiQuadlet(p, f), 0x80000000u) << "packet " << p;
        }
    }
    EXPECT_GT(dataPackets, 8u);
}

TEST_F(MidiTxComposerTest, ComposesAtTheFrontierAndNotBelowIt) {
    stream.Arm(200);
    Queue(block, 0, {0x90, 0x3C, 0x40});

    const auto result = composer.Compose(&block, stream.timeline(),
                                         /*firstWritable=*/100, stream.armedEnd());
    EXPECT_EQ(result.bytesComposed, 3u) << "the whole Note On fits the window";
    EXPECT_TRUE(block.hostToDevice[0].Empty()) << "composed bytes are retired";

    for (uint64_t p = 0; p < 100; ++p) {
        EXPECT_TRUE(stream.MidiBytes(p).empty()) << "packet " << p << " is below the frontier";
    }
    // The first byte goes into the first packet that has a block for port 0.
    bool found = false;
    for (uint64_t p = 100; p < 100 + 16 && !found; ++p) {
        const auto bytes = stream.MidiBytes(p);
        if (bytes.empty()) continue;
        found = true;
        EXPECT_EQ(bytes.size(), 1u);
        EXPECT_EQ(bytes[0].second, 0x90);
        const uint8_t dbc = stream.Dbc(p);
        EXPECT_EQ((dbc + bytes[0].first) % 8, 0u) << "port 0 must sit at (DBC + block) % 8";
    }
    EXPECT_TRUE(found);
}

TEST_F(MidiTxComposerTest, NeverWritesBeyondTheWindow) {
    stream.Arm(300);
    // Enough bytes on every port that every reachable packet could carry one.
    for (uint32_t port = 0; port < 4; ++port) {
        Queue(block, port, std::vector<uint8_t>(200, 0x11));
    }
    composer.Compose(&block, stream.timeline(), 100, stream.armedEnd());
    const uint64_t windowEnd = 100 + MidiTxComposerConfig{}.windowPackets;
    for (uint64_t p = windowEnd; p < 300; ++p) {
        EXPECT_TRUE(stream.MidiBytes(p).empty())
            << "packet " << p << " is beyond the composition window";
    }
}

TEST_F(MidiTxComposerTest, NeverWritesBeyondTheArmedEnd) {
    stream.Arm(104);
    Queue(block, 0, std::vector<uint8_t>(50, 0x22));
    composer.Compose(&block, stream.timeline(), 100, /*armedEnd=*/104);
    for (uint64_t p = 104; p < 120; ++p) {
        EXPECT_EQ(stream.Slot(p), nullptr) << "unarmed packets must not exist yet";
    }
    EXPECT_LE(composer.Cursor(), 104u);
}

TEST_F(MidiTxComposerTest, ABytePassedOverForWantOfAnArmedPacketStaysQueued) {
    // Nothing armed yet: the pass cannot place anything and must not consume.
    Queue(block, 0, {0x90, 0x3C, 0x40});
    const auto result = composer.Compose(&block, stream.timeline(), 0, 0);
    EXPECT_EQ(result.bytesComposed, 0u);
    EXPECT_EQ(block.hostToDevice[0].Available(), 3u);

    stream.Arm(32);
    const auto later = composer.Compose(&block, stream.timeline(), 0, stream.armedEnd());
    EXPECT_EQ(later.bytesComposed + block.hostToDevice[0].Available(), 3u);
    EXPECT_GE(later.bytesComposed, 1u);
}

TEST_F(MidiTxComposerTest, ByteOrderPerPortIsPreserved) {
    stream.Arm(200);
    Queue(block, 1, {0x90, 0x3C, 0x40, 0x80, 0x3C, 0x00});
    std::vector<uint8_t> seen;
    uint64_t frontier = 0;
    for (int pass = 0; pass < 40 && seen.size() < 6; ++pass) {
        frontier += 8;
        composer.Compose(&block, stream.timeline(), frontier, stream.armedEnd());
        for (uint64_t p = frontier - 8; p < frontier + 16; ++p) {
            for (const auto& [block_index, byte] : stream.MidiBytes(p)) {
                (void)block_index;
                seen.push_back(byte);
                // Wipe so a packet seen again by the next window is not recounted.
                const uint32_t f = block_index;
                uint8_t* q = stream.SlotBytes(p) + 8 + (f * kDbs + kMidiSlot) * 4;
                q[0] = 0x80; q[1] = q[2] = q[3] = 0;
            }
        }
    }
    EXPECT_EQ(seen, (std::vector<uint8_t>{0x90, 0x3C, 0x40, 0x80, 0x3C, 0x00}));
}

TEST_F(MidiTxComposerTest, ARateBurstIsHeldToTheUartModel) {
    stream.Arm(kSlots - 8);
    Queue(block, 0, std::vector<uint8_t>(300, 0x33));
    uint64_t composed = 0;
    uint64_t cycles = 0;
    uint64_t frontier = 0;
    for (int pass = 0; pass < 60; ++pass) {
        frontier += 8;
        const auto result = composer.Compose(&block, stream.timeline(), frontier, stream.armedEnd());
        composed += result.bytesComposed;
        cycles += result.packetsExamined;   // every bus cycle, DATA or not
    }
    ASSERT_GT(cycles, 100u);
    // 3093 B/s over the elapsed bus time (125 us per cycle), plus the credit.
    const double seconds = static_cast<double>(cycles) * 125e-6;
    EXPECT_LE(static_cast<double>(composed), 3093.0 * seconds + 3.0)
        << "the UART model must hold one port under 31.25 kbaud";
    EXPECT_GT(composed, 0u);
}

TEST_F(MidiTxComposerTest, ANewEpochDiscardsWhatWasQueuedBeforeTheStreamStarted) {
    stream.Arm(64);
    Queue(block, 0, std::vector<uint8_t>(10, 0x44));   // queued under epoch 1
    block.Arm(2);                                      // stream restarted
    const auto result = composer.Compose(&block, stream.timeline(), 0, stream.armedEnd());
    EXPECT_EQ(result.bytesComposed, 0u);
    EXPECT_TRUE(block.hostToDevice[0].Empty());
    EXPECT_EQ(composer.Counters().epochChanges, 2u);   // SetUp's epoch + this one
}

TEST_F(MidiTxComposerTest, AnInactiveBlockIsNotComposedFrom) {
    stream.Arm(64);
    Queue(block, 0, {0x90, 0x3C, 0x40});
    block.Quiesce();
    const auto result = composer.Compose(&block, stream.timeline(), 0, stream.armedEnd());
    EXPECT_EQ(result.bytesComposed, 0u);
    EXPECT_EQ(composer.Counters().inactivePasses, 1u);
    EXPECT_EQ(block.hostToDevice[0].Available(), 3u);
}

TEST_F(MidiTxComposerTest, TheFrontierOvertakingTheCursorIsCountedNotComposed) {
    stream.Arm(200);
    composer.Compose(&block, stream.timeline(), 50, stream.armedEnd());
    const uint64_t before = composer.Counters().packetsPastFrontier;
    // The next wake is late: the frontier moved well past where the pass ended.
    composer.Compose(&block, stream.timeline(), 150, stream.armedEnd());
    EXPECT_GT(composer.Counters().packetsPastFrontier, before);
}

TEST_F(MidiTxComposerTest, ByteForAPortBeyondThePortCountIsNotWritten) {
    // The rotation visits ports 0..7; only 0..3 exist. A ring for port 6 must
    // never be read, whatever was written into it.
    stream.Arm(64);
    Queue(block, 6, std::vector<uint8_t>(10, 0x55));
    composer.Compose(&block, stream.timeline(), 0, stream.armedEnd());
    for (uint64_t p = 0; p < 32; ++p) {
        for (const auto& entry : stream.MidiBytes(p)) {
            const uint8_t port = static_cast<uint8_t>((stream.Dbc(p) + entry.first) % 8);
            EXPECT_LT(port, 4u);
        }
    }
}

TEST_F(MidiTxComposerTest, ResetForRunRestartsPacketIndices) {
    stream.Arm(100);
    composer.Compose(&block, stream.timeline(), 60, stream.armedEnd());
    ASSERT_GE(composer.Cursor(), 60u);
    composer.ResetForRun();
    EXPECT_EQ(composer.Cursor(), 0u);
}

TEST(MidiTxComposerConfigure, RejectsGeometryThatCannotCarryMidi) {
    MidiTxComposer composer;
    auto config = Config();
    config.geometry.midiSlotIndex = kDbs;   // outside the data block
    EXPECT_FALSE(composer.Configure(config));
    EXPECT_FALSE(composer.Configured());
    config = Config();
    config.sampleRateHz = 0;
    EXPECT_FALSE(composer.Configure(config));
}

TEST(MidiTxComposerRates, ASixteenBlockPacketCarriesAtMostEightMidiBlocks) {
    // 96 kHz blocking: 16 blocks per packet; Linux and this port cap MIDI to the
    // first eight (amdtp-am824.c:28,306).
    MidiTransportBlock block;
    block.Arm(1);
    MidiTxComposer composer;
    ASSERT_TRUE(composer.Configure(Config(8, 96000, 16)));
    ArmedStream stream(96000);
    stream.Arm(64);
    for (uint32_t port = 0; port < 8; ++port) {
        ASSERT_TRUE(block.hostToDevice[port].TryWrite(std::vector<uint8_t>(8, 0x66), 0u));
    }
    composer.Compose(&block, stream.timeline(), 0, stream.armedEnd());
    for (uint64_t p = 0; p < 16; ++p) {
        for (const auto& entry : stream.MidiBytes(p)) {
            EXPECT_LT(entry.first, 8u) << "block " << entry.first << " is beyond the first eight";
        }
    }
}
