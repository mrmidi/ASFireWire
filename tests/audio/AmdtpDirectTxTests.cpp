// Modified in 2026 by Rafal Zalech to add original MOTU UltraLite support.
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/Wire/MOTU/MotuBlockCodec.hpp"
#include "Audio/Wire/MOTU/MotuSph.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;

class TestTxSlotProvider final : public IAmdtpTxSlotProvider {
public:
    bool allowAcquire{false};
    bool allowPublish{false};
    std::array<uint8_t, 128> bytes{};

    bool AcquireWritableSlot(
        uint32_t packetIndex,
        TxPacketSlotView& outSlot) noexcept override {
        if (!allowAcquire) {
            return false;
        }
        outSlot = {
            .packetIndex = packetIndex,
            .bytes = bytes.data(),
            .capacityBytes =
                static_cast<uint32_t>(bytes.size()),
        };
        return true;
    }

    bool PublishSlot(
        const PreparedTxPacket&) noexcept override {
        return allowPublish;
    }

    uint32_t SlotCount() const noexcept override {
        return 1;
    }
};

AmdtpStreamConfig BlockingStereoConfig() {
    AmdtpStreamConfig config{};
    config.streamMode = StreamMode::Blocking;
    config.dbs = 2;
    config.pcmChannels = 2;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 128;
    return config;
}

AmdtpStreamConfig MotuUltraLiteConfig() {
    AmdtpStreamConfig config{};
    config.sampleRate = 48000;
    config.streamMode = StreamMode::Blocking;
    config.dbs = 13;
    config.pcmChannels = 14;
    config.fmt = 0x02;
    config.fdf = 0x22;
    config.sph = true;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 424;
    return config;
}

TEST(AmdtpDirectTxTests, Int32EncodingUsesHighSigned24Bits) {
    EXPECT_EQ(PcmSlotCodec::EncodeInt32(
                  INT32_MAX, PcmSlotEncoding::RawSigned24In32BE),
              0x007FFFFFu);
    EXPECT_EQ(PcmSlotCodec::EncodeInt32(
                  INT32_MIN, PcmSlotEncoding::RawSigned24In32BE),
              0xFF800000u);
    EXPECT_EQ(PcmSlotCodec::EncodeInt32(
                  INT32_MAX, PcmSlotEncoding::Am824MBLA),
              0x407FFFFFu);
}

TEST(AmdtpDirectTxTests, ForcedNoDataHoldsDbcAndAudioFrame) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(
        timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(
        BlockingStereoConfig(), AmdtpTxPolicy{}));

    std::array<std::array<uint8_t, 128>, 3> bytes{};
    PreparedTxPacket first{};
    PreparedTxPacket forced{};
    PreparedTxPacket data{};

    AmdtpTimingState allowData{};
    allowData.txClockValid = true;
    allowData.disposition = AmdtpPacketDisposition::Data;
    allowData.nextDataSyt = 0x1234;
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, allowData, first));
    EXPECT_FALSE(first.isData);
    EXPECT_EQ(first.byteCount, 8U);
    EXPECT_EQ(bytes[0][4], 0x90);
    EXPECT_EQ(bytes[0][5], 0xFF);
    EXPECT_EQ(bytes[0][6], 0xFF);
    EXPECT_EQ(bytes[0][7], 0xFF);

    AmdtpTimingState noData{};
    noData.disposition = AmdtpPacketDisposition::NoData;
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, noData, forced));
    EXPECT_FALSE(forced.isData);
    EXPECT_EQ(forced.byteCount, 8U);
    EXPECT_EQ(forced.dbc, first.dbc);
    EXPECT_EQ(forced.firstAudioFrame, 0U);

    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {2, bytes[2].data(), bytes[2].size()}, allowData, data));
    EXPECT_TRUE(data.isData);
    EXPECT_EQ(data.dbc, first.dbc);
    EXPECT_EQ(data.firstAudioFrame, 0U);
    EXPECT_EQ(data.framesInPacket, 8U);
    EXPECT_EQ(data.syt, 0x1234U);
}

