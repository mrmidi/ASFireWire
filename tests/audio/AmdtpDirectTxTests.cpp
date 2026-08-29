#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Runtime/PcmPublicationCache.hpp"

#include <gtest/gtest.h>

#include <array>

namespace {
using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Audio::Runtime::PcmPublicationCache;
using ASFW::Audio::Runtime::PcmPublishResult;
using ASFW::Encoding::AudioWireFormat;
using ASFW::Isoch::Audio::AudioStreamConfig;
using ASFW::Isoch::Audio::AudioStreamDirection;
using ASFW::Isoch::Audio::IAudioStreamProfile;
using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;

class TestProfile final : public IAudioStreamProfile {
public:
    uint8_t sourceOffset{0};
    bool substituteSilence{false};
    ASFW::Isoch::Audio::AudioStreamTxPolicy TxStreamPolicy() const noexcept override {
        ASFW::Isoch::Audio::AudioStreamTxPolicy policy{};
        policy.substituteSilenceOnPcmUnavailable = substituteSilence;
        return policy;
    }
    const char* Name() const noexcept override { return "v3-test"; }
    AudioWireFormat TxWireFormat() const noexcept override {
        return AudioWireFormat::kAM824;
    }
    AudioWireFormat RxWireFormat() const noexcept override {
        return AudioWireFormat::kAM824;
    }
    bool BuildDefaultTxStreamConfig(AudioStreamConfig& out) const noexcept override {
        out = {};
        out.direction = AudioStreamDirection::HostToDevice;
        out.sampleRate = 48'000;
        out.streamMode = ASFW::Encoding::StreamMode::kBlocking;
        out.pcmChannels = 2;
        out.dbs = 2;
        out.framesPerDataPacket = 8;
        out.sourceChannelOffset = sourceOffset;
        return true;
    }
    bool BuildDefaultRxStreamConfig(AudioStreamConfig& out) const noexcept override {
        out = {};
        out.direction = AudioStreamDirection::DeviceToHost;
        return true;
    }
    uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
    uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }
};

class SlotProvider final : public IAmdtpTxSlotProvider {
public:
    bool publishAllowed{true};
    std::array<uint8_t, 256> bytes{};
    PreparedTxPacket packet{};

    bool AcquireWritableSlot(uint32_t packetIndex,
                             TxPacketSlotView& out) noexcept override {
        out = {packetIndex, bytes.data(), static_cast<uint32_t>(bytes.size())};
        return true;
    }
    bool PublishSlot(const PreparedTxPacket& value) noexcept override {
        if (!publishAllowed) return false;
        packet = value;
        return true;
    }
    uint32_t SlotCount() const noexcept override { return 192; }
};

bool Configure(DiceTxStreamEngine& engine, const TestProfile& profile) {
    AudioStreamConfig config{};
    return profile.BuildDefaultTxStreamConfig(config) &&
           engine.Configure(profile, config);
}

TxPresentationPlan DataPlan(uint64_t first = 100, uint64_t cycle = 7) {
    return {
        .epoch = 3,
        .cycleOrdinal = cycle,
        .firstAudioFrame = first,
        .frameCount = 8,
        .presentationBusTicks = 1'000'000 + cycle * 3'072,
        .disposition = AmdtpPacketDisposition::Data,
    };
}

TEST(AmdtpDirectTxTests, PacketizerEncodesSuppliedAbsoluteFrame) {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpStreamConfig config{};
    config.sampleRate = 48'000;
    config.dbs = 2;
    config.pcmChannels = 2;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 128;
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, {}));

    std::array<uint8_t, 128> bytes{};
    std::array<float, 16> pcm{};
    PreparedTxPacket packet{};
    ASSERT_TRUE(packetizer.PrepareDataPacket(
        {5, bytes.data(), static_cast<uint32_t>(bytes.size())},
        DataPlan(42'000), 8, 0x1234, {pcm.data(), 8, 2}, packet));
    EXPECT_EQ(packet.firstAudioFrame, 42'000U);
    EXPECT_EQ(timeline.FinalizedFrameEnd(), 0U);
    ASSERT_TRUE(packetizer.CommitPreparedPacket(packet, 8));
    EXPECT_EQ(timeline.FinalizedFrameEnd(), 42'008U);
}

TEST(AmdtpDirectTxTests, EngineReadsMatchingEpochAndAbsoluteRange) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);
    std::array<float, 32> host{};
    ASSERT_EQ(cache.Publish({host.data(), 3, 96, 16, 16, 2}),
              PcmPublishResult::Published);
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);
    EXPECT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);
    EXPECT_EQ(slots.packet.firstAudioFrame, 100U);
    EXPECT_TRUE(slots.packet.pcmFinalized);
}

