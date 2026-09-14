#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Runtime/PcmPublicationCache.hpp"
#include "Audio/Wire/AM824/MpxMidiDemux.hpp"
#include "Midi/Transport/MidiTransportBlock.hpp"

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
using ASFW::Protocols::Audio::DICE::TxSlotFillResult;

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

    // Late-payload seam. finalizedEnd defaults to 0, so every packet is fillable
    // unless a test freezes it.
    uint64_t finalizedEnd{0};
    std::array<uint8_t, 256> lateBytes{};
    uint32_t latePublished{0};
    bool lateAcquireAllowed{true};

    uint64_t FinalizedEnd() const noexcept override { return finalizedEnd; }

    bool AcquireLatePayloadSlot(uint32_t packetIndex,
                                TxPacketSlotView& out) noexcept override {
        if (!lateAcquireAllowed || packetIndex < finalizedEnd) return false;
        out = {packetIndex, lateBytes.data(),
               static_cast<uint32_t>(lateBytes.size())};
        return true;
    }

    bool PublishLatePayload(uint32_t) noexcept override {
        ++latePublished;
        return true;
    }
};

bool Configure(DiceTxStreamEngine& engine, const IAudioStreamProfile& profile) {
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

// --- Late PCM fill (finality is independent of descriptor mapping) -----------
//
// A packet is armed with silence at plan time so transport always has a valid
// image for the slot, then re-encoded with real PCM if content arrives before
// the slot is mapped into a descriptor. The two images must agree everywhere
// except the sample words, so a fill that loses the race to freeze is
// indistinguishable from never having happened.

struct RefillFixture {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    AmdtpTxPacketizer packetizer{};

    bool Setup() {
        if (!timeline.AttachSlots(slots.data(), slots.size())) return false;
        AmdtpStreamConfig config{};
        config.sampleRate = 48'000;
        config.dbs = 2;
        config.pcmChannels = 2;
        config.framesPerDataPacket = 8;
        config.maxPacketBytes = 128;
        packetizer.BindTimeline(&timeline);
        return packetizer.Configure(config, {});
    }
};

TEST(AmdtpDirectTxTests, RefillPcmMatchesEncodingThePcmUpFront) {
    RefillFixture armFixture{};
    RefillFixture directFixture{};
    ASSERT_TRUE(armFixture.Setup());
    ASSERT_TRUE(directFixture.Setup());

    std::array<float, 16> silence{};
    std::array<float, 16> content{};
    for (size_t i = 0; i < content.size(); ++i) {
        content[i] = -1.0f + static_cast<float>(i) * 0.05f;
    }

    // Path A: arm with silence, then fill with content.
    std::array<uint8_t, 128> armed{};
    PreparedTxPacket armedPacket{};
    ASSERT_TRUE(armFixture.packetizer.PrepareDataPacket(
        {5, armed.data(), static_cast<uint32_t>(armed.size())},
        DataPlan(42'000), 8, 0x1234, {silence.data(), 8, 2}, armedPacket));
    std::array<uint8_t, 128> filled = armed;
    ASSERT_TRUE(armFixture.packetizer.RefillPcm(
        {5, filled.data(), static_cast<uint32_t>(filled.size())},
        armedPacket, {content.data(), 8, 2}));

    // Path B: encode the content directly, as today's one-shot path does.
    std::array<uint8_t, 128> direct{};
    PreparedTxPacket directPacket{};
    ASSERT_TRUE(directFixture.packetizer.PrepareDataPacket(
        {5, direct.data(), static_cast<uint32_t>(direct.size())},
        DataPlan(42'000), 8, 0x1234, {content.data(), 8, 2}, directPacket));

    EXPECT_EQ(filled, direct);
    EXPECT_EQ(armedPacket.dbc, directPacket.dbc);
    EXPECT_EQ(armedPacket.syt, directPacket.syt);
    EXPECT_EQ(armedPacket.byteCount, directPacket.byteCount);
}

TEST(AmdtpDirectTxTests, RefillPcmChangesOnlyTheSampleWords) {
    RefillFixture fixture{};
    ASSERT_TRUE(fixture.Setup());

    std::array<float, 16> silence{};
    std::array<float, 16> content{};
    for (auto& value : content) value = 0.5f;

    std::array<uint8_t, 128> armed{};
    PreparedTxPacket packet{};
    ASSERT_TRUE(fixture.packetizer.PrepareDataPacket(
        {5, armed.data(), static_cast<uint32_t>(armed.size())},
        DataPlan(42'000), 8, 0x1234, {silence.data(), 8, 2}, packet));

    std::array<uint8_t, 128> filled = armed;
    ASSERT_TRUE(fixture.packetizer.RefillPcm(
        {5, filled.data(), static_cast<uint32_t>(filled.size())},
        packet, {content.data(), 8, 2}));

    // The CIP header carries DBC and SYT; both were decided at arm time.
    for (uint32_t index = 0; index < 8; ++index) {
        EXPECT_EQ(armed[index], filled[index]) << "CIP byte " << index;
    }
    // Nothing beyond the packet may be touched in either image.
    for (uint32_t index = packet.byteCount;
         index < static_cast<uint32_t>(armed.size()); ++index) {
        EXPECT_EQ(armed[index], 0u) << "past end " << index;
        EXPECT_EQ(filled[index], 0u) << "past end " << index;
    }
    EXPECT_NE(armed, filled);
}

TEST(AmdtpDirectTxTests, RefillPcmDoesNotAdvanceCadenceOrDbc) {
    RefillFixture fixture{};
    ASSERT_TRUE(fixture.Setup());

    std::array<float, 16> pcm{};
    std::array<uint8_t, 128> bytes{};
    PreparedTxPacket first{};
    ASSERT_TRUE(fixture.packetizer.PrepareDataPacket(
        {5, bytes.data(), static_cast<uint32_t>(bytes.size())},
        DataPlan(42'000), 8, 0x1234, {pcm.data(), 8, 2}, first));

    // Many fills between arm and commit must leave the counter untouched.
    for (int i = 0; i < 4; ++i) {
        std::array<uint8_t, 128> image{};
        ASSERT_TRUE(fixture.packetizer.RefillPcm(
            {5, image.data(), static_cast<uint32_t>(image.size())},
            first, {pcm.data(), 8, 2}));
    }
    EXPECT_EQ(fixture.timeline.FinalizedFrameEnd(), 0U);

    PreparedTxPacket second{};
    ASSERT_TRUE(fixture.packetizer.PrepareDataPacket(
        {5, bytes.data(), static_cast<uint32_t>(bytes.size())},
        DataPlan(42'000), 8, 0x1234, {pcm.data(), 8, 2}, second));
    EXPECT_EQ(second.dbc, first.dbc);
}

TEST(AmdtpDirectTxTests, RefillPcmRejectsAMismatchedOrNonDataArm) {
    RefillFixture fixture{};
    ASSERT_TRUE(fixture.Setup());

    std::array<float, 16> pcm{};
    std::array<uint8_t, 128> bytes{};
    PreparedTxPacket packet{};
    ASSERT_TRUE(fixture.packetizer.PrepareDataPacket(
        {5, bytes.data(), static_cast<uint32_t>(bytes.size())},
        DataPlan(42'000), 8, 0x1234, {pcm.data(), 8, 2}, packet));

    std::array<uint8_t, 128> image{};
    const TxPacketSlotView wrongIndex{
        6, image.data(), static_cast<uint32_t>(image.size())};
    EXPECT_FALSE(fixture.packetizer.RefillPcm(
        wrongIndex, packet, {pcm.data(), 8, 2}));

    const TxPacketSlotView good{
        5, image.data(), static_cast<uint32_t>(image.size())};
    // Frame count must match the armed geometry: a fill may not change it.
    EXPECT_FALSE(fixture.packetizer.RefillPcm(
        good, packet, {pcm.data(), 4, 2}));
    EXPECT_FALSE(fixture.packetizer.RefillPcm(good, packet, {nullptr, 8, 2}));

    PreparedTxPacket noData = packet;
    noData.isData = false;
    EXPECT_FALSE(fixture.packetizer.RefillPcm(
        good, noData, {pcm.data(), 8, 2}));
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

// Arming no longer consults content at all, so neither of these outcomes --
// withholding a packet, or "substituting" silence -- is reachable from
// PrepareTransmitSlot any more. Silence is the armed state, not a fallback.
// What replaces them is the arm/fill pair below.

TEST(AmdtpDirectTxTests, ArmingNeverFailsForMissingContent) {
    for (const bool substitute : {false, true}) {
        TestProfile profile{};
        profile.substituteSilence = substitute;
        DiceTxStreamEngine engine{};
        SlotProvider slots{};
        ASSERT_TRUE(Configure(engine, profile));
        engine.BindSlotProvider(&slots);
        PcmPublicationCache cache{};
        ASSERT_TRUE(cache.Configure(2, 8192));
        cache.BeginEpoch(3);   // nothing published for [100,108)
        engine.BindPcmSource(&cache);
        engine.ResetForStart(0);

        // Transport must have a complete image for the slot regardless of
        // whether content exists, so withholding is not an option and the
        // policy cannot change the outcome.
        EXPECT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
                  TxSlotPrepareResult::Prepared)
            << "substituteSilence=" << substitute;
        EXPECT_TRUE(slots.packet.isData);
        EXPECT_EQ(slots.packet.framesInPacket, 8U);
    }
}

TEST(AmdtpDirectTxTests, FillReportsContentUnavailableAndLeavesTheArmedSilence) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);   // nothing published for [100,108)
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);

    ASSERT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);
    const auto armed = slots.bytes;

    EXPECT_EQ(engine.FillTransmitSlot(7), TxSlotFillResult::ContentUnavailable);
    EXPECT_EQ(engine.Counters().lateFillsUnavailable.load(), 1U);
    EXPECT_EQ(engine.Counters().lateFillsPublished.load(), 0U);
    // The armed image is untouched: no late image was offered.
    EXPECT_EQ(slots.bytes, armed);
    EXPECT_EQ(slots.latePublished, 0U);
}

TEST(AmdtpDirectTxTests, ResetForStartClearsTheCountersTransportResetsToo) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);

    ASSERT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);
    ASSERT_EQ(engine.FillTransmitSlot(7), TxSlotFillResult::ContentUnavailable);
    ASSERT_EQ(engine.Counters().lateFillsUnavailable.load(), 1U);
    ASSERT_EQ(engine.Counters().packetsPrepared.load(), 1U);

    // The queue zeroes its own consumer counters on every arm. These are read
    // beside them -- filled minus transport's lost is advertised as the
    // truthful content figure -- so carrying them across a restart would raise
    // that figure without a single new packet reaching the wire.
    engine.ResetForStart(0);
    EXPECT_EQ(engine.Counters().lateFillsUnavailable.load(), 0U);
    EXPECT_EQ(engine.Counters().lateFillsPublished.load(), 0U);
    EXPECT_EQ(engine.Counters().packetsPrepared.load(), 0U);
}

TEST(AmdtpDirectTxTests, FillIsRefusedOnceTransportHasFrozenTheSlot) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);
    std::array<float, 32> host{};
    for (size_t i = 0; i < host.size(); ++i) host[i] = 0.25f;
    // Cover both packets: [100,108) and [108,116).
    ASSERT_EQ(cache.Publish({host.data(), 3, 100, 16,
                             static_cast<uint32_t>(host.size() / 2), 2}),
              PcmPublishResult::Published);
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);

    ASSERT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);

    // Transport has bound packet 7: the content lead has run out.
    slots.finalizedEnd = 8;
    EXPECT_EQ(engine.FillTransmitSlot(7), TxSlotFillResult::TooLate);
    EXPECT_EQ(engine.Counters().lateFillsTooLate.load(), 1U);
    EXPECT_EQ(slots.latePublished, 0U);

    // One packet further on is still writable.
    ASSERT_EQ(engine.PrepareTransmitSlot(8, DataPlan(108), 8, 0x4568),
              TxSlotPrepareResult::Prepared);
    EXPECT_EQ(engine.FillTransmitSlot(8), TxSlotFillResult::Filled);
    EXPECT_EQ(slots.latePublished, 0U) << "encode must not publish on its own";
    EXPECT_TRUE(engine.CommitFill(8));
    EXPECT_EQ(slots.latePublished, 1U);
}

TEST(AmdtpDirectTxTests, FrozenWithoutContentIsCountedOncePerArmedDataPacket) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);
    std::array<float, 32> host{};
    ASSERT_EQ(cache.Publish({host.data(), 3, 100, 16,
                             static_cast<uint32_t>(host.size() / 2), 2}),
              PcmPublishResult::Published);
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);

    ASSERT_EQ(engine.PrepareTransmitSlot(7, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);
    auto noData = DataPlan(108);
    noData.disposition = AmdtpPacketDisposition::NoData;
    noData.frameCount = 0;
    ASSERT_EQ(engine.PrepareTransmitSlot(8, noData, 0, 0xFFFF),
              TxSlotPrepareResult::Prepared);
    ASSERT_EQ(engine.PrepareTransmitSlot(9, DataPlan(108), 8, 0x4569),
              TxSlotPrepareResult::Prepared);
    ASSERT_EQ(engine.FillTransmitSlot(9), TxSlotFillResult::Filled);
    ASSERT_TRUE(engine.CommitFill(9));

    // Packet 7 was armed and never filled: it goes out as silence.
    engine.NoteFrozenWithoutContent(7);
    engine.NoteFrozenWithoutContent(7);   // idempotent
    // Packet 8 is cadence NO-DATA -- it carries no samples to be missing.
    engine.NoteFrozenWithoutContent(8);
    // Packet 9 carries real content.
    engine.NoteFrozenWithoutContent(9);
    EXPECT_EQ(engine.Counters().pcmSilenceSubstitutions.load(), 1U);

    // A packet this engine never armed is not attributed to it either.
    engine.NoteFrozenWithoutContent(42);
    EXPECT_EQ(engine.Counters().pcmSilenceSubstitutions.load(), 1U);
}

TEST(AmdtpDirectTxTests, FillIsRefusedForCadenceNoDataAndForARepeatFill) {
    TestProfile profile{};
    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);
    PcmPublicationCache cache{};
    ASSERT_TRUE(cache.Configure(2, 8192));
    cache.BeginEpoch(3);
    std::array<float, 32> host{};
    ASSERT_EQ(cache.Publish({host.data(), 3, 100, 8,
                             static_cast<uint32_t>(host.size() / 2), 2}),
              PcmPublishResult::Published);
    engine.BindPcmSource(&cache);
    engine.ResetForStart(0);

    // Never armed.
    EXPECT_EQ(engine.FillTransmitSlot(3), TxSlotFillResult::NotFillable);

    auto noData = DataPlan(100);
    noData.disposition = AmdtpPacketDisposition::NoData;
    noData.frameCount = 0;
    ASSERT_EQ(engine.PrepareTransmitSlot(3, noData, 0, 0xFFFF),
              TxSlotPrepareResult::Prepared);
    // A cadence NO-DATA packet carries no samples and keeps its planned
    // geometry: filling it would move the data-block cadence.
    EXPECT_EQ(engine.FillTransmitSlot(3), TxSlotFillResult::NotFillable);

    ASSERT_EQ(engine.PrepareTransmitSlot(4, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);
    EXPECT_EQ(engine.FillTransmitSlot(4), TxSlotFillResult::Filled);
    EXPECT_TRUE(engine.CommitFill(4));
    // Content is offered once per arm; a second attempt is not an error but
    // must not republish.
    EXPECT_EQ(engine.FillTransmitSlot(4), TxSlotFillResult::NotFillable);
    EXPECT_FALSE(engine.CommitFill(4));
    EXPECT_EQ(slots.latePublished, 1U);
}

TEST(AmdtpDirectTxTests, ArmedDataPacketCarriesTheExactAm824SilenceWord) {
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

TEST(AmdtpDirectTxTests, MidiOnlyFillComposesMidiIntoSilenceWithoutPcmSource) {
    class MidiTestProfile final : public IAudioStreamProfile {
    public:
        ASFW::Isoch::Audio::AudioStreamTxPolicy TxStreamPolicy() const noexcept override {
            return {};
        }
        const char* Name() const noexcept override { return "midi-test"; }
        AudioWireFormat TxWireFormat() const noexcept override { return AudioWireFormat::kAM824; }
        AudioWireFormat RxWireFormat() const noexcept override { return AudioWireFormat::kAM824; }
        bool BuildDefaultTxStreamConfig(AudioStreamConfig& out) const noexcept override {
            out = {};
            out.direction = AudioStreamDirection::HostToDevice;
            out.sampleRate = 48'000;
            out.streamMode = ASFW::Encoding::StreamMode::kBlocking;
            out.pcmChannels = 2;
            out.dbs = 3; // 2 audio + 1 MIDI
            out.framesPerDataPacket = 8;
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
    } profile;

    DiceTxStreamEngine engine{};
    SlotProvider slots{};
    ASSERT_TRUE(Configure(engine, profile));
    engine.BindSlotProvider(&slots);

    // No PCM source bound!
    engine.BindPcmSource(nullptr);

    // Set up MIDI transport block with 1 byte on port 0
    ASFW::Midi::MidiTransportBlock midiBlock{};
    midiBlock.Arm(1);
    const uint8_t byteToSend[] = {0x90};
    ASSERT_TRUE(midiBlock.hostToDevice[0].TryWrite(byteToSend));

    const ASFW::Encoding::MpxMidiGeometry midiGeometry{
        .dbs = 3,
        .midiSlotIndex = 2,
        .portCount = 1,
        .dbcAligned = true,
    };
    engine.SetMidiTransport(&midiBlock, 1, midiGeometry, 48000, 8);
    engine.ResetForStart(0);

    // Arm slot 0
    EXPECT_EQ(engine.PrepareTransmitSlot(0, DataPlan(100), 8, 0x4567),
              TxSlotPrepareResult::Prepared);

    // Fill slot 0 in MIDI-only mode
    EXPECT_EQ(engine.FillTransmitSlot(0), TxSlotFillResult::Filled);

    // Commit fill
    EXPECT_TRUE(engine.CommitFill(0));
    EXPECT_EQ(slots.latePublished, 1U);

    // Verify MIDI byte was committed from ring (ring is now empty)
    uint8_t peekByte = 0;
    EXPECT_EQ(midiBlock.hostToDevice[0].Peek(std::span<uint8_t, 1>(&peekByte, 1)), 0U);

    // Verify lateBytes carries AM824 silence for PCM slots and MIDI byte for slot 2
    // CIP header is 8 bytes, each slot is 4 bytes.
    // Frame 0: slot 0 (offset 8) = 0x40000000, slot 1 (offset 12) = 0x40000000, slot 2 (offset 16) = MIDI (0x81900000)
    const size_t midiOffset = 8 + (0 * 3 + 2) * 4;
    const uint32_t midiWord =
        (static_cast<uint32_t>(slots.lateBytes[midiOffset]) << 24) |
        (static_cast<uint32_t>(slots.lateBytes[midiOffset + 1]) << 16) |
        (static_cast<uint32_t>(slots.lateBytes[midiOffset + 2]) << 8) |
        static_cast<uint32_t>(slots.lateBytes[midiOffset + 3]);
    EXPECT_EQ(midiWord, 0x81900000U);
}
