#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/DriverKit/Config/AVC/MAudioSpecialProfile.hpp"
#include "../support/MAudioSpecialHappyPathFixture.inc"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;

class TestTxSlotProvider final : public IAmdtpTxSlotProvider {
public:
    bool allowAcquire{false};
    bool allowPublish{false};
    std::array<uint8_t, 128> bytes{};
    PreparedTxPacket publishedPacket{};

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
        const PreparedTxPacket& packet) noexcept override {
        publishedPacket = packet;
        return allowPublish;
    }

    uint32_t SlotCount() const noexcept override {
        return 1;
    }
};

class CaptureTxSlotProvider final : public IAmdtpTxSlotProvider {
public:
    std::array<uint8_t, 232> bytes{};
    PreparedTxPacket published{};

    bool AcquireWritableSlot(uint32_t packetIndex,
                             TxPacketSlotView& outSlot) noexcept override {
        outSlot = {packetIndex, bytes.data(), static_cast<uint32_t>(bytes.size())};
        return true;
    }
    bool PublishSlot(const PreparedTxPacket& packet) noexcept override {
        published = packet;
        return true;
    }
    uint32_t SlotCount() const noexcept override { return 1; }
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

TEST(AmdtpDirectTxTests,
     MAudioCadenceNoDataCarriesFullBlocksCFSlotsAndAdvancesDbc) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    AmdtpStreamConfig config{};
    config.streamMode = StreamMode::Blocking;
    config.dbs = 7; // six PCM slots and one MIDI slot
    config.pcmChannels = 6;
    config.midiSlots = 1;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 232;

    AmdtpTxPolicy policy{};
    policy.cadencePacketsCarryDataBlocks = true;
    policy.cadenceSlotWord = 0xCF000000;
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, policy));

    std::array<std::array<uint8_t, 232>, 3> bytes{};
    TxPresentationPlan plan{};
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;
    PreparedTxPacket packet{};
    for (uint32_t index = 0; index < 2; ++index) {
        plan.cycleOrdinal = index;
        ASSERT_TRUE(packetizer.PrepareNextPacket(
            {index, bytes[index].data(),
             static_cast<uint32_t>(bytes[index].size())}, {}, plan, packet));
        ASSERT_TRUE(packet.isData);
        EXPECT_EQ(packet.dbc, index * 8U);
    }

    plan.cycleOrdinal = 2;
    plan.disposition = AmdtpPacketDisposition::NoData;
    plan.frameCount = 0;
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {2, bytes[2].data(), bytes[2].size()}, {}, plan, packet));
    EXPECT_FALSE(packet.isData);
    EXPECT_EQ(packet.framesInPacket, 0U);
    EXPECT_EQ(packet.byteCount, 232U);
    EXPECT_EQ(packet.dbc, 16U);
    EXPECT_EQ(bytes[2][3], 16U); // DBC for the first block in this packet.
    EXPECT_EQ(bytes[2][7], 0xFFU); // NO-DATA SYT.

    // Six audio slots are labelled CF; the seventh slot remains the MIDI
    // non-audio word. This matches the working 1814 host-to-device capture.
    for (uint32_t block = 0; block < 8; ++block) {
        const uint8_t* base = bytes[2].data() + 8 + block * 7 * 4;
        for (uint32_t slot = 0; slot < 6; ++slot) {
            EXPECT_EQ(base[slot * 4], 0xCFU) << "block " << block;
            EXPECT_EQ(base[slot * 4 + 1], 0U) << "block " << block;
            EXPECT_EQ(base[slot * 4 + 2], 0U) << "block " << block;
            EXPECT_EQ(base[slot * 4 + 3], 0U) << "block " << block;
        }
        EXPECT_EQ(base[6 * 4], 0x80U) << "block " << block;
        EXPECT_EQ(base[6 * 4 + 1], 0U) << "block " << block;
        EXPECT_EQ(base[6 * 4 + 2], 0U) << "block " << block;
        EXPECT_EQ(base[6 * 4 + 3], 0U) << "block " << block;
    }

    plan.cycleOrdinal = 3;
    plan.disposition = AmdtpPacketDisposition::Data;
    plan.frameCount = 8;
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {3, bytes[1].data(), bytes[1].size()}, {}, plan, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.dbc, 24U);
}

