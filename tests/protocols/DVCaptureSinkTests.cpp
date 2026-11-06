#include <gtest/gtest.h>

#include "Protocols/DV/DVCaptureSink.hpp"

#include <array>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

void WriteBE32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

std::array<uint8_t, 496> MakePacket(uint8_t dbc = 0) {
    std::array<uint8_t, 496> packet{};
    WriteBE32(packet.data() + 8, (2u << 24) | (120u << 16) | dbc);
    WriteBE32(packet.data() + 12, 0x8000FFFFu);
    for (size_t index = 0; index < 6; ++index) {
        const size_t offset = 16 + index * 80;
        packet[offset] = 4u << 5;
        packet[offset + 1] = 0;
        packet[offset + 2] = static_cast<uint8_t>(index);
        packet[offset + 3] = static_cast<uint8_t>(0xA0u + index);
    }
    return packet;
}

ASFW::Isoch::IsochReceivePacket View(std::span<const uint8_t> payload,
                                     uint16_t status = 0x11) {
    return ASFW::Isoch::IsochReceivePacket{
        .descriptorIndex = 0,
        .transferStatus = status,
        .residualCount = 0,
        .payload = payload,
    };
}

void Consume(ASFW::Protocols::DV::DVCaptureSink& sink,
             const ASFW::Isoch::IsochReceivePacket& packet) {
    sink.ConsumePacket(ASFW::Isoch::IsochReceiveBatch{}, packet);
}

using Sink = ASFW::Protocols::DV::DVCaptureSink;
using SourcePacket = std::array<uint8_t, Sink::kTsSourcePacketBytes>;

// MPEG-2 TS CIP header as a Sony HVR-Z1U sends it while playing HDV:
// q0=0x0006C4B8 (SID 0, DBS 6, FN 3, SPH 1, DBC 0xB8), q1=0xA0800000.
constexpr uint32_t kTsQ0WithoutDbc = 0x0006C400u;
constexpr uint32_t kTsQ1 = 0xA0800000u;

SourcePacket MakeSourcePacket(uint8_t marker) {
    SourcePacket packet{};
    packet.fill(marker);
    packet[0] = 0x01; // source packet header (timestamp), dropped on publish
    packet[1] = 0x02;
    packet[2] = 0x03;
    packet[3] = 0x04;
    packet[Sink::kSphBytes] = Sink::kTsSyncByte;
    return packet;
}

// One bus packet carrying `blocks` 24-byte data blocks taken from `stream`,
// starting `firstBlock` blocks into it.
std::vector<uint8_t> MakeTsPacket(uint8_t dbc,
                                  const std::vector<uint8_t>& stream,
                                  size_t firstBlock,
                                  size_t blocks,
                                  uint32_t q0WithoutDbc = kTsQ0WithoutDbc) {
    std::vector<uint8_t> packet(16 + blocks * Sink::kTsBlockBytes);
    WriteBE32(packet.data() + 8, q0WithoutDbc | dbc);
    WriteBE32(packet.data() + 12, kTsQ1);
    if (blocks > 0) {
        std::memcpy(packet.data() + 16,
                    stream.data() + firstBlock * Sink::kTsBlockBytes,
                    blocks * Sink::kTsBlockBytes);
    }
    return packet;
}

std::vector<uint8_t> Concat(std::initializer_list<SourcePacket> packets) {
    std::vector<uint8_t> stream;
    for (const auto& packet : packets) {
        stream.insert(stream.end(), packet.begin(), packet.end());
    }
    return stream;
}

struct TsFixture {
    explicit TsFixture(uint32_t records = 8)
        : memory(Sink::RequiredBytes(records)) {
        EXPECT_TRUE(sink.InitializeAndAttach(memory.data(), memory.size(),
                                             records));
    }

    ASFW::Protocols::DV::DVRingHeader& Header() {
        return *reinterpret_cast<ASFW::Protocols::DV::DVRingHeader*>(
            memory.data());
    }

    void Feed(const std::vector<uint8_t>& packet) {
        Consume(sink, View(packet));
    }

