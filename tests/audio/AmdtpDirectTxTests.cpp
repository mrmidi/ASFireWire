#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/DriverKit/Config/AVC/MAudioSpecialProfile.hpp"
#include "Audio/DriverKit/Config/AVC/GenericAvcProfile.hpp"
#include "Audio/DriverKit/Config/MOTU/MOTU828Mk3Profile.hpp"
#include "Audio/DriverKit/Config/ResolvedStreamConfig.hpp"
#include "Audio/Wire/AMDTP/MotuV3WireFormat.hpp"
#include "../support/MAudioSpecialHappyPathFixture.inc"

#include "TxPacketizerTestSupport.hpp"

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
        uint64_t packetIndex,
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

    bool AcquireWritableSlot(uint64_t packetIndex,
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

TEST(AmdtpDirectTxTests, Upper24LittleEndianCodecHasIndependentWireGoldens) {
    using ASFW::Protocols::Audio::AMDTP::PcmSlotCodec;
    using ASFW::Protocols::Audio::AMDTP::PcmSlotEncoding;
    const auto wireBytes = [](uint32_t encoded) {
        return std::array<uint8_t, 4>{
            static_cast<uint8_t>(encoded >> 24), static_cast<uint8_t>(encoded >> 16),
            static_cast<uint8_t>(encoded >> 8), static_cast<uint8_t>(encoded)};
    };
    EXPECT_EQ(wireBytes(PcmSlotCodec::EncodeInt32(INT32_MAX,
                      PcmSlotEncoding::RawPcm24Upper24In32LE)),
              (std::array<uint8_t, 4>{0x00, 0xFF, 0xFF, 0x7F}));
    EXPECT_EQ(wireBytes(PcmSlotCodec::EncodeInt32(INT32_MIN,
                      PcmSlotEncoding::RawPcm24Upper24In32LE)),
              (std::array<uint8_t, 4>{0x00, 0x00, 0x00, 0x80}));
    EXPECT_EQ(wireBytes(PcmSlotCodec::EncodeInt32(0,
                      PcmSlotEncoding::RawPcm24Upper24In32LE)),
              (std::array<uint8_t, 4>{0, 0, 0, 0}));
    EXPECT_NE(PcmSlotCodec::EncodeInt32(INT32_MAX,
                  PcmSlotEncoding::RawPcm24Upper24In32LE),
              PcmSlotCodec::EncodeInt32(INT32_MAX,
                  PcmSlotEncoding::RawSigned24In32LE));
    EXPECT_EQ(PcmSlotCodec::EncodeFloat32(2.0f,
                  PcmSlotEncoding::RawPcm24Upper24In32LE),
              PcmSlotCodec::EncodeFloat32(1.0f,
                  PcmSlotEncoding::RawPcm24Upper24In32LE));
    EXPECT_EQ(PcmSlotCodec::EncodeFloat32(-2.0f,
                  PcmSlotEncoding::RawPcm24Upper24In32LE),
              PcmSlotCodec::EncodeFloat32(-1.0f,
                  PcmSlotEncoding::RawPcm24Upper24In32LE));
}

TEST(AmdtpDirectTxTests, HeaderlessPacketSizingAndNoDataOperationAreExplicit) {
    for (const uint32_t channels : {18U, 28U}) {
        AmdtpPacketTimeline timeline{};
        std::array<PacketTimelineSlot, 8> timelineSlots{};
        ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));
        AmdtpStreamConfig config{};
        config.streamMode = StreamMode::Blocking;
        config.packetFraming = AmdtpStreamConfig::PacketFraming::Headerless;
        config.pcmChannels = channels;
        config.dbs = static_cast<uint8_t>(channels);
        config.framesPerDataPacket = 8;
        config.maxPacketBytes = channels == 18 ? 576 : 896;
        AmdtpTxPolicy policy{};
        policy.hostToDevicePcmEncoding = PcmSlotEncoding::RawPcm24Upper24In32LE;
        AmdtpTxPacketizer packetizer{};
        packetizer.BindTimeline(&timeline);
        ASSERT_TRUE(packetizer.Configure(config, policy));
        std::array<uint8_t, 896> bytes{};
        PreparedTxPacket packet{};
        uint64_t frame = 0;
        AmdtpTimingState data{};
        data.txClockValid = true;
        data.disposition = AmdtpPacketDisposition::Data;
        data.nextDataSyt = 0x1234;
        data.replayValid = true;
        data.replayDataBlocks = 8;
        ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(
            packetizer, {0, bytes.data(), config.maxPacketBytes}, data, frame, packet));
        EXPECT_EQ(packet.byteCount, config.maxPacketBytes);
        EXPECT_EQ(packet.isochTag, 0);
        EXPECT_EQ(packet.isochSync, 0);
        EXPECT_EQ(packet.syt, 0xFFFFU);
        EXPECT_EQ(packet.operation, PreparedTxPacket::Operation::Packet);

        AmdtpTimingState idle{};
        idle.txClockValid = true;
        idle.disposition = AmdtpPacketDisposition::NoData;
        idle.replayValid = true;
        ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(
            packetizer, {1, bytes.data(), config.maxPacketBytes}, idle, frame, packet));
        EXPECT_EQ(packet.byteCount, 0U);
        EXPECT_EQ(packet.operation, PreparedTxPacket::Operation::SkipCycle);
    }
}

