#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Runtime/TxContentRecoveryPolicy.hpp"
#include "Audio/Runtime/TxPcmStagingRing.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <vector>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Audio::Ports::TxPcmReadRequest;
using ASFW::Audio::Ports::TxPcmReadResult;
using ASFW::Audio::Runtime::TxPcmHostRingView;
using ASFW::Audio::Runtime::TxPcmStagingRing;
using ASFW::Audio::Runtime::TxContentShortageAction;
using ASFW::Audio::Runtime::TxContentShortageKind;
using ASFW::Encoding::AudioWireFormat;
using ASFW::Isoch::Audio::AudioStreamConfig;
using ASFW::Isoch::Audio::AudioStreamDirection;
using ASFW::Isoch::Audio::IAudioStreamProfile;
using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;

class TestAudioStreamProfile final : public IAudioStreamProfile {
public:
    uint8_t pcmChannels{2};
    uint8_t dbs{2};
    uint8_t sourceChannelOffset{0};

    const char* Name() const noexcept override { return "TX ownership test"; }
    AudioWireFormat TxWireFormat() const noexcept override {
        return AudioWireFormat::kAM824;
    }
    AudioWireFormat RxWireFormat() const noexcept override {
        return AudioWireFormat::kAM824;
    }
    bool BuildDefaultTxStreamConfig(
        AudioStreamConfig& out) const noexcept override {
        out = {};
        out.direction = AudioStreamDirection::HostToDevice;
        out.pcmChannels = pcmChannels;
        out.dbs = dbs;
        out.sourceChannelOffset = sourceChannelOffset;
        return true;
    }
    bool BuildDefaultRxStreamConfig(
        AudioStreamConfig& out) const noexcept override {
        out = {};
        out.direction = AudioStreamDirection::DeviceToHost;
        out.pcmChannels = pcmChannels;
        out.dbs = dbs;
        return true;
    }
    uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
    uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }
};

class TestTxSlotProvider final : public IAmdtpTxSlotProvider {
public:
    bool allowAcquire{true};
    bool allowPublish{true};
    std::array<uint8_t, 256> bytes{};
    std::array<uint8_t, 256> publishedBytes{};
    PreparedTxPacket publishedPacket{};
    uint32_t publishCount{0};
    uint32_t unfinalizedDataAttempts{0};

    bool AcquireWritableSlot(uint32_t packetIndex,
                             TxPacketSlotView& out) noexcept override {
        if (!allowAcquire) return false;
        out = {packetIndex, bytes.data(),
               static_cast<uint32_t>(bytes.size())};
        return true;
    }

    bool PublishSlot(const PreparedTxPacket& packet) noexcept override {
        if (!allowPublish || packet.byteCount > bytes.size()) return false;
        if (packet.isData && !packet.pcmFinalized) {
            ++unfinalizedDataAttempts;
            return false;
        }
        publishedPacket = packet;
        publishedBytes.fill(0);
        std::copy_n(bytes.begin(), packet.byteCount,
                    publishedBytes.begin());
        ++publishCount;
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

AmdtpTimingState DataTiming(uint16_t syt = 0x1234) {
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = syt;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;
    return timing;
}

TxPcmSnapshotView StereoSnapshot(std::array<float, 16>& pcm) {
    return {pcm.data(), 8, 2};
}

bool ConfigureEngine(DiceTxStreamEngine& engine,
                     const TestAudioStreamProfile& profile) {
    AudioStreamConfig config{};
    return profile.BuildDefaultTxStreamConfig(config) &&
           engine.Configure(profile, config);
}

uint32_t ReadBE32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
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

TEST(AmdtpDirectTxTests, DataRequiresCompletePcmBeforeStateAdvances) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(
        BlockingStereoConfig(), AmdtpTxPolicy{}));

