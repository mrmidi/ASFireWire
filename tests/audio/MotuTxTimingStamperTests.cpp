// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The MOTU transmit SPH regulator behind the ITxDeviceTimingStamper seam.
//
// The engine is the production DiceTxStreamEngine, configured from the
// production MotuProfile, and the stamper and payload writer are bound to it as
// ArmPrimaryTxProducer binds them. `MotuTx::Apply` mirrors the producer's servo
// hook: feed the servo, then put the payload writer on the regulator's mute.

#include "Audio/DriverKit/Config/MOTU/MotuProfile.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/MOTU/MotuDeviceTiming.hpp"
#include "Audio/Wire/MOTU/MotuModel.hpp"
#include "Audio/Wire/MOTU/MotuPayloadWriter.hpp"
#include "Audio/Wire/MOTU/MotuPortLayout.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using namespace ASFW::Encoding::Motu;
using ASFW::Audio::TxTimingStampResult;
using ASFW::Audio::Wire::MotuSphServoApplyResult;
using ASFW::Audio::Wire::MotuSphServoRuntimeInput;
using ASFW::Audio::Wire::MotuTxTimingStamper;
using ASFW::Isoch::Audio::MOTU::Profiles::MotuProfile;
using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;

constexpr int64_t kOneQ32 = SphSynthesizer::kOneQ32;
constexpr int64_t kLead = kDefaultPresentationLeadTicks;
// Zero-frame offset for payload-only tests.
constexpr uint32_t kRelativeLoop = 0;
constexpr uint32_t k828 = 0x1;     // protocol v1
constexpr uint32_t k828mk2 = 0x3;  // protocol v2
constexpr uint32_t k828mk3 = 0x15; // protocol v3

class MotuSlotProvider final : public IAmdtpTxSlotProvider {
public:
    bool allowAcquire{true};
    std::array<uint8_t, 512> bytes{};
    PreparedTxPacket published{};