TEST(AmdtpDirectTxTests, HeaderlessPayloadWriterStartsAtByteZeroAndPreservesChannelOrder) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));
    AmdtpStreamConfig config{};
    config.streamMode = StreamMode::Blocking;
    config.packetFraming = AmdtpStreamConfig::PacketFraming::Headerless;
    config.pcmChannels = 2;
    config.dbs = 2;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 64;
    AmdtpTxPolicy policy{};
    policy.hostToDevicePcmEncoding = PcmSlotEncoding::RawPcm24Upper24In32LE;
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, policy));
    std::array<uint8_t, 64> packetBytes{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;
    uint64_t nextFrame = 0;
    PreparedTxPacket packet{};
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(
        packetizer, {0, packetBytes.data(), packetBytes.size()}, timing, nextFrame, packet));

    AmdtpPayloadWriter writer{};
    writer.Configure(config, policy);
    writer.BindTimeline(&timeline);
    std::array<float, 16> host{};
    host[0] = 0.5f;
    host[1] = -0.5f;
    writer.WriteFloat32Interleaved({host.data(), 0, 8, 8, 2}, 0);
    EXPECT_EQ((std::array<uint8_t, 4>{packetBytes[0], packetBytes[1], packetBytes[2], packetBytes[3]}),
              (std::array<uint8_t, 4>{0x00, 0x00, 0x00, 0x40}));
    EXPECT_EQ((std::array<uint8_t, 4>{packetBytes[4], packetBytes[5], packetBytes[6], packetBytes[7]}),
              (std::array<uint8_t, 4>{0x00, 0x00, 0x00, 0xC0}));
}

TEST(AmdtpDirectTxTests, ForcedNoDataHoldsDbcAndAudioFrame) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(
        timelineSlots.data(), timelineSlots.size()));

    AmdtpTxPacketizer packetizer{};

    uint64_t packetizerFrame = 0;
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
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {0, bytes[0].data(), bytes[0].size()}, allowData, packetizerFrame, first));
    EXPECT_FALSE(first.isData);
    EXPECT_EQ(first.byteCount, 8U);
    EXPECT_EQ(bytes[0][4], 0x90);
    EXPECT_EQ(bytes[0][5], 0xFF);
    EXPECT_EQ(bytes[0][6], 0xFF);
    EXPECT_EQ(bytes[0][7], 0xFF);

    AmdtpTimingState noData{};
    noData.disposition = AmdtpPacketDisposition::NoData;
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {1, bytes[1].data(), bytes[1].size()}, noData, packetizerFrame, forced));
    EXPECT_FALSE(forced.isData);
    EXPECT_EQ(forced.byteCount, 8U);
    EXPECT_EQ(forced.operation, PreparedTxPacket::Operation::Packet);
    EXPECT_EQ(forced.dbc, first.dbc);
    EXPECT_EQ(forced.firstAudioFrame, 0U);

    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {2, bytes[2].data(), bytes[2].size()}, allowData, packetizerFrame, data));
    EXPECT_TRUE(data.isData);
    EXPECT_EQ(data.dbc, first.dbc);
    EXPECT_EQ(data.firstAudioFrame, 0U);
    EXPECT_EQ(data.framesInPacket, 8U);
    EXPECT_EQ(data.syt, 0x1234U);
}