TEST(AmdtpDirectTxTests, MAudioRevertedDataKeepsFullSizeCadenceAndDbc) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));
    AmdtpStreamConfig config{};
    config.streamMode = StreamMode::Blocking;
    config.dbs = 7;
    config.pcmChannels = 6;
    config.midiSlots = 1;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 232;
    AmdtpTxPolicy policy{};
    policy.cadencePacketsCarryDataBlocks = true;
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, policy));

    std::array<uint8_t, 232> bytes{};
    const TxPacketSlotView slot{0, bytes.data(), bytes.size()};
    TxPresentationPlan plan{};
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(slot, {}, plan, packet));
    ASSERT_TRUE(packet.isData);
    packetizer.RevertToNoData(slot, packet);
    EXPECT_FALSE(packet.isData);
    EXPECT_EQ(packet.byteCount, 232U);
    EXPECT_EQ(packet.framesInPacket, 0U);
    EXPECT_EQ(packet.dbc, 0U);
    EXPECT_EQ(bytes[8], 0xCFU);
    EXPECT_EQ(bytes[8 + 6 * 4], 0x80U);

    plan.cycleOrdinal = 1;
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes.data(), bytes.size()}, {}, plan, packet));
    EXPECT_EQ(packet.dbc, 8U);
}

TEST(AmdtpDirectTxTests, Captured1814HostPacketMatchesProfileAndEngine) {
    namespace Capture = MAudioSpecialHappyPathFixture;
    const Capture::Event* capturedPacket = nullptr;
    for (const auto& event : Capture::kEvents) {
        if (event.kind == Capture::EventKind::IsochPacket && event.dst == 0 &&
            event.size == 232 && event.payloadSize == 232) {
            capturedPacket = &event;
            break;
        }
    }
    ASSERT_NE(capturedPacket, nullptr);

    ASFW::Isoch::Audio::AVC::Profiles::MAudioSpecialProfile profile(false);
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(config));
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(engine.Configure(profile, config));
    CaptureTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    engine.ResetForStart(0, 0);
    AmdtpTimingState timing{};
    timing.replayValid = true;
    timing.disposition = AmdtpPacketDisposition::NoData;
    ASSERT_EQ(engine.PrepareNextTransmitSlot(0, timing), TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(provider.published.byteCount, capturedPacket->size);
    EXPECT_TRUE(std::equal(provider.bytes.begin(), provider.bytes.end(),
                           capturedPacket->payload));
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

TEST(AmdtpDirectTxTests, DiceTxStreamEngineSuppliesPresentationPlanAndControlsPacketFraming) {
    class TestAudioProfile final : public ASFW::Isoch::Audio::IAudioStreamProfile {
    public:
        const char* Name() const noexcept override { return "TestAudioProfile"; }
        ASFW::Encoding::AudioWireFormat TxWireFormat() const noexcept override { return {}; }
        ASFW::Encoding::AudioWireFormat RxWireFormat() const noexcept override { return {}; }
        uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
        uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
        uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
        uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }

        bool BuildDefaultTxStreamConfig(ASFW::Isoch::Audio::AudioStreamConfig& c) const noexcept override {
            c = {};
            c.direction = ASFW::Isoch::Audio::AudioStreamDirection::HostToDevice;
            c.sampleRate = 48000;
            c.streamMode = ASFW::Encoding::StreamMode::kBlocking;
            c.pcmChannels = 2;
            c.dbs = 2;
            c.midiSlots = 0;
            c.framesPerDataPacket = 8;
            c.fdf = 0x02;
            c.fmt = 0x10;
            c.sid = 0;
            return true;
        }
        bool BuildDefaultRxStreamConfig(ASFW::Isoch::Audio::AudioStreamConfig& c) const noexcept override {
            return BuildDefaultTxStreamConfig(c);
        }
    };

    DiceTxStreamEngine engine{};
    TestAudioProfile profile{};
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(config));
    ASSERT_TRUE(engine.Configure(profile, config));

    TestTxSlotProvider provider{};
    provider.allowAcquire = true;
    provider.allowPublish = true;
    engine.BindSlotProvider(&provider);

    // Initial state: not aligned, frame 0
    EXPECT_FALSE(engine.IsFrameCursorAligned());
    EXPECT_EQ(engine.NextAudioFrame(), 0U);

    // 1. One-shot cursor alignment
    EXPECT_TRUE(engine.AlignFrameCursorOnce(48000U));
    EXPECT_TRUE(engine.IsFrameCursorAligned());
    EXPECT_EQ(engine.NextAudioFrame(), 48000U);

    // Subsequent alignment rejected
    EXPECT_FALSE(engine.AlignFrameCursorOnce(96000U));
    EXPECT_EQ(engine.NextAudioFrame(), 48000U);

    // 2. Prepare next transmit slot with presentation plan
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x3456;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;

    EXPECT_EQ(
        engine.PrepareNextTransmitSlot(0, timing),
        TxSlotPrepareResult::kPrepared);

    EXPECT_TRUE(provider.publishedPacket.isData);
    EXPECT_EQ(provider.publishedPacket.firstAudioFrame, 48000U);
    EXPECT_EQ(provider.publishedPacket.framesInPacket, 8U);
    EXPECT_EQ(provider.publishedPacket.syt, 0x3456U);
    // Cursor advanced by 8 frames
    EXPECT_EQ(engine.NextAudioFrame(), 48008U);

    // 3. Re-arm cursor alignment after replay interruption
    engine.ReArmFrameCursorAlignment();
    EXPECT_FALSE(engine.IsFrameCursorAligned());

    // Can re-align after re-arm
    EXPECT_TRUE(engine.AlignFrameCursorOnce(100000U));
    EXPECT_TRUE(engine.IsFrameCursorAligned());
    EXPECT_EQ(engine.NextAudioFrame(), 100000U);

    // Next prepared slot uses new cursor position from the plan
    EXPECT_EQ(
        engine.PrepareNextTransmitSlot(1, timing),
        TxSlotPrepareResult::kPrepared);

    EXPECT_TRUE(provider.publishedPacket.isData);
    EXPECT_EQ(provider.publishedPacket.firstAudioFrame, 100000U);
    EXPECT_EQ(provider.publishedPacket.framesInPacket, 8U);
    EXPECT_EQ(engine.NextAudioFrame(), 100008U);
}

} // namespace