    bool AcquireWritableSlot(uint64_t packetIndex, TxPacketSlotView& outSlot) noexcept override {
        if (!allowAcquire) {
            return false;
        }
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

// The MOTU TX path as ArmPrimaryTxProducer assembles it, for one model.
struct MotuTx final {
    explicit MotuTx(uint32_t unitVersion = k828mk3) : profile(unitVersion) {}

    MotuProfile profile;
    ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
    DiceTxStreamEngine engine{};
    MotuTxTimingStamper stamper{};
    MotuPayloadWriter writer{};
    MotuSlotProvider provider{};
    std::atomic<uint32_t> ready{0};

    [[nodiscard]] bool Arm() {
        if (!profile.BuildDefaultTxStreamConfig(txConfig) || !engine.Configure(profile, txConfig)) {
            return false;
        }
        const auto policy = profile.TxStreamPolicy();
        if (!stamper.Configure(txConfig.dbs, txConfig.sampleRate,
                               policy.motuPresentationLeadTicks,
                               policy.motuPresentationOffsetFrames)) {
            return false;
        }
        writer.Configure({.pcmChunks = txConfig.motuPcmChunks,
                          .sourceChannelOffset = txConfig.sourceChannelOffset,
                          .ports = ASFW::Isoch::Audio::MOTU::Profiles::PlaybackPortsForSwVersion(
                              policy.motuUnitVersion, txConfig.sampleRate, txConfig.motuPcmChunks),
                          .pcmByteOffset = txConfig.motuPcmByteOffset});
        writer.SetPcmMuted(stamper.IsOutputMuted());
        writer.BindTimeline(&engine.Timeline());
        engine.SetPayloadWriter(&writer);
        engine.BindTimingStamper(&stamper);
        engine.BindSlotProvider(&provider);
        engine.ResetForStart(0, 0);
        stamper.BindReadiness(&ready);
        return true;
    }

    // The producer's servo hook.
    MotuSphServoApplyResult Apply(const MotuSphServoRuntimeInput& input) {
        const auto result = stamper.ApplyServoInput(input);
        writer.SetPcmMuted(stamper.IsOutputMuted());
        return result;
    }

    // The SPH of one block of the packet the provider holds.
    [[nodiscard]] uint32_t BlockSph(uint32_t block) const {
        return ReadBE32(provider.bytes.data() + 8 + block * txConfig.dbs * 4);
    }

    // Byte offset, from the CIP header, of host channel 0's PCM in `block`.
    [[nodiscard]] size_t HostChannelZeroAt(uint32_t block) const {
        const auto ports = EffectivePortMap(ASFW::Isoch::Audio::MOTU::Profiles::PlaybackPortsForSwVersion(
            profile.TxStreamPolicy().motuUnitVersion, txConfig.sampleRate, txConfig.motuPcmChunks),
            txConfig.motuPcmChunks);
        return 8 + block * txConfig.dbs * 4 + txConfig.motuPcmByteOffset +
               ChunkForHostChannel(ports, 0) * kBytesPerChunk;
    }
};

// A DATA packet as the producer's replay branch builds it for MOTU: a replayed
// block count, SYT NO_INFO, and the packet's transmit time.
AmdtpTimingState DataTiming(int64_t transmitTicks) {
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;
    timing.txClockValid = true;
    timing.nextDataSyt = 0xFFFF;
    timing.transmitCycleValid = true;
    timing.transmitCycle = static_cast<uint32_t>(
        (WrapTick(transmitTicks) / kTicksPerCycle) % kCyclesPerSecond);
    timing.transmitTicks = transmitTicks;
    return timing;
}

AmdtpTimingState UntimedDataTiming() {
    AmdtpTimingState timing = DataTiming(0);
    timing.transmitCycleValid = false;
    timing.transmitTicks = 0;
    return timing;
}

AmdtpTimingState NoDataTiming() {
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::NoData;
    timing.replayValid = true;
    return timing;
}

MotuSphServoRuntimeInput Window(uint64_t generation, uint64_t updates, uint64_t frames,
                                int64_t ticks) {
    return MotuSphServoRuntimeInput{
        .valid = true,
        .streamGeneration = generation,
        .expectedStreamGeneration = generation,
        .bridgeUpdates = updates,
        .rxFrames = frames,
        .rxTicks = ticks,
    };
}

uint32_t SphAt(int64_t ticks) { return SphFromTick(WrapTick(ticks)); }

// ---------------------------------------------------------------------------
// The SPH clock on the wire, for each protocol version's block layout.

class MotuTxTimingStamperLayoutTests : public ::testing::TestWithParam<uint32_t> {};

INSTANTIATE_TEST_SUITE_P(EveryProtocolVersion, MotuTxTimingStamperLayoutTests,
                         ::testing::Values(k828, k828mk2, k828mk3),
                         [](const ::testing::TestParamInfo<uint32_t>& info) {
                             return info.param == k828      ? std::string("V1_828")
                                    : info.param == k828mk2 ? std::string("V2_828mk2")
                                                            : std::string("V3_828mk3");
                         });

// The SPH is the first quadlet of every data block in every version; PCM
// starts after it (v1) or after it and two message chunks (v2, v3).
TEST_P(MotuTxTimingStamperLayoutTests, StampsFreeRunningSphFromTheFirstTransmitTimeAndNeverReseeds) {
    MotuTx tx{GetParam()};
    ASSERT_TRUE(tx.Arm());
    EXPECT_TRUE(tx.engine.IsSytUnaware());
    EXPECT_EQ(tx.txConfig.motuPcmByteOffset, PcmByteOffset(GetParam()));

    // The anchor sits one cycle before a second boundary with a sub-cycle
    // phase, so the seed both keeps that phase and wraps the SPH second.
    const int64_t transmitTicks = static_cast<int64_t>(kTicksPerSecond) * 7 +
                                  7999 * static_cast<int64_t>(kTicksPerCycle) + 2800;
    const int64_t seed = transmitTicks + kLead;
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(transmitTicks)),
              TxSlotPrepareResult::kPrepared);
    ASSERT_TRUE(tx.provider.published.isData);
    EXPECT_EQ(tx.provider.published.syt, 0xFFFFu);
    EXPECT_EQ(tx.stamper.LastFirstSph(), SphAt(seed));
    for (uint32_t block = 0; block < 8; ++block) {
        EXPECT_EQ(tx.BlockSph(block), SphAt(seed + block * 512)) << "block " << block;
    }

    // The payload writer fills PCM behind the SPH without touching it.
    tx.writer.SetPcmMuted(false);
    std::vector<float> host(8 * tx.txConfig.pcmChannels, 0.5f);
    tx.engine.FillFromHostOutput({host.data(), 0, 8, 8, tx.txConfig.pcmChannels}, 0);
    for (uint32_t block = 0; block < 8; ++block) {
        EXPECT_EQ(tx.BlockSph(block), SphAt(seed + block * 512)) << "block " << block;
        EXPECT_NE(tx.provider.bytes[tx.HostChannelZeroAt(block)], 0u) << "block " << block;
    }

