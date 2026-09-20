#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"

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

TEST(AmdtpDirectTxTests, DbcIsEndEventWritesTheCountAfterEachDataPacket) {
    // MOTU counts the DBC at the end of a packet's blocks; Linux sets CIP_DBC_IS_END_EVENT
    // on every MOTU transmit stream (amdtp-motu.c:465, amdtp-stream.c:1040-1046). Run the
    // default and end-event packetizers through the same sequence: each DATA packet must
    // differ by exactly its block count, and a NO-DATA packet -- no blocks -- not at all.
    AmdtpPacketTimeline timelineStart{};
    AmdtpPacketTimeline timelineEnd{};
    std::array<PacketTimelineSlot, 8> slotsStart{};
    std::array<PacketTimelineSlot, 8> slotsEnd{};
    ASSERT_TRUE(timelineStart.AttachSlots(slotsStart.data(), slotsStart.size()));
    ASSERT_TRUE(timelineEnd.AttachSlots(slotsEnd.data(), slotsEnd.size()));

    AmdtpTxPacketizer start{};
    AmdtpTxPacketizer end{};
    start.BindTimeline(&timelineStart);
    end.BindTimeline(&timelineEnd);
    ASSERT_TRUE(start.Configure(BlockingStereoConfig(), AmdtpTxPolicy{}));
    ASSERT_TRUE(end.Configure(BlockingStereoConfig(), AmdtpTxPolicy{.dbcIsEndEvent = true}));

    AmdtpTimingState allowData{};
    allowData.txClockValid = true;
    allowData.disposition = AmdtpPacketDisposition::Data;
    allowData.nextDataSyt = 0x1234;

    uint32_t dataPackets = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        std::array<uint8_t, 128> bytesStart{};
        std::array<uint8_t, 128> bytesEnd{};
        PreparedTxPacket a{};
        PreparedTxPacket b{};
        ASSERT_TRUE(start.PrepareNextPacket({i, bytesStart.data(), bytesStart.size()},
                                            allowData, a));
        ASSERT_TRUE(end.PrepareNextPacket({i, bytesEnd.data(), bytesEnd.size()},
                                          allowData, b));
        ASSERT_EQ(a.isData, b.isData) << "packet " << i;
        const uint8_t expected = a.isData
            ? static_cast<uint8_t>((a.dbc + a.framesInPacket) & 0xFFU)
            : a.dbc;
        EXPECT_EQ(b.dbc, expected) << "packet " << i;
        // The CIP header must carry it too (DBC is the last byte of quadlet 0).
        EXPECT_EQ(bytesEnd[3], expected) << "packet " << i;
        dataPackets += a.isData ? 1U : 0U;
    }
    EXPECT_GT(dataPackets, 1U) << "fixture must exercise consecutive data packets";
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

TEST(AmdtpDirectTxTests, TxPresentationPlanSuppliedFrameRangeDeterminesPacketAndWriterSelection) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    const auto config = BlockingStereoConfig();
    ASSERT_TRUE(packetizer.Configure(config, AmdtpTxPolicy{}));

    std::array<uint8_t, 128> bytes{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x5678;

    TxPresentationPlan plan{};
    plan.epoch = packetizer.CursorEpoch();
    plan.cycleOrdinal = 0;
    plan.firstAudioFrame = 12345;
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;

    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, plan, packet));

    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.firstAudioFrame, 12345U);
    EXPECT_EQ(packet.framesInPacket, 8U);
    EXPECT_EQ(packetizer.NextAudioFrame(), 12345U + 8U);

    // Payload writer writes based on packetizer's timeline exposure
    AmdtpPayloadWriter writer{};
    writer.Configure(config, AmdtpTxPolicy{});
    writer.BindTimeline(&timeline);

    // Provide a host ring with known values
    std::array<float, 64> hostRing{};
    // Offset in ring corresponding to frame 12345 % 32:
    const uint64_t ringOffset = (12345 % 32) * 2;
    hostRing[ringOffset] = 0.5f;
    hostRing[ringOffset + 1] = -0.5f;

    writer.WriteFloat32Interleaved(
        {hostRing.data(), 12345, 8, 32, 2}, 0);

    // Verify PCM slot in packet received the sample from the specified frame
    const uint32_t sample0 = (static_cast<uint32_t>(bytes[8]) << 24) |
                             (static_cast<uint32_t>(bytes[9]) << 16) |
                             (static_cast<uint32_t>(bytes[10]) << 8) |
                             static_cast<uint32_t>(bytes[11]);
    EXPECT_EQ(sample0, PcmSlotCodec::EncodeFloat32(0.5f, PcmSlotEncoding::Am824MBLA));
}

TEST(AmdtpDirectTxTests, TxPresentationPlanStaleEpochCannotPublish) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(BlockingStereoConfig(), AmdtpTxPolicy{}));

    std::array<uint8_t, 128> bytes{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;

    TxPresentationPlan plan{};
    plan.epoch = packetizer.CursorEpoch();
    plan.cycleOrdinal = 0;
    plan.firstAudioFrame = 100;
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;

    // Advance epoch via AlignFrameCursorOnce
    ASSERT_TRUE(packetizer.AlignFrameCursorOnce(100));
    EXPECT_NE(plan.epoch, packetizer.CursorEpoch());

    PreparedTxPacket packet{};
    // Stale epoch must be rejected
    EXPECT_FALSE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, plan, packet));

    // Update plan with current epoch -> succeeds
    plan.epoch = packetizer.CursorEpoch();
    EXPECT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, plan, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.firstAudioFrame, 100U);
}

TEST(AmdtpDirectTxTests, TxPresentationPlanFailureDoesNotAdvanceStateAndCanRetry) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(BlockingStereoConfig(), AmdtpTxPolicy{}));

    std::array<uint8_t, 128> bytes{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;

    TxPresentationPlan plan{};
    plan.epoch = 999; // Wrong epoch
    plan.cycleOrdinal = 0;
    plan.firstAudioFrame = 500;
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;

    PreparedTxPacket packet{};
    // Fails due to wrong epoch
    EXPECT_FALSE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, plan, packet));

    // State not advanced
    EXPECT_EQ(packetizer.NextAudioFrame(), 0U);

    // Also fail due to insufficient capacity
    plan.epoch = packetizer.CursorEpoch();
    EXPECT_FALSE(packetizer.PrepareNextPacket(
        {0, bytes.data(), 4}, timing, plan, packet));
    EXPECT_EQ(packetizer.NextAudioFrame(), 0U);

    // Retry with corrected slot & plan succeeds
    EXPECT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, plan, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.firstAudioFrame, 500U);
    EXPECT_EQ(packet.dbc, 0U);
    EXPECT_EQ(packetizer.NextAudioFrame(), 508U);
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