TEST(AmdtpDirectTxTests, NoDataFdfCanUseSaffireCompatibilityQuirk) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(
        timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPolicy policy{};
    policy.preserveFdfInNoDataPackets = true;

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(BlockingStereoConfig(), policy));

    std::array<uint8_t, 128> bytes{};
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::NoData;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, packet));

    ASSERT_FALSE(packet.isData);
    EXPECT_EQ(bytes[4], 0x90);
    EXPECT_EQ(bytes[5], 0x02);
    EXPECT_EQ(bytes[6], 0xFF);
    EXPECT_EQ(bytes[7], 0xFF);
}

TEST(AmdtpDirectTxTests, ReplayOverridesLocalCadencePerPhysicalCycle) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(
        timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(
        BlockingStereoConfig(), AmdtpTxPolicy{}));

    std::array<std::array<uint8_t, 128>, 2> bytes{};
    AmdtpTimingState data{};
    data.txClockValid = true;
    data.disposition = AmdtpPacketDisposition::Data;
    data.nextDataSyt = 0x2345;
    data.replayValid = true;
    data.replayDataBlocks = 8;

    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, data, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.framesInPacket, 8U);
    EXPECT_EQ(packet.syt, 0x2345U);

    AmdtpTimingState noData{};
    noData.disposition = AmdtpPacketDisposition::NoData;
    noData.replayValid = true;
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, noData, packet));
    EXPECT_FALSE(packet.isData);
    EXPECT_EQ(packet.framesInPacket, 0U);
    EXPECT_EQ(packet.dbc, 8U);
}

TEST(AmdtpDirectTxTests, MotuPacketUsesEndDbcNoInfoSytAndReplayedSph) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(MotuUltraLiteConfig(), AmdtpTxPolicy{}));

    std::array<uint8_t, 320> bytes{};
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = true;
    timing.replayDataBlocks = 6;
    timing.packetCycleTicks = 100 * ASFW::Encoding::Motu::kTicksPerCycle;
    timing.motuSphCount = 6;
    timing.motuSphOffsets = {10, 567, 1124, 1681, 2238, 2795, 0, 0};

    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.byteCount, 320U);
    EXPECT_EQ(packet.framesInPacket, 6U);
    EXPECT_EQ(packet.dbc, 6U);
    EXPECT_EQ(packet.syt, 0xFFFFU);

    // DBS=13, SPH=1 and end-event DBC=6 in CIP Q0; MOTU fmt/fdf and
    // SYT_NO_INFO in Q1.
    EXPECT_EQ(bytes[1], 13U);
    EXPECT_EQ(bytes[2] & 0x04U, 0x04U);
    EXPECT_EQ(bytes[3], 6U);
    EXPECT_EQ(bytes[4], 0x82U);
    EXPECT_EQ(bytes[5], 0x22U);
    EXPECT_EQ(bytes[6], 0xFFU);
    EXPECT_EQ(bytes[7], 0xFFU);

    constexpr uint32_t kBlockBytes = 13U * 4U;
    for (uint32_t i = 0; i < 6; ++i) {
        const std::span<const uint8_t> block(
            bytes.data() + 8 + i * kBlockBytes, kBlockBytes);
        const uint32_t expected = ASFW::Encoding::Motu::ReplaySph(
            timing.motuSphOffsets[i],
            static_cast<uint32_t>(timing.packetCycleTicks));
        EXPECT_EQ(ASFW::Encoding::Motu::ReadSph(block), expected);
    }
}

TEST(AmdtpDirectTxTests, MotuPacketRejectsMissingSphReplayData) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(MotuUltraLiteConfig(), AmdtpTxPolicy{}));

    std::array<uint8_t, 320> bytes{};
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = true;
    timing.replayDataBlocks = 6;
    timing.motuSphCount = 5;
    PreparedTxPacket packet{};
    EXPECT_FALSE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, packet));
}

