#include <gtest/gtest.h>

#include "Protocols/DV/DVCaptureSink.hpp"

#include <array>
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
    EXPECT_EQ(header->dvSourcePackets.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(header->nonDvPackets.load(std::memory_order_relaxed), 0u);
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
    EXPECT_EQ(header->nonDvPackets.load(std::memory_order_relaxed), 1u);
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
    EXPECT_EQ(header->invalidDifBlocks.load(std::memory_order_relaxed), 1u);
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