TEST(AmdtpDirectTxTests, EmptyCipPacketRemainsPacketOperation) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    auto config = BlockingStereoConfig();
    AmdtpTxPolicy policy{};
    policy.emptyPacketsDuringIdle = true;
    ASSERT_TRUE(packetizer.Configure(config, policy));
    std::array<uint8_t, 128> bytes{};
    AmdtpTimingState idle{};
    idle.disposition = AmdtpPacketDisposition::NoData;
    uint64_t frame = 0;
    PreparedTxPacket packet{};
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(
        packetizer, {0, bytes.data(), bytes.size()}, idle, frame, packet));
    EXPECT_FALSE(packet.isData);
    EXPECT_EQ(packet.byteCount, 0U);
    EXPECT_EQ(packet.operation, PreparedTxPacket::Operation::Packet);
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

// The TX producer commits the M-Audio cadence with the timeline's DATA verdict.
// Packet size cannot answer that question: cadence NO-DATA is full-size too.
// Using `byteCount > 8` killed IT at the first steady-state NO-DATA slot.
TEST(AmdtpDirectTxTests, MAudioCadenceNoDataIsFullSizeButTimelineSaysNoData) {
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

    std::array<std::array<uint8_t, 232>, 2> bytes{};
    TxPresentationPlan plan{};
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;
    PreparedTxPacket data{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, {}, plan, data));

    plan.cycleOrdinal = 1;
    plan.frameCount = 0;
    plan.disposition = AmdtpPacketDisposition::NoData;
    PreparedTxPacket noData{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, {}, plan, noData));

    EXPECT_EQ(data.byteCount, 232U);
    EXPECT_EQ(noData.byteCount, 232U);
    const auto* dataSlot = timeline.SlotByIndex(0);
    const auto* noDataSlot = timeline.SlotByIndex(1);
    ASSERT_NE(dataSlot, nullptr);
    ASSERT_NE(noDataSlot, nullptr);
    EXPECT_TRUE(dataSlot->isData);
    EXPECT_FALSE(noDataSlot->isData);
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
    EXPECT_EQ(timeline.ExposedFrameEnd(), 8U);
    packetizer.RevertToNoData(slot, packet);
    // The reverted frames belong to the next DATA packet: the fill must wait
    // for it instead of treating them as lost.
    EXPECT_EQ(timeline.ExposedFrameEnd(), 0U);
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