TEST(AmdtpDirectTxTests, RevertedEndEventPacketRestoresDbcAndFrameTelemetry) {
    AmdtpTxPacketizer packetizer;
    AmdtpPacketTimeline timeline;
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    packetizer.BindTimeline(&timeline);
    AmdtpStreamConfig config{};
    config.sampleRate = 48000;
    config.dbs = 2;
    config.pcmChannels = 2;
    config.framesPerDataPacket = 8;
    AmdtpTxPolicy policy{};
    policy.dbcIsEndEvent = true;
    ASSERT_TRUE(packetizer.Configure(config, policy));
    packetizer.Reset(250, 100);
    std::array<uint8_t, 128> bytes{};
    TxPacketSlotView slot{.packetIndex = 0, .bytes = bytes.data(), .capacityBytes = bytes.size()};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    TxPresentationPlan plan{};
    plan.firstAudioFrame = 100;
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(slot, timing, plan, packet));
    EXPECT_EQ(packet.dbc, 2U);
    packetizer.RevertToNoData(slot, packet);
    EXPECT_EQ(packet.dbc, 250U);
    EXPECT_EQ(bytes[3], 250U);
    EXPECT_EQ(packetizer.TelemetrySnapshot().nextAudioFrame, 100U);
    EXPECT_FALSE(packetizer.TelemetrySnapshot().hasLastDataPacket);
    slot.packetIndex = 1;
    ASSERT_TRUE(packetizer.PrepareNextPacket(slot, timing, plan, packet));
    EXPECT_EQ(packet.dbc, 2U);
    EXPECT_EQ(packet.firstAudioFrame, 100U);
}