    // A header-only packet carries no SPH and does not advance the clock.
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(1, NoDataTiming()),
              TxSlotPrepareResult::kPrepared);
    EXPECT_FALSE(tx.provider.published.isData);
    EXPECT_EQ(tx.provider.published.byteCount, 8u);

    // A later transmit time must not re-seed: the clock free-runs one sample
    // period per block across DATA packets.
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(2, DataTiming(0)),
              TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(tx.BlockSph(0), SphAt(seed + 8 * 512));
    EXPECT_EQ(tx.stamper.LastFirstSph(), tx.BlockSph(0));

    // A new start seeds again.
    tx.engine.ResetForStart(0, 0);
    tx.stamper.ResetForStart();
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(1000)),
              TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(tx.BlockSph(0), SphAt(1000 + kLead));
}

// Before the clock has a seed there is no presentation time: the packet is
// reverted to NO-DATA, as for any stamper without timing. Once seeded the
// clock always has a time, so a DATA packet without its own transmit time
// still goes out as DATA, stamped from the clock.
TEST_P(MotuTxTimingStamperLayoutTests, RevertsToNoDataOnlyBeforeTheSeed) {
    MotuTx tx{GetParam()};
    ASSERT_TRUE(tx.Arm());
    tx.provider.bytes.fill(0xEE);

    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, UntimedDataTiming()),
              TxSlotPrepareResult::kPrepared);
    EXPECT_FALSE(tx.provider.published.isData);
    EXPECT_EQ(tx.engine.Counters().timingUnavailableReverts.load(), 1u);
    EXPECT_FALSE(tx.stamper.IsSeeded());

    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(1, DataTiming(5000)),
              TxSlotPrepareResult::kPrepared);
    ASSERT_TRUE(tx.provider.published.isData);
    EXPECT_TRUE(tx.stamper.IsSeeded());

    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(2, UntimedDataTiming()),
              TxSlotPrepareResult::kPrepared);
    EXPECT_TRUE(tx.provider.published.isData);
    EXPECT_EQ(tx.BlockSph(0), SphAt(5000 + kLead + 8 * 512));
    EXPECT_EQ(tx.engine.Counters().timingUnavailableReverts.load(), 1u);

    // At the seam: kNotApplicable only for a packet with nothing to stamp.
    const TxPacketSlotView slot{0, tx.provider.bytes.data(),
                                static_cast<uint32_t>(tx.provider.bytes.size())};
    EXPECT_EQ(tx.stamper.StampPacket(slot, PreparedTxPacket{}, UntimedDataTiming()),
              TxTimingStampResult::kNotApplicable);
}

// ---------------------------------------------------------------------------
// Configuration.

// The lead is the model's: the stamper adds what it was configured with.
TEST(MotuTxTimingStamperTests, SeedsFromTheTransmitTimePlusTheConfiguredLead) {
    EXPECT_EQ(FindModel(k828mk3)->runtime.presentationLeadTicks, 9216u);
    for (const uint32_t lead : {0u, 9216u, 4096u}) {
        SCOPED_TRACE(lead);
        MotuTxTimingStamper stamper{};
        ASSERT_TRUE(stamper.Configure(4, 48000, lead, kRelativeLoop));
        EXPECT_EQ(stamper.PresentationLeadTicks(), lead);
        std::array<uint8_t, 8 + 16> bytes{};
        const TxPacketSlotView slot{0, bytes.data(), static_cast<uint32_t>(bytes.size())};
        const PreparedTxPacket packet{.byteCount = static_cast<uint32_t>(bytes.size()),
                                      .isData = true,
                                      .framesInPacket = 1,
                                      .dbs = 4};
        ASSERT_EQ(stamper.StampPacket(slot, packet, DataTiming(123456)), TxTimingStampResult::kOk);
        EXPECT_EQ(ReadBE32(bytes.data() + 8), SphAt(123456 + lead));
    }
}