TEST(AmdtpDirectTxTests, DevicePlaybackMapMovesPhase88ChannelsToPlanarSlots) {
    // The Phase 88 playback block is planar (channel positions captured from
    // the device, 2026-09-27): Out 1-8 are channels 0-7, SPDIF L/R channels 8-9.
    constexpr std::array<uint8_t, 10> kPlanarSlots{1, 6, 2, 7, 3, 8, 4, 9, 0, 5};
    ASFW::Audio::Wire::PcmSlotMap deviceMap{};
    ASSERT_TRUE(deviceMap.SetSlots(kPlanarSlots));

    // The shape comes from discovery: 10 PCM + 1 MIDI, as the device publishes
    // it, framed the way StartIO frames it.
    ASFW::Isoch::Audio::AVC::Profiles::GenericAvcProfile profile{};
    const ASFW::Isoch::Audio::ParsedWireStream published{
        .pcmChannels = 10, .am824Slots = 11, .midiPorts = 1};
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    ASSERT_TRUE(ASFW::Isoch::Audio::BuildResolvedTxStreamConfig(profile, &published, 1, 0, config));
    ASSERT_EQ(config.pcmChannels, 10U);
    ASSERT_EQ(config.midiSlots, 1U);
    ASSERT_EQ(config.dbs, 11U);

    constexpr uint32_t kFrames = 8;
    std::array<float, kFrames * 10> host{};
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        for (uint32_t channel = 0; channel < 10; ++channel) {
            host[frame * 10 + channel] = static_cast<float>(channel + 1) / 16.0f;
        }
    }
    const HostAudioBufferView hostView{
        .interleavedFloat32 = host.data(),
        .firstFrame = 0,
        .frameCount = kFrames,
        .frameCapacity = kFrames,
        .channels = 10,
    };
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x1234;
    timing.replayValid = true;
    timing.replayDataBlocks = kFrames;

    const auto firstBlockSlots = [&](const ASFW::Audio::Wire::PcmSlotMap& map) {
        DiceTxStreamEngine engine{};
        auto mapped = config;
        mapped.pcmSlotMap = map;
        mapped.hasPcmSlotMap = !map.IsIdentity();
        EXPECT_TRUE(engine.Configure(profile, mapped));
        std::array<uint8_t, 512> bytes{};
        struct Provider final : IAmdtpTxSlotProvider {
            std::array<uint8_t, 512>* bytes{nullptr};
            bool AcquireWritableSlot(uint64_t packetIndex, TxPacketSlotView& outSlot) noexcept override {
                outSlot = {packetIndex, bytes->data(), static_cast<uint32_t>(bytes->size())};
                return true;
            }
            bool PublishSlot(const PreparedTxPacket&) noexcept override { return true; }
            uint32_t SlotCount() const noexcept override { return 1; }
        } provider{};
        provider.bytes = &bytes;
        engine.BindSlotProvider(&provider);
        engine.ResetForStart(0, 0);
        EXPECT_EQ(engine.PrepareNextTransmitSlot(0, timing), TxSlotPrepareResult::kPrepared);
        engine.FillFromHostOutput(hostView, 0);
        std::array<uint32_t, 11> slots{};
        for (uint32_t slot = 0; slot < slots.size(); ++slot) {
            const uint8_t* q = bytes.data() + 8 + slot * 4;
            slots[slot] = (uint32_t{q[0]} << 24) | (uint32_t{q[1]} << 16) | (uint32_t{q[2]} << 8) | q[3];
        }
        return slots;
    };

    const auto interleaved = firstBlockSlots({});
    const auto planar = firstBlockSlots(deviceMap);
    for (uint32_t channel = 0; channel < 10; ++channel) {
        ASSERT_NE(interleaved[channel], 0U) << "channel " << channel << " carried no sample";
        EXPECT_EQ(planar[kPlanarSlots[channel]], interleaved[channel]) << "channel " << channel;
    }
    EXPECT_EQ(planar[10], interleaved[10]);  // MIDI slot is not PCM; untouched
}