    std::array<uint8_t, 128> bytes{};
    PreparedTxPacket packet{};
    const auto timing = DataTiming();
    EXPECT_FALSE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing, packet));
    EXPECT_EQ(packetizer.TelemetrySnapshot().nextAudioFrame, 0U);
    EXPECT_EQ(timeline.FinalizedFrameEnd(), 0U);

    std::array<float, 16> pcm{};
    pcm[0] = 1.0f;
    pcm[1] = -1.0f;
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes.data(), bytes.size()}, timing,
        StereoSnapshot(pcm), packet));
    EXPECT_TRUE(packet.isData);
    EXPECT_TRUE(packet.pcmFinalized);
    EXPECT_EQ(packet.firstAudioFrame, 0U);
    EXPECT_EQ(packetizer.TelemetrySnapshot().nextAudioFrame, 8U);
    EXPECT_EQ(timeline.FinalizedFrameEnd(), 8U);
    EXPECT_EQ(ReadBE32(bytes.data() + 8), 0x407FFFFFu);
    EXPECT_EQ(ReadBE32(bytes.data() + 12), 0x40800001u);
}

TEST(AmdtpDirectTxTests, ForcedNoDataHoldsDbcAndAudioFrame) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(
        BlockingStereoConfig(), AmdtpTxPolicy{}));

    std::array<std::array<uint8_t, 128>, 3> bytes{};
    std::array<float, 16> pcm{};
    PreparedTxPacket first{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, DataTiming(),
        StereoSnapshot(pcm), first));
    ASSERT_TRUE(first.isData);

    AmdtpTimingState noData{};
    noData.replayValid = true;
    noData.disposition = AmdtpPacketDisposition::NoData;
    PreparedTxPacket forced{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, noData, forced));
    EXPECT_FALSE(forced.isData);
    EXPECT_EQ(forced.dbc, 8U);
    EXPECT_EQ(packetizer.TelemetrySnapshot().nextAudioFrame, 8U);

    PreparedTxPacket second{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {2, bytes[2].data(), bytes[2].size()}, DataTiming(),
        StereoSnapshot(pcm), second));
    EXPECT_EQ(second.dbc, 8U);
    EXPECT_EQ(second.firstAudioFrame, 8U);
}

TEST(AmdtpDirectTxTests,
     ExplicitPacketScheduleCanAdvanceDbcOnNoDataForMAudioSpecialFirmware) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPolicy policy{};
    policy.noDataDbcPolicy = NoDataDbcPolicy::AdvanceBySytInterval;
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(BlockingStereoConfig(), policy));

    std::array<std::array<uint8_t, 128>, 4> bytes{};
    std::array<float, 16> pcm{};

    AmdtpTimingState noData{};
    noData.disposition = AmdtpPacketDisposition::NoData;
    noData.hasExplicitPacketSchedule = true;
    PreparedTxPacket firstNoData{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, noData, firstNoData));
    EXPECT_FALSE(firstNoData.isData);
    EXPECT_EQ(firstNoData.dbc, 0U);

    AmdtpTimingState data{};
    data.txClockValid = true;
    data.disposition = AmdtpPacketDisposition::Data;
    data.nextDataSyt = 0x1234;
    data.hasExplicitPacketSchedule = true;
    data.explicitDataBlocks = 8;
    PreparedTxPacket firstData{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, data,
        StereoSnapshot(pcm), firstData));
    ASSERT_TRUE(firstData.isData);
    EXPECT_EQ(firstData.dbc, 8U);
    EXPECT_EQ(firstData.syt, 0x1234U);

    PreparedTxPacket secondNoData{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {2, bytes[2].data(), bytes[2].size()}, noData, secondNoData));
    EXPECT_FALSE(secondNoData.isData);
    EXPECT_EQ(secondNoData.dbc, 16U);

    PreparedTxPacket secondData{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {3, bytes[3].data(), bytes[3].size()}, data,
        StereoSnapshot(pcm), secondData));
    EXPECT_TRUE(secondData.isData);
    EXPECT_EQ(secondData.dbc, 24U);
}

TEST(AmdtpDirectTxTests, NoDataFdfCanUseCompatibilityQuirk) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
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
    EXPECT_EQ(bytes[4], 0x90);
    EXPECT_EQ(bytes[5], 0x02);
    EXPECT_EQ(bytes[6], 0xFF);
    EXPECT_EQ(bytes[7], 0xFF);
}