// Each start is configured for its stream's rate: the nominal step, and one
// decision per decision interval of that rate (MotuSphSynthesizer.hpp).
TEST(MotuTxTimingStamperTests, ConfigureFollowsTheStreamRate) {
    MotuTxTimingStamper stamper{};
    for (const uint32_t rate : kClockRates) {
        SCOPED_TRACE(rate);
        ASSERT_TRUE(stamper.Configure(4, rate, kDefaultPresentationLeadTicks, kRelativeLoop));
        EXPECT_TRUE(stamper.IsConfigured());
        EXPECT_FALSE(stamper.IsSeeded());
        EXPECT_TRUE(stamper.IsOutputMuted());
        EXPECT_EQ(stamper.StepQ32(), SphSynthesizer::NominalStepQ32(rate));
        EXPECT_GT(stamper.DecisionIntervalFrames(), 0U);
    }

    // A rate change mid-life is a fresh configuration: no seed survives it.
    ASSERT_TRUE(stamper.Configure(4, 48000, kDefaultPresentationLeadTicks, kRelativeLoop));
    std::array<uint8_t, 8 + 16> bytes{};
    const TxPacketSlotView slot{0, bytes.data(), static_cast<uint32_t>(bytes.size())};
    const PreparedTxPacket packet{.byteCount = static_cast<uint32_t>(bytes.size()),
                                  .isData = true,
                                  .framesInPacket = 1,
                                  .dbs = 4};
    ASSERT_EQ(stamper.StampPacket(slot, packet, DataTiming(1000)), TxTimingStampResult::kOk);
    ASSERT_TRUE(stamper.IsSeeded());
    ASSERT_TRUE(stamper.Configure(4, 96000, kDefaultPresentationLeadTicks, kRelativeLoop));
    EXPECT_FALSE(stamper.IsSeeded());
    EXPECT_EQ(stamper.StepQ32(), 256 * kOneQ32);
    ASSERT_EQ(stamper.StampPacket(slot, packet, DataTiming(2000)), TxTimingStampResult::kOk);
    EXPECT_EQ(ReadBE32(bytes.data() + 8), SphAt(2000 + kLead));
}

// A rate the regulator has no configuration for leaves the stamper
// unconfigured: never muted (no decision would lift it), never seeded.
TEST(MotuTxTimingStamperTests, ConfigureRefusesARateWithoutARegulatorConfiguration) {
    MotuTxTimingStamper stamper{};
    ASSERT_TRUE(stamper.Configure(4, 48000, kDefaultPresentationLeadTicks, kRelativeLoop));
    for (const uint32_t rate : {0u, 32000u, 22050u}) {
        SCOPED_TRACE(rate);
        EXPECT_FALSE(stamper.Configure(4, rate, kDefaultPresentationLeadTicks, kRelativeLoop));
        EXPECT_FALSE(stamper.IsConfigured());
        EXPECT_FALSE(stamper.IsOutputMuted());
        std::array<uint8_t, 8 + 16> bytes{};
        const TxPacketSlotView slot{0, bytes.data(), static_cast<uint32_t>(bytes.size())};
        const PreparedTxPacket packet{.byteCount = static_cast<uint32_t>(bytes.size()),
                                      .isData = true,
                                      .framesInPacket = 1,
                                      .dbs = 4};
        EXPECT_EQ(stamper.StampPacket(slot, packet, DataTiming(1000)),
                  TxTimingStampResult::kTimingUnavailable);
        EXPECT_FALSE(stamper.ApplyServoInput(Window(1, 1, 512, 512 * 512)).observationApplied);
    }
}

TEST(MotuTxTimingStamperTests, AppliedCorrectionCountsOnlyStampedFramesAndResetClearsIt) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());
    const auto observation = [](uint64_t update, uint64_t frame, int64_t ticks) {
        return MotuSphServoRuntimeInput{.valid=true, .streamGeneration=7, .expectedStreamGeneration=7,
            .bridgeUpdates=update, .rxFrames=frame+8, .rxTicks=ticks+4096,
            .rxFirstFrame=frame, .txFirstFrame=frame,
            .rxSphTicks=WrapTick(kLead+ticks-13*512), .replayEpoch=1, .expectedReplayEpoch=1,
            .isData=true, .transmitCycleValid=true, .transmitTicks=0};
    };
    ASSERT_TRUE(tx.Apply(observation(1,0,0)).observationApplied);
    uint64_t packet=0;
    const auto advance = [&] {
        for (unsigned i=0;i<64;++i) {
            EXPECT_EQ(tx.engine.PrepareNextTransmitSlot(packet++,DataTiming(0)),TxSlotPrepareResult::kPrepared);
        }
    };
    advance();
    ASSERT_TRUE(tx.Apply(observation(2,512,512*512)).observationApplied);
    advance();
    ASSERT_TRUE(tx.Apply(observation(3,1024,512*512+512*511)).stepApplied);
    const int64_t step=tx.stamper.StepQ32();
    ASSERT_NE(step,512*kOneQ32);
    const int64_t baseline=tx.stamper.AppliedCorrectionQ32();
    tx.provider.allowAcquire=false;
    EXPECT_EQ(tx.engine.PrepareNextTransmitSlot(packet,DataTiming(0)),TxSlotPrepareResult::kSlotAcquireFailed);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(),baseline);
    tx.provider.allowAcquire=true;
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(packet++,DataTiming(0)),TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(),baseline+8*(step-512*kOneQ32));
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(packet,NoDataTiming()),TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(),baseline+8*(step-512*kOneQ32));
    tx.stamper.ResetForStart();
    EXPECT_EQ(tx.stamper.StepQ32(),512*kOneQ32);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(),0);
}

} // namespace