TEST(AmdtpDirectTxTests, MotuPacketAcceptsLargestReplayedCadencePacket) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(MotuUltraLiteConfig(), AmdtpTxPolicy{}));

    std::array<uint8_t, 424> bytes{};
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;
    timing.packetCycleTicks = 200 * ASFW::Encoding::Motu::kTicksPerCycle;
    timing.motuSphCount = 8;
    timing.motuSphOffsets = {10, 567, 1124, 1681, 2238, 2795, 3352, 3909};

    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.byteCount, 424U);
    EXPECT_EQ(packet.framesInPacket, 8U);
    EXPECT_EQ(packet.dbc, 8U);
}

TEST(AmdtpDirectTxTests, PayloadWriterReadsMappedInt32RingDirectly) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(
        timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    const auto config = BlockingStereoConfig();
    ASSERT_TRUE(packetizer.Configure(config, AmdtpTxPolicy{}));

    std::array<uint8_t, 128> noDataBytes{};
    std::array<uint8_t, 128> dataBytes{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x2222;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, noDataBytes.data(), noDataBytes.size()}, timing, packet));
    ASSERT_FALSE(packet.isData);
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, dataBytes.data(), dataBytes.size()}, timing, packet));
    ASSERT_TRUE(packet.isData);

    AmdtpPayloadWriter writer{};
    writer.Configure(config, AmdtpTxPolicy{});
    writer.BindTimeline(&timeline);
    std::array<float, 16> mappedRing{};
    mappedRing[0] = 1.0f;
    mappedRing[1] = -1.0f;
    // completionCursor 0: no packet counts as already transmitted here.
    writer.WriteFloat32Interleaved(
        {mappedRing.data(), 0, 8, 8, 2}, 0);

    EXPECT_EQ(dataBytes[8], 0x40);
    EXPECT_EQ(dataBytes[9], 0x7F);
    EXPECT_EQ(dataBytes[10], 0xFF);
    EXPECT_EQ(dataBytes[11], 0xFF);
    EXPECT_EQ(dataBytes[12], 0x40);
    EXPECT_EQ(dataBytes[13], 0x80);
    EXPECT_EQ(dataBytes[14], 0x00);
    EXPECT_EQ(dataBytes[15], 0x01);
}

TEST(AmdtpDirectTxTests, MotuPayloadWriterMapsLogicalMainOutToWireSlotsElevenAndTwelve) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    auto config = MotuUltraLiteConfig();
    AmdtpTxPolicy policy{};
    policy.hostToDevicePcmEncoding = PcmSlotEncoding::MotuV2Packed24;
    policy.sourceChannelMapEnabled = true;
    policy.sourceChannelForWireSlot = {
        12, 13, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 10, 11,
    };

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, policy));

    std::array<uint8_t, 320> packetBytes{};
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = true;
    timing.replayDataBlocks = 6;
    timing.motuSphCount = 6;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, packetBytes.data(), packetBytes.size()}, timing, packet));
    ASSERT_TRUE(packet.isData);

    AmdtpPayloadWriter writer{};
    writer.Configure(config, policy);
    writer.BindTimeline(&timeline);
    std::array<float, 6 * 14> hostRing{};
    for (uint32_t frame = 0; frame < 6; ++frame) {
        hostRing[frame * 14] = 1.0f;       // logical Main Out left
        hostRing[frame * 14 + 10] = -1.0f; // logical channel 11
    }
    writer.WriteFloat32Interleaved(
        {hostRing.data(), 0, 6, 6, 14}, 0);

    const std::span<const uint8_t> firstBlock(
        packetBytes.data() + 8, ASFW::Encoding::Motu::DataBlockBytes(14));
    EXPECT_EQ(ASFW::Encoding::Motu::ReadPcmChannel(firstBlock, 12),
              static_cast<int32_t>(0x80000100));
    EXPECT_EQ(ASFW::Encoding::Motu::ReadPcmChannel(firstBlock, 10),
              0x7FFFFF00);
}