TEST(AmdtpDirectTxTests, MissingSourceCannotReachReleaseCommit) {
    TestAudioStreamProfile profile{};
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);

    EXPECT_EQ(engine.PrepareNextTransmitSlot(0, DataTiming()),
              TxSlotPrepareResult::kPcmSourceUnavailable);
    EXPECT_EQ(provider.publishCount, 0U);
    EXPECT_EQ(provider.unfinalizedDataAttempts, 0U);
    EXPECT_EQ(engine.PacketizerTelemetrySnapshot().nextAudioFrame, 0U);
}

TEST(AmdtpDirectTxTests, HostWriteBeforePacketIsBackfilledAtCommit) {
    TestAudioStreamProfile profile{};
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);

    TxPcmStagingRing staging{};
    ASSERT_TRUE(staging.Configure(2, 64));
    engine.BindPcmSource(&staging);

    std::array<float, 16> host{};
    for (size_t index = 0; index < host.size(); ++index) {
        host[index] = (index & 1U) == 0 ? 0.5f : -0.5f;
    }
    ASSERT_EQ(staging.Stage({host.data(), 0, 8, 8, 2}),
              ASFW::Audio::Runtime::TxPcmStageResult::kStaged);

    ASSERT_EQ(engine.PrepareNextTransmitSlot(0, DataTiming()),
              TxSlotPrepareResult::kPrepared);
    ASSERT_EQ(provider.publishCount, 1U);
    ASSERT_TRUE(provider.publishedPacket.isData);
    ASSERT_TRUE(provider.publishedPacket.pcmFinalized);
    EXPECT_EQ(ReadBE32(provider.publishedBytes.data() + 8), 0x40400000u);
    EXPECT_EQ(ReadBE32(provider.publishedBytes.data() + 12), 0x40C00000u);
}