TEST(AmdtpDirectTxTests, Captured1814Default48kGeometryMatchesProfile) {
    // tools/pydice/1814-default-geom-48k.txt has six 232-byte host packets,
    // four 360-byte device packets, and two 8-byte device CIP-only packets.
    // tools/pydice/tests/test_1814_default_geometry.py verifies those facts
    // against the raw FireBug capture.
    ASFW::Isoch::Audio::AVC::Profiles::MAudioSpecialProfile profile(false);
    ASFW::Isoch::Audio::AudioStreamConfig tx{};
    ASFW::Isoch::Audio::AudioStreamConfig rx{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(tx));
    ASSERT_TRUE(profile.BuildDefaultRxStreamConfig(rx));
    EXPECT_EQ(tx.sampleRate, 48000U);
    EXPECT_EQ(rx.sampleRate, 48000U);
    EXPECT_EQ(tx.dbs, 7U);
    EXPECT_EQ(rx.dbs, 11U);
    EXPECT_EQ(tx.pcmChannels + tx.midiSlots, tx.dbs);
    EXPECT_EQ(rx.pcmChannels + rx.midiSlots, rx.dbs);
    EXPECT_EQ(tx.framesPerDataPacket, 8U);
    EXPECT_EQ(rx.framesPerDataPacket, 8U);
    EXPECT_EQ(8U + tx.framesPerDataPacket * tx.dbs * 4U, 232U);
    EXPECT_EQ(8U + rx.framesPerDataPacket * rx.dbs * 4U, 360U);
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

    uint64_t startFrame = 0;
    AmdtpTxPacketizer end{};
    uint64_t endFrame = 0;
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
        ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(start,{i, bytesStart.data(), bytesStart.size()},
                                            allowData, startFrame, a));
        ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(end,{i, bytesEnd.data(), bytesEnd.size()},
                                          allowData, endFrame, b));
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

    uint64_t packetizerFrame = 0;
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(BlockingStereoConfig(), policy));

    std::array<uint8_t, 128> bytes{};
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::NoData;
    PreparedTxPacket packet{};
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {0, bytes.data(), bytes.size()}, timing, packetizerFrame, packet));

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

    uint64_t packetizerFrame = 0;
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
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {0, bytes[0].data(), bytes[0].size()}, data, packetizerFrame, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.framesInPacket, 8U);
    EXPECT_EQ(packet.syt, 0x2345U);

    AmdtpTimingState noData{};
    noData.disposition = AmdtpPacketDisposition::NoData;
    noData.replayValid = true;
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {1, bytes[1].data(), bytes[1].size()}, noData, packetizerFrame, packet));
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

    uint64_t packetizerFrame = 0;
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
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {0, noDataBytes.data(), noDataBytes.size()}, timing, packetizerFrame, packet));
    ASSERT_FALSE(packet.isData);
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(packetizer,
        {1, dataBytes.data(), dataBytes.size()}, timing, packetizerFrame, packet));
    ASSERT_TRUE(packet.isData);

    AmdtpPayloadWriter writer{};
    writer.Configure(config, AmdtpTxPolicy{});
    writer.BindTimeline(&timeline);
    std::array<float, 16> mappedRing{};
    mappedRing[0] = 1.0f;
    mappedRing[1] = -1.0f;
    // firstWritablePacket 0: every packet is writable here.
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

// T3 (documentation/TX_OWNERSHIP.md): a planned DATA packet is armed with valid
// AM824 silence -- 0x40000000 in every PCM slot, as Linux amdtp-am824.c:209-220
// writes when it has no PCM -- so a packet whose PCM never arrives is still
// valid on the wire. Filling it later changes only the PCM sample words.
TEST(AmdtpDirectTxTests, ArmedDataPacketCarriesValidSilenceAndFillChangesOnlySamples) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));

    AmdtpStreamConfig config = BlockingStereoConfig();
    config.dbs = 3;         // two PCM slots and one MIDI slot
    config.midiSlots = 1;
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, AmdtpTxPolicy{}));

    std::array<uint8_t, 128> bytes{};
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    TxPresentationPlan plan{};
    plan.firstAudioFrame = 0;
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket({0, bytes.data(), bytes.size()}, timing, plan, packet));
    ASSERT_TRUE(packet.isData);

    const auto quadlet = [](const std::array<uint8_t, 128>& b, size_t offset) {
        return (uint32_t{b[offset]} << 24) | (uint32_t{b[offset + 1]} << 16) |
               (uint32_t{b[offset + 2]} << 8) | uint32_t{b[offset + 3]};
    };
    const auto slotOffset = [&](uint32_t frame, uint32_t slot) {
        return 8 + (static_cast<size_t>(frame) * config.dbs + slot) * 4;
    };
    for (uint32_t frame = 0; frame < 8; ++frame) {
        EXPECT_EQ(quadlet(bytes, slotOffset(frame, 0)), 0x40000000U) << "frame " << frame;
        EXPECT_EQ(quadlet(bytes, slotOffset(frame, 1)), 0x40000000U) << "frame " << frame;
        EXPECT_EQ(quadlet(bytes, slotOffset(frame, 2)), 0x80000000U) << "frame " << frame;
    }
    const auto armed = bytes;

    AmdtpPayloadWriter writer{};
    writer.Configure(config, AmdtpTxPolicy{});
    writer.BindTimeline(&timeline);
    std::array<float, 16> ring{};
    for (uint32_t i = 0; i < ring.size(); ++i) {
        ring[i] = 0.25f + 0.01f * static_cast<float>(i);
    }
    writer.WriteFloat32Interleaved({ring.data(), 0, 8, 8, 2}, 0);

    for (size_t offset = 0; offset < 8; ++offset) {
        EXPECT_EQ(bytes[offset], armed[offset]) << "CIP header byte " << offset;
    }
    for (uint32_t frame = 0; frame < 8; ++frame) {
        EXPECT_EQ(quadlet(bytes, slotOffset(frame, 2)), quadlet(armed, slotOffset(frame, 2)));
        for (uint32_t slot = 0; slot < 2; ++slot) {
            const uint32_t filled = quadlet(bytes, slotOffset(frame, slot));
            EXPECT_EQ(filled >> 24, 0x40U) << "frame " << frame << " slot " << slot;
            EXPECT_NE(filled, quadlet(armed, slotOffset(frame, slot)));
        }
    }
}