    // True when record `index` holds the 188-byte TS packet of `expected`.
    bool RecordHolds(uint32_t index, const SourcePacket& expected) {
        const uint8_t* record = memory.data() +
                                sizeof(ASFW::Protocols::DV::DVRingHeader) +
                                size_t(index) * Sink::kRecordBytes;
        return std::memcmp(record, expected.data() + Sink::kSphBytes,
                           Sink::kTsPacketBytes) == 0;
    }

    std::vector<uint8_t> memory;
    Sink sink;
};

} // namespace

TEST(DVCaptureSinkTests, PublishesValidatedDifChunk) {
    constexpr uint32_t records = 4;
    std::vector<uint8_t> memory(
        ASFW::Protocols::DV::DVCaptureSink::RequiredBytes(records));
    ASFW::Protocols::DV::DVCaptureSink sink;
    ASSERT_TRUE(sink.InitializeAndAttach(memory.data(), memory.size(), records));

    const auto packet = MakePacket();
    Consume(sink, View(packet));

    auto* header = reinterpret_cast<ASFW::Protocols::DV::DVRingHeader*>(
        memory.data());
    EXPECT_EQ(header->version,
              ASFW::Protocols::DV::DVCaptureSink::kVersion);
    EXPECT_EQ(header->writeIndex.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(header->streamFormat.load(std::memory_order_acquire),
              static_cast<uint32_t>(ASFW::Protocols::DV::StreamFormat::Dv));
    EXPECT_EQ(header->sourcePacketBytes.load(std::memory_order_relaxed), 480u);
    EXPECT_EQ(header->sourcePackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header->rejectedPackets.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(std::memcmp(memory.data() + sizeof(*header),
                          packet.data() + 16, 480),
              0);
}

TEST(DVCaptureSinkTests, TracksDbcDiscontinuityAndBadCompletion) {
    constexpr uint32_t records = 4;
    std::vector<uint8_t> memory(
        ASFW::Protocols::DV::DVCaptureSink::RequiredBytes(records));
    ASFW::Protocols::DV::DVCaptureSink sink;
    ASSERT_TRUE(sink.InitializeAndAttach(memory.data(), memory.size(), records));

    const auto first = MakePacket(7);
    const auto gap = MakePacket(10);
    Consume(sink, View(first));
    Consume(sink, View(gap));
    Consume(sink, View(gap, 0x1D));

    auto* header = reinterpret_cast<ASFW::Protocols::DV::DVRingHeader*>(
        memory.data());
    EXPECT_EQ(header->dbcDiscontinuities.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header->rejectedPackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header->writeIndex.load(std::memory_order_acquire), 2u);
}

TEST(DVCaptureSinkTests, RejectsChunkWithInvalidDifIdentifier) {
    constexpr uint32_t records = 2;
    std::vector<uint8_t> memory(
        ASFW::Protocols::DV::DVCaptureSink::RequiredBytes(records));
    ASFW::Protocols::DV::DVCaptureSink sink;
    ASSERT_TRUE(sink.InitializeAndAttach(memory.data(), memory.size(), records));

    auto packet = MakePacket();
    packet[16] = 7u << 5;
    Consume(sink, View(packet));

    auto* header = reinterpret_cast<ASFW::Protocols::DV::DVRingHeader*>(
        memory.data());
    EXPECT_EQ(header->invalidSourcePackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header->writeIndex.load(std::memory_order_acquire), 0u);
}

TEST(DVCaptureSinkTests, PublishesConnectionAndBusResetState) {
    constexpr uint32_t records = 2;
    std::vector<uint8_t> memory(
        ASFW::Protocols::DV::DVCaptureSink::RequiredBytes(records));
    ASFW::Protocols::DV::DVCaptureSink sink;
    ASSERT_TRUE(sink.InitializeAndAttach(memory.data(), memory.size(), records));

    auto* header = reinterpret_cast<ASFW::Protocols::DV::DVRingHeader*>(
        memory.data());
    EXPECT_EQ(header->state.load(std::memory_order_acquire),
              static_cast<uint32_t>(
                  ASFW::Protocols::DV::CaptureState::Initializing));

    sink.MarkActive(12, 3);
    EXPECT_EQ(header->state.load(std::memory_order_acquire),
              static_cast<uint32_t>(
                  ASFW::Protocols::DV::CaptureState::Active));
    EXPECT_EQ(header->channel, 12u);
    EXPECT_EQ(header->connectionMode, 3u);

    sink.MarkTerminal(ASFW::Protocols::DV::CaptureState::BusReset);
    EXPECT_EQ(header->state.load(std::memory_order_acquire),
              static_cast<uint32_t>(
                  ASFW::Protocols::DV::CaptureState::BusReset));
}

TEST(DVCaptureSinkTests, PublishesTsPacketFromCapturedHvrZ1uHeader) {
    TsFixture fixture;
    const auto source = MakeSourcePacket(0x5A);
    fixture.Feed(MakeTsPacket(0xB8, Concat({source}), 0, 8));

    auto& header = fixture.Header();
    EXPECT_EQ(header.streamFormat.load(std::memory_order_acquire),
              static_cast<uint32_t>(ASFW::Protocols::DV::StreamFormat::MpegTs));
    EXPECT_EQ(header.sourcePacketBytes.load(std::memory_order_relaxed),
              Sink::kTsPacketBytes);
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(header.sourcePackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header.rejectedPackets.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(fixture.RecordHolds(0, source));
}

TEST(DVCaptureSinkTests, PublishesEverySourcePacketInOneBusPacket) {
    TsFixture fixture;
    const auto a = MakeSourcePacket(0x11);
    const auto b = MakeSourcePacket(0x22);
    const auto c = MakeSourcePacket(0x33);
    fixture.Feed(MakeTsPacket(0xF8, Concat({a, b, c}), 0, 24));
    // DBC wraps from 0xF8 through 0x0F; the next packet continues at 0x10.
    const auto d = MakeSourcePacket(0x44);
    fixture.Feed(MakeTsPacket(0x10, Concat({d}), 0, 8));

    auto& header = fixture.Header();
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 4u);
    EXPECT_EQ(header.dbcDiscontinuities.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(fixture.RecordHolds(0, a));
    EXPECT_TRUE(fixture.RecordHolds(1, b));
    EXPECT_TRUE(fixture.RecordHolds(2, c));
    EXPECT_TRUE(fixture.RecordHolds(3, d));
}

TEST(DVCaptureSinkTests, ReassemblesSourcePacketSplitAcrossBusPackets) {
    TsFixture fixture;
    const auto a = MakeSourcePacket(0x11);
    const auto b = MakeSourcePacket(0x22);
    const auto stream = Concat({a, b});
    fixture.Feed(MakeTsPacket(0, stream, 0, 11));  // a + first 3 blocks of b
    fixture.Feed(MakeTsPacket(11, stream, 11, 5)); // rest of b

    auto& header = fixture.Header();
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(header.dbcDiscontinuities.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(header.droppedFragments.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(fixture.RecordHolds(0, a));
    EXPECT_TRUE(fixture.RecordHolds(1, b));
}

TEST(DVCaptureSinkTests, EmptyPacketBetweenFragmentsKeepsReassembly) {
    TsFixture fixture;
    const auto a = MakeSourcePacket(0x11);
    const auto stream = Concat({a});
    fixture.Feed(MakeTsPacket(0, stream, 0, 5));
    fixture.Feed(MakeTsPacket(5, stream, 0, 0)); // CIP-only, DBC unchanged
    fixture.Feed(MakeTsPacket(5, stream, 5, 3));

    auto& header = fixture.Header();
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(header.dbcDiscontinuities.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(fixture.RecordHolds(0, a));
}

TEST(DVCaptureSinkTests, DbcGapDropsPartialSourcePacketAndResyncs) {
    TsFixture fixture;
    const auto a = MakeSourcePacket(0x11);
    const auto b = MakeSourcePacket(0x22);
    fixture.Feed(MakeTsPacket(0, Concat({a}), 0, 4)); // first half of a
    fixture.Feed(MakeTsPacket(16, Concat({b}), 0, 8)); // expected DBC 4

    auto& header = fixture.Header();
    EXPECT_EQ(header.dbcDiscontinuities.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header.droppedFragments.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 1u);
    EXPECT_TRUE(fixture.RecordHolds(0, b));
}

TEST(DVCaptureSinkTests, SkipsTailOfSourcePacketWhoseStartWasNotSeen) {
    TsFixture fixture;
    const auto lost = MakeSourcePacket(0x11);
    const auto c = MakeSourcePacket(0x33);
    // Capture starts mid-stream: the last 3 blocks of `lost`, then all of c.
    fixture.Feed(MakeTsPacket(5, Concat({lost, c}), 5, 11));

    auto& header = fixture.Header();
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(header.invalidSourcePackets.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(fixture.RecordHolds(0, c));
}

TEST(DVCaptureSinkTests, CountsTsPacketWithoutSyncByte) {
    TsFixture fixture;
    auto bad = MakeSourcePacket(0x11);
    bad[Sink::kSphBytes] = 0x00;
    fixture.Feed(MakeTsPacket(0, Concat({bad}), 0, 8));

    auto& header = fixture.Header();
    EXPECT_EQ(header.invalidSourcePackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 0u);
}

TEST(DVCaptureSinkTests, RejectsTsWithUnexpectedGeometry) {
    TsFixture fixture;
    const auto a = MakeSourcePacket(0x11);
    constexpr uint32_t kFractionZero = kTsQ0WithoutDbc & ~(3u << 14);
    fixture.Feed(MakeTsPacket(0, Concat({a}), 0, 8, kFractionZero));

    auto& header = fixture.Header();
    EXPECT_EQ(header.rejectedPackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header.streamFormat.load(std::memory_order_acquire),
              static_cast<uint32_t>(ASFW::Protocols::DV::StreamFormat::Unknown));
    EXPECT_EQ(header.lastRejectQ0.load(std::memory_order_relaxed),
              kFractionZero);
    EXPECT_EQ(header.lastRejectQ1.load(std::memory_order_relaxed), kTsQ1);
}

TEST(DVCaptureSinkTests, LockedTsSessionRejectsDvPackets) {
    TsFixture fixture;
    fixture.Feed(MakeTsPacket(0, Concat({MakeSourcePacket(0x11)}), 0, 8));
    const auto dv = MakePacket();
    Consume(fixture.sink, View(dv));

    auto& header = fixture.Header();
    EXPECT_EQ(header.streamFormat.load(std::memory_order_acquire),
              static_cast<uint32_t>(ASFW::Protocols::DV::StreamFormat::MpegTs));
    EXPECT_EQ(header.rejectedPackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header.lastRejectQ1.load(std::memory_order_relaxed), 0x8000FFFFu);
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 1u);
}

TEST(DVCaptureSinkTests, EmptyTsPacketDoesNotLockFormat) {
    TsFixture fixture;
    fixture.Feed(MakeTsPacket(0, {}, 0, 0));
    const auto dv = MakePacket();
    Consume(fixture.sink, View(dv));

    auto& header = fixture.Header();
    EXPECT_EQ(header.streamFormat.load(std::memory_order_acquire),
              static_cast<uint32_t>(ASFW::Protocols::DV::StreamFormat::Dv));
    EXPECT_EQ(header.rejectedPackets.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 1u);
}

TEST(DVCaptureSinkTests, TsOverrunIsCountedAndNotPublished) {
    TsFixture fixture(2);
    const auto stream = Concat({MakeSourcePacket(0x11), MakeSourcePacket(0x22),
                                MakeSourcePacket(0x33)});
    fixture.Feed(MakeTsPacket(0, stream, 0, 24));

    auto& header = fixture.Header();
    EXPECT_EQ(header.writeIndex.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(header.sourcePackets.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(header.overruns.load(std::memory_order_relaxed), 1u);
}