TEST(AmdtpDirectTxTests, PacketWaitsForPcmAndRetriesSameFrame) {
    TestAudioStreamProfile profile{};
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    TxPcmStagingRing staging{};
    ASSERT_TRUE(staging.Configure(2, 64));
    engine.BindPcmSource(&staging);

    EXPECT_EQ(engine.PrepareNextTransmitSlot(91, DataTiming()),
              TxSlotPrepareResult::kPcmNotYetWritten);
    EXPECT_EQ(provider.publishCount, 0U);
    EXPECT_EQ(engine.PacketizerTelemetrySnapshot().nextAudioFrame, 0U);

    std::array<float, 16> host{};
    host.fill(0.25f);
    ASSERT_EQ(staging.Stage({host.data(), 0, 8, 8, 2}),
              ASFW::Audio::Runtime::TxPcmStageResult::kStaged);
    EXPECT_EQ(engine.PrepareNextTransmitSlot(91, DataTiming()),
              TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(provider.publishedPacket.firstAudioFrame, 0U);
    EXPECT_EQ(engine.PacketizerTelemetrySnapshot().nextAudioFrame, 8U);
}

TEST(AmdtpDirectTxTests,
     HardFutureShortageHoldsCursorUntilMissingSuffixIsPublished) {
    using ASFW::Audio::Runtime::DecideTxContentShortageAction;

    TestAudioStreamProfile profile{};
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    TxPcmStagingRing staging{};
    ASSERT_TRUE(staging.Configure(2, 64));
    engine.BindPcmSource(&staging);
    ASSERT_TRUE(engine.AlignFrameCursorOnce(960));

    // Model the live failure exactly: DATA needs [960, 968), while WriteEnd
    // has published only [940, 964), leaving four of eight frames available.
    std::array<float, 48> prefix{};
    prefix.fill(0.25f);
    ASSERT_EQ(staging.Stage({prefix.data(), 940, 24, 24, 2}),
              ASFW::Audio::Runtime::TxPcmStageResult::kStaged);
    ASSERT_EQ(engine.PrepareNextTransmitSlot(37, DataTiming()),
              TxSlotPrepareResult::kPcmNotYetWritten);
    ASSERT_EQ(
        DecideTxContentShortageAction(
            TxContentShortageKind::kNotYetWritten, true),
        TxContentShortageAction::kEmitNoDataHoldCursor);

    AmdtpTimingState noData{};
    noData.replayValid = true;
    noData.disposition = AmdtpPacketDisposition::NoData;
    ASSERT_EQ(engine.PrepareNextTransmitSlot(37, noData),
              TxSlotPrepareResult::kPrepared);
    EXPECT_FALSE(provider.publishedPacket.isData);
    EXPECT_TRUE(engine.IsFrameCursorAligned());
    EXPECT_EQ(engine.PacketizerTelemetrySnapshot().nextAudioFrame, 960U);
    EXPECT_FALSE(engine.AlignFrameCursorOnce(4'096));

    std::array<float, 16> suffix{};
    suffix.fill(-0.25f);
    ASSERT_EQ(staging.Stage({suffix.data(), 964, 8, 8, 2}),
              ASFW::Audio::Runtime::TxPcmStageResult::kStaged);
    ASSERT_EQ(engine.PrepareNextTransmitSlot(38, DataTiming()),
              TxSlotPrepareResult::kPrepared);
    EXPECT_TRUE(provider.publishedPacket.isData);
    EXPECT_EQ(provider.publishedPacket.firstAudioFrame, 960U);
    EXPECT_EQ(engine.PacketizerTelemetrySnapshot().nextAudioFrame, 968U);
}

TEST(AmdtpDirectTxTests, ContentShortagePolicyRebasesOnlyStalePcm) {
    using ASFW::Audio::Runtime::DecideTxContentShortageAction;

    EXPECT_EQ(DecideTxContentShortageAction(
                  TxContentShortageKind::kNotYetWritten, false),
              TxContentShortageAction::kDefer);
    EXPECT_EQ(DecideTxContentShortageAction(
                  TxContentShortageKind::kSnapshotBusy, false),
              TxContentShortageAction::kDefer);
    EXPECT_EQ(DecideTxContentShortageAction(
                  TxContentShortageKind::kNotYetWritten, true),
              TxContentShortageAction::kEmitNoDataHoldCursor);
    EXPECT_EQ(DecideTxContentShortageAction(
                  TxContentShortageKind::kSnapshotBusy, true),
              TxContentShortageAction::kEmitNoDataHoldCursor);
    EXPECT_EQ(DecideTxContentShortageAction(
                  TxContentShortageKind::kStaleOverwritten, false),
              TxContentShortageAction::kEmitNoDataRebaseCursor);
    EXPECT_EQ(DecideTxContentShortageAction(
                  TxContentShortageKind::kStaleOverwritten, true),
              TxContentShortageAction::kEmitNoDataRebaseCursor);
}

TEST(AmdtpDirectTxTests, AlignmentSelectsACompleteRetainedPacket) {
    using ASFW::Audio::Runtime::SelectCompletePcmPacket;

    // Exact geometry captured from the dirty hardware run: projected DATA
    // starts four frames below W, so that projected 8-frame block is partial.
    const auto live = SelectCompletePcmPacket(
        6'213'896, 6'197'516, 6'213'900, 8);
    ASSERT_TRUE(live.available);
    EXPECT_EQ(live.firstFrame, 6'213'888U);
    EXPECT_GE(live.firstFrame, 6'197'516U);
    EXPECT_LE(live.firstFrame + 8, 6'213'900U);

    const auto exact =
        SelectCompletePcmPacket(960, 900, 1'024, 8);
    ASSERT_TRUE(exact.available);
    EXPECT_EQ(exact.firstFrame, 960U);

    EXPECT_FALSE(
        SelectCompletePcmPacket(960, 960, 964, 8).available);
    EXPECT_FALSE(
        SelectCompletePcmPacket(960, 968, 960, 8).available);
    EXPECT_FALSE(
        SelectCompletePcmPacket(960, 0, 1'024, 0).available);
}

TEST(AmdtpDirectTxTests, PostCommitHostWritesCannotMutatePacketImage) {
    TestAudioStreamProfile profile{};
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    TxPcmStagingRing staging{};
    ASSERT_TRUE(staging.Configure(2, 64));
    engine.BindPcmSource(&staging);

    std::array<float, 16> host{};
    host.fill(0.5f);
    ASSERT_EQ(staging.Stage({host.data(), 0, 8, 8, 2}),
              ASFW::Audio::Runtime::TxPcmStageResult::kStaged);
    ASSERT_EQ(engine.PrepareNextTransmitSlot(0, DataTiming()),
              TxSlotPrepareResult::kPrepared);
    const auto packetAtCommit = provider.bytes;

    host.fill(-0.75f);
    EXPECT_EQ(staging.Stage({host.data(), 0, 8, 8, 2}),
              ASFW::Audio::Runtime::TxPcmStageResult::kDuplicate);
    EXPECT_EQ(provider.bytes, packetAtCommit);
    EXPECT_EQ(provider.publishedBytes, packetAtCommit);
}

TEST(AmdtpDirectTxTests, StaleCursorIsExplicitAndDoesNotPublish) {
    TestAudioStreamProfile profile{};
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    TxPcmStagingRing staging{};
    ASSERT_TRUE(staging.Configure(2, 16));
    engine.BindPcmSource(&staging);

    std::array<float, 32> host{};
    host.fill(0.5f);
    ASSERT_EQ(staging.Stage({host.data(), 100, 16, 16, 2}),
              ASFW::Audio::Runtime::TxPcmStageResult::kStaged);
    EXPECT_EQ(engine.PrepareNextTransmitSlot(0, DataTiming()),
              TxSlotPrepareResult::kPcmStaleOverwritten);
    EXPECT_EQ(provider.publishCount, 0U);
    EXPECT_EQ(engine.PacketizerTelemetrySnapshot().nextAudioFrame, 0U);
}

TEST(AmdtpDirectTxTests, StreamChannelOffsetIsAppliedBeforeCommit) {
    TestAudioStreamProfile profile{};
    profile.sourceChannelOffset = 2;
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);
    TxPcmStagingRing staging{};
    ASSERT_TRUE(staging.Configure(4, 16));
    engine.BindPcmSource(&staging);

    std::array<float, 32> host{};
    for (uint32_t frame = 0; frame < 8; ++frame) {
        host[frame * 4 + 0] = 0.1f;
        host[frame * 4 + 1] = 0.2f;
        host[frame * 4 + 2] = 0.5f;
        host[frame * 4 + 3] = -0.5f;
    }
    ASSERT_EQ(staging.Stage({host.data(), 0, 8, 8, 4}),
              ASFW::Audio::Runtime::TxPcmStageResult::kStaged);
    ASSERT_EQ(engine.PrepareNextTransmitSlot(0, DataTiming()),
              TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(ReadBE32(provider.publishedBytes.data() + 8), 0x40400000u);
    EXPECT_EQ(ReadBE32(provider.publishedBytes.data() + 12), 0x40C00000u);
}

TEST(AmdtpDirectTxTests, AlignFrameCursorIsAcceptedOnlyOncePerReset) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(
        BlockingStereoConfig(), AmdtpTxPolicy{}));
    EXPECT_TRUE(packetizer.AlignFrameCursorOnce(960));
    EXPECT_FALSE(packetizer.AlignFrameCursorOnce(2'000));
    packetizer.Reset(0, 0);
    EXPECT_TRUE(packetizer.AlignFrameCursorOnce(3'000));
}

TEST(AmdtpDirectTxTests,
     ExplicitRecoveryCanRebaseNextDataWithoutConsumingDbcOrOldPcm) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(
        BlockingStereoConfig(), AmdtpTxPolicy{}));
    ASSERT_TRUE(packetizer.AlignFrameCursorOnce(960));

    std::array<float, 16> pcm{};
    std::array<std::array<uint8_t, 128>, 3> bytes{};
    PreparedTxPacket first{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {0, bytes[0].data(), bytes[0].size()}, DataTiming(),
        StereoSnapshot(pcm), first));
    ASSERT_TRUE(first.isData);
    ASSERT_EQ(first.dbc, 0U);
    ASSERT_EQ(first.firstAudioFrame, 960U);

    AmdtpTimingState noData{};
    noData.replayValid = true;
    noData.disposition = AmdtpPacketDisposition::NoData;
    PreparedTxPacket missed{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {1, bytes[1].data(), bytes[1].size()}, noData, missed));
    EXPECT_FALSE(missed.isData);
    EXPECT_EQ(missed.dbc, 8U);
    EXPECT_EQ(packetizer.TelemetrySnapshot().nextAudioFrame, 968U);

    packetizer.ReArmFrameCursorAlignment();
    ASSERT_TRUE(packetizer.AlignFrameCursorOnce(4'096));
    PreparedTxPacket recovered{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {2, bytes[2].data(), bytes[2].size()}, DataTiming(),
        StereoSnapshot(pcm), recovered));
    EXPECT_TRUE(recovered.isData);
    EXPECT_EQ(recovered.dbc, 8U);
    EXPECT_EQ(recovered.firstAudioFrame, 4'096U);
    EXPECT_EQ(packetizer.TelemetrySnapshot().nextAudioFrame, 4'104U);
}

TEST(AmdtpDirectTxTests,
     PacketizerTelemetryTracksFinalizedRangeAndRearmEpoch) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(
        BlockingStereoConfig(), AmdtpTxPolicy{}));
    ASSERT_TRUE(packetizer.AlignFrameCursorOnce(960));

    std::array<float, 16> pcm{};
    std::array<uint8_t, 128> bytes{};
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareNextPacket(
        {37, bytes.data(), bytes.size()}, DataTiming(),
        StereoSnapshot(pcm), packet));

    const auto snapshot = packetizer.TelemetrySnapshot();
    EXPECT_TRUE(snapshot.frameCursorAligned);
    EXPECT_TRUE(snapshot.hasLastDataPacket);
    EXPECT_EQ(snapshot.nextAudioFrame, 968U);
    EXPECT_EQ(snapshot.lastDataFirstAudioFrame, 960U);
    EXPECT_EQ(snapshot.lastDataEndAudioFrame, 968U);
    EXPECT_EQ(snapshot.lastDataPacketIndex, 37U);
    EXPECT_EQ(timeline.FinalizedFrameEnd(), 968U);

    packetizer.ReArmFrameCursorAlignment();
    const auto rearmed = packetizer.TelemetrySnapshot();
    EXPECT_FALSE(rearmed.frameCursorAligned);
    EXPECT_GT(rearmed.cursorEpoch, snapshot.cursorEpoch);
}

TEST(AmdtpDirectTxTests, PacketizerRejectsPcmGeometryBeyondDbs) {
    auto config = BlockingStereoConfig();
    config.pcmChannels = 3;
    config.dbs = 2;
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 4> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    EXPECT_FALSE(packetizer.Configure(config, AmdtpTxPolicy{}));
}

TEST(AmdtpDirectTxTests, EngineReportsProviderAndPacketizerFailures) {
    DiceTxStreamEngine engine{};
    AmdtpTimingState timing{};
    EXPECT_EQ(engine.PrepareNextTransmitSlot(192, timing),
              TxSlotPrepareResult::kSlotProviderUnavailable);
    TestTxSlotProvider provider{};
    provider.allowAcquire = false;
    engine.BindSlotProvider(&provider);
    EXPECT_EQ(engine.PrepareNextTransmitSlot(192, timing),
              TxSlotPrepareResult::kPacketizerRejected);

    TestAudioStreamProfile profile{};
    ASSERT_TRUE(ConfigureEngine(engine, profile));
    EXPECT_EQ(engine.PrepareNextTransmitSlot(192, timing),
              TxSlotPrepareResult::kSlotAcquireFailed);
}

} // namespace