TEST(AmdtpDirectTxTests, PayloadWriterCountsFramesWithoutPacket) {
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
        counters.framesWithoutPacket.load(std::memory_order_relaxed), 8U);
    EXPECT_EQ(counters.framesWritten.load(std::memory_order_relaxed), 0U);
}

// The engine owns the one TX audio-frame cursor. Alignment is accepted once
// per reset or re-arm, and each change starts a new cursor epoch.
TEST(AmdtpDirectTxTests, EngineAlignsItsFrameCursorOncePerResetOrReArm) {
    DiceTxStreamEngine engine{};
    engine.ResetForStart(0, 0);
    const uint64_t startEpoch = engine.CursorEpoch();
    EXPECT_FALSE(engine.IsFrameCursorAligned());
    EXPECT_TRUE(engine.AlignFrameCursorOnce(960U));
    EXPECT_TRUE(engine.IsFrameCursorAligned());
    EXPECT_FALSE(engine.AlignFrameCursorOnce(2000U));
    EXPECT_GT(engine.CursorEpoch(), startEpoch);

    const uint64_t alignedEpoch = engine.CursorEpoch();
    engine.ReArmFrameCursorAlignment();
    EXPECT_FALSE(engine.IsFrameCursorAligned());
    EXPECT_GT(engine.CursorEpoch(), alignedEpoch);
    EXPECT_TRUE(engine.AlignFrameCursorOnce(3000U));

    engine.ResetForStart(0, 0);
    EXPECT_TRUE(engine.AlignFrameCursorOnce(4000U));
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
    plan.cycleOrdinal = 0;
    plan.firstAudioFrame = 500;
    plan.frameCount = 8;
    plan.disposition = AmdtpPacketDisposition::Data;

    PreparedTxPacket packet{};
    // Too small a slot fails without advancing DBC or cadence.
    EXPECT_FALSE(packetizer.PrepareNextPacket(
        {0, bytes.data(), 4}, timing, plan, packet));

    // Retry with a corrected slot succeeds with the same DBC.
    EXPECT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, plan, packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_EQ(packet.firstAudioFrame, 500U);
    EXPECT_EQ(packet.dbc, 0U);
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

TEST(AmdtpDirectTxTests, RevertedEndEventPacketRestoresDbc) {
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
    packetizer.Reset(250);
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
    slot.packetIndex = 1;
    ASSERT_TRUE(packetizer.PrepareNextPacket(slot, timing, plan, packet));
    EXPECT_EQ(packet.dbc, 2U);
    EXPECT_EQ(packet.firstAudioFrame, 100U);
}

namespace {

class LargeCaptureTxSlotProvider final : public IAmdtpTxSlotProvider {
public:
    std::array<uint8_t, 512> bytes{};
    PreparedTxPacket published{};

    bool AcquireWritableSlot(uint64_t packetIndex,
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

uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

// The Mk3 profile's CIP fields drive the generic CIP builder to the
// captured V3 header -- SPH set, FMT 0x02, FDF 0x22 kept against the 48 kHz AM824 SFC,
// SYT NO_INFO -- in both packet kinds. MotuV3Wire::BuildCipQ0/Q1 is the reference.
TEST(AmdtpDirectTxTests, Mk3ProfileCipHeaderIsTheCapturedV3HeaderThroughTheGenericBuilder) {
    ASFW::Isoch::Audio::MOTU::Profiles::MOTU828Mk3Profile profile;
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(config));
    ASSERT_TRUE(config.cipSph);
    ASSERT_TRUE(config.fdfIsFixed);
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(engine.Configure(profile, config));
    LargeCaptureTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    engine.ResetForStart(0, 0);

    AmdtpTimingState timing{};
    timing.replayValid = true;
    timing.disposition = AmdtpPacketDisposition::NoData;
    ASSERT_EQ(engine.PrepareNextTransmitSlot(0, timing), TxSlotPrepareResult::kPrepared);
    EXPECT_FALSE(provider.published.isData);
    EXPECT_EQ(provider.published.byteCount, 8U);
    EXPECT_EQ(ReadBE32(provider.bytes.data()),
              MotuV3Wire::BuildCipQ0(config.sid, config.dbs, 0));
    EXPECT_EQ(ReadBE32(provider.bytes.data() + 4),
              MotuV3Wire::BuildCipQ1(config.fmt, config.fdf));

    // DATA as the SYT-unaware producer path builds it: a replayed block count and
    // SYT NO_INFO (V3 capture never carries a valid SYT to replay).
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayDataBlocks = config.framesPerDataPacket;
    timing.txClockValid = true;
    timing.nextDataSyt = 0xFFFF;
    ASSERT_EQ(engine.PrepareNextTransmitSlot(1, timing), TxSlotPrepareResult::kPrepared);
    ASSERT_TRUE(provider.published.isData);
    EXPECT_EQ(provider.published.byteCount,
              8U + config.framesPerDataPacket * config.dbs * 4U);
    EXPECT_EQ(ReadBE32(provider.bytes.data()),
              MotuV3Wire::BuildCipQ0(config.sid, config.dbs, 0));
    EXPECT_EQ(ReadBE32(provider.bytes.data() + 4),
              MotuV3Wire::BuildCipQ1(config.fmt, config.fdf));
    // The captured DATA header literally: SID 0, DBS 13, the 0x04 FN/QPC/SPH octet,
    // DBC 0; then FMT 0x02 (0x90 here would be AM824), FDF 0x22 rather than the 48 kHz
    // SFC 0x02, SYT NO_INFO.
    const std::array<uint8_t, 8> captured{0x00, 0x0d, 0x04, 0x00, 0x82, 0x22, 0xff, 0xff};
    EXPECT_TRUE(std::equal(captured.begin(), captured.end(), provider.bytes.begin()));
}

// The new fields default off, so every other profile's header is unchanged: AM824
// keeps SPH clear and re-derives FDF from the rate.
TEST(AmdtpDirectTxTests, CipSphAndFixedFdfDefaultOffForEveryOtherStream) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> timelineSlots{};
    ASSERT_TRUE(timeline.AttachSlots(timelineSlots.data(), timelineSlots.size()));
    AmdtpStreamConfig config = BlockingStereoConfig();
    config.fdf = 0x22;  // a profile value the rate must override when not fixed
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, AmdtpTxPolicy{}));
    std::array<uint8_t, 128> bytes{};
    TxPresentationPlan plan{};
    plan.disposition = AmdtpPacketDisposition::Data;
    plan.frameCount = 8;
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket({0, bytes.data(), bytes.size()}, {}, plan, packet));
    EXPECT_EQ(bytes[2] & 0x04, 0);
    EXPECT_EQ(bytes[5], 0x02);
}