TEST(AmdtpDirectTxTests, PayloadWriterCountsUnderExposureAtCallBoundary) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(
        timelineSlots.data(), timelineSlots.size()));

    AmdtpPayloadWriter writer{};
    writer.Configure(BlockingStereoConfig(), AmdtpTxPolicy{});
    writer.BindTimeline(&timeline);

    std::array<float, 16> mappedRing{};
    writer.WriteFloat32Interleaved(
        {mappedRing.data(), 0, 8, 8, 2}, 0);

    const auto& counters = writer.Counters();
    EXPECT_EQ(
        counters.underExposureCalls.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(
        counters.underExposureFrames.load(std::memory_order_relaxed), 8U);
    EXPECT_EQ(
        counters.framesWithoutPacket.load(std::memory_order_relaxed), 8U);
}

TEST(AmdtpDirectTxTests, AlignFrameCursorIsAcceptedOnlyOncePerReset) {
    AmdtpTxPacketizer packetizer{};

    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(BlockingStereoConfig(), AmdtpTxPolicy{}));
    EXPECT_TRUE(packetizer.AlignFrameCursorOnce(960U));
    EXPECT_FALSE(packetizer.AlignFrameCursorOnce(2000U));

    std::array<std::array<uint8_t, 128>, 3> bytes{};
    PreparedTxPacket packet{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x1234;

    // Packet index 0: not data in cadence
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, timing, packet));
    EXPECT_FALSE(packet.isData);

    // Packet index 1: data in cadence
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, timing, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.firstAudioFrame, 960U);

    // Packet index 2: data in cadence
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {2, bytes[2].data(), bytes[2].size()}, timing, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.firstAudioFrame, 968U);

    packetizer.Reset(0, 0);
    EXPECT_TRUE(packetizer.AlignFrameCursorOnce(3000U));
}

TEST(AmdtpDirectTxTests, PacketizerTelemetryTracksCursorAlignmentAndLastDataRange) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(BlockingStereoConfig(), AmdtpTxPolicy{}));
    EXPECT_TRUE(packetizer.AlignFrameCursorOnce(960U));

    std::array<std::array<uint8_t, 128>, 2> bytes{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x1234;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, timing, packet));
    ASSERT_FALSE(packet.isData);
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, timing, packet));
    ASSERT_TRUE(packet.isData);

    const auto snapshot = packetizer.TelemetrySnapshot();
    EXPECT_TRUE(snapshot.frameCursorAligned);
    EXPECT_TRUE(snapshot.hasLastDataPacket);
    EXPECT_EQ(snapshot.nextAudioFrame, 968U);
    EXPECT_EQ(snapshot.lastDataFirstAudioFrame, 960U);
    EXPECT_EQ(snapshot.lastDataEndAudioFrame, 968U);
    EXPECT_EQ(snapshot.lastDataPacketIndex, 1U);

    packetizer.ReArmFrameCursorAlignment();
    const auto rearmed = packetizer.TelemetrySnapshot();
    EXPECT_FALSE(rearmed.frameCursorAligned);
    EXPECT_GT(rearmed.cursorEpoch, snapshot.cursorEpoch);
}

TEST(AmdtpDirectTxTests, TxEngineReportsPreparationFailureStage) {
    DiceTxStreamEngine engine{};
    AmdtpTimingState timing{};

    EXPECT_EQ(
        engine.PrepareNextTransmitSlot(192, timing),
        TxSlotPrepareResult::kSlotProviderUnavailable);

    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    EXPECT_EQ(
        engine.PrepareNextTransmitSlot(192, timing),
        TxSlotPrepareResult::kSlotAcquireFailed);

    provider.allowAcquire = true;
    EXPECT_EQ(
        engine.PrepareNextTransmitSlot(192, timing),
        TxSlotPrepareResult::kPacketizerRejected);
}

} // namespace