TEST(AmdtpDirectTxTests, FailedPublicationDoesNotCommitTimeline) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);
    std::array<float, 16> host{};
    ASSERT_EQ(cache.Publish({host.data(), 3, 0, 8, 8, 2}),
              PcmPublishResult::Published);
    engine.BindPcmSource(&cache);
    slots.publishAllowed = false;
    EXPECT_EQ(engine.PrepareTransmitSlot(0, DataPlan(0), 8, 0x1111),
              TxSlotPrepareResult::SlotPublishFailed);
    EXPECT_EQ(engine.Timeline().FinalizedFrameEnd(), 0U);
    slots.publishAllowed = true;
    EXPECT_EQ(engine.PrepareTransmitSlot(0, DataPlan(0), 8, 0x1111),
              TxSlotPrepareResult::Prepared);
    EXPECT_EQ(engine.Timeline().FinalizedFrameEnd(), 8U);
}

TEST(AmdtpDirectTxTests, MissedNoDataRetainsExactPlannedRange) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider provider{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&provider);
    auto plan = DataPlan(8'192, 99);
    plan.disposition = AmdtpPacketDisposition::NoData;
    ASSERT_EQ(engine.PrepareTransmitSlot(99, plan, 0, 0xFFFF),
              TxSlotPrepareResult::Prepared);
    const auto* slot = engine.Timeline().SlotByIndex(99);
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->isData);
    EXPECT_EQ(slot->firstAudioFrame, 8'192U);
    EXPECT_EQ(slot->plannedFrameCount, 8U);
}

TEST(AmdtpDirectTxTests, DiceStreamsConsumeIdenticalPlanCoordinates) {
    TestProfile firstProfile{};
    TestProfile secondProfile{};
    secondProfile.sourceOffset = 2;
    DiceTxStreamEngine first{};
    DiceTxStreamEngine second{};
    SlotProvider firstSlots{};
    SlotProvider secondSlots{};
    ASSERT_TRUE(Configure(first, firstProfile));
    ASSERT_TRUE(Configure(second, secondProfile));
    first.BindSlotProvider(&firstSlots);
    second.BindSlotProvider(&secondSlots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(4, 8192));
    cache.BeginEpoch(3);
    std::array<float, 32> host{};
    ASSERT_EQ(cache.Publish({host.data(), 3, 0, 8, 8, 4}),
              PcmPublishResult::Published);
    first.BindPcmSource(&cache);
    second.BindPcmSource(&cache);
    const auto plan = DataPlan(0, 55);
    ASSERT_EQ(first.PrepareTransmitSlot(55, plan, 8, 0x2222),
              TxSlotPrepareResult::Prepared);
    ASSERT_EQ(second.PrepareTransmitSlot(55, plan, 8, 0x2222),
              TxSlotPrepareResult::Prepared);
    EXPECT_EQ(firstSlots.packet.firstAudioFrame,
              secondSlots.packet.firstAudioFrame);
    EXPECT_EQ(firstSlots.packet.presentationBusTicks,
              secondSlots.packet.presentationBusTicks);
    EXPECT_EQ(firstSlots.packet.syt, secondSlots.packet.syt);
}

} // namespace

// Content availability must not gate packet production. Withholding the packet
// starves the IT descriptor ring, which is what faulted the context on hardware
// ("IT FATAL: slot 175 not committed"). Linux fills the gap with
// write_pcm_silence() (sound/firewire/amdtp-am824.c:358-363) and Apple's
// AppleFWAudio has no availability check at all.

TEST(AmdtpDirectTxTests, UnavailablePcmIsWithheldWhenPolicyDisallowsSilence) {
    TestProfile profile{};
    profile.substituteSilence = false;
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);   // nothing published for [100,108)
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);

    EXPECT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::PcmNotYetPublished);
    EXPECT_EQ(engine.Counters().pcmSilenceSubstitutions.load(), 0U);
}

TEST(AmdtpDirectTxTests, UnavailablePcmBecomesSilentDataWhenPolicyAllows) {
    TestProfile profile{};
    profile.substituteSilence = true;
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);   // nothing published for [100,108)
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);

    EXPECT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);
    // Still a DATA packet consuming its planned range: a NO-DATA packet
    // consumes no frames and would stall the data-block cadence.
    EXPECT_TRUE(slots.packet.isData);
    EXPECT_EQ(slots.packet.framesInPacket, 8U);
    EXPECT_EQ(slots.packet.firstAudioFrame, 100U);
    EXPECT_EQ(slots.packet.syt, 0x4567U);
    EXPECT_EQ(engine.Counters().pcmSilenceSubstitutions.load(), 1U);

    // Every PCM slot carries Linux's exact AM824 MBLA silence word.
    for (uint32_t frame = 0; frame < 8; ++frame) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            const size_t offset = 8 + (frame * 2 + channel) * 4;
            const uint32_t word =
                (static_cast<uint32_t>(slots.bytes[offset]) << 24) |
                (static_cast<uint32_t>(slots.bytes[offset + 1]) << 16) |
                (static_cast<uint32_t>(slots.bytes[offset + 2]) << 8) |
                static_cast<uint32_t>(slots.bytes[offset + 3]);
            EXPECT_EQ(word, 0x40000000U)
                << "frame " << frame << " channel " << channel;
        }
    }
}
