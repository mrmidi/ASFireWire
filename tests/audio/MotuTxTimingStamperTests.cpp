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

bool MotuSphHardResyncEventOccurred(const MotuSphServoApplyResult& result) {
    return result.muteActivated || result.unmuteActivated;
}
constexpr int64_t kOneQ32 = SphSynthesizer::kOneQ32;
constexpr int64_t kLead = kDefaultPresentationLeadTicks;
// No absolute-phase setpoint: the loop holds the phase the stream started with.
constexpr std::optional<int32_t> kRelativeLoop = std::nullopt;
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
                               AbsolutePhaseSetpointTicksAtRate(
                                   policy.motuAbsolutePhaseSetpointTicks48k, txConfig.sampleRate))) {
            return false;
        }
        writer.Configure({.pcmChunks = txConfig.pcmChannels,
                          .sourceChannelOffset = txConfig.sourceChannelOffset,
                          .ports = policy.motuPlaybackPorts,
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
        const auto ports = EffectivePortMap(profile.TxStreamPolicy().motuPlaybackPorts,
                                            txConfig.pcmChannels);
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
    constexpr uint64_t generation = 7;
    ASSERT_TRUE(tx.Apply(Window(generation, 1, 512, 512 * 512)).observationApplied);
    ASSERT_TRUE(tx.Apply(Window(generation, 2, 1024, 2 * 512 * 512)).observationApplied);
    const auto steered = tx.Apply(Window(generation, 3, 1536, 2 * 512 * 512 + 512 * 511));
    ASSERT_TRUE(steered.stepApplied);
    const int64_t step = tx.stamper.StepQ32();
    ASSERT_NE(step, 512 * kOneQ32);

    tx.provider.allowAcquire = false;
    EXPECT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(1000)),
              TxSlotPrepareResult::kSlotAcquireFailed);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(), 0);

    tx.provider.allowAcquire = true;
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(1000)),
              TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(), 8 * (step - 512 * kOneQ32));

    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(1, NoDataTiming()),
              TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(), 8 * (step - 512 * kOneQ32));

    tx.stamper.ResetForStart();
    EXPECT_EQ(tx.stamper.StepQ32(), 512 * kOneQ32);
    EXPECT_EQ(tx.stamper.AppliedCorrectionQ32(), 0);
}

// ---------------------------------------------------------------------------
// The servo, its gate and the drop attribution.

TEST(MotuTxTimingStamperTests, ClosesRxMinusTxPhaseWithOneContinuousServo) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());

    constexpr uint64_t generation = 7;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    constexpr int64_t deviceTicks = windowFrames * 511;

    const auto reference = tx.Apply(Window(generation, 1, windowFrames, nominalTicks));
    ASSERT_TRUE(reference.observationApplied);
    ASSERT_TRUE(reference.stepApplied);
    EXPECT_TRUE(reference.decision.phaseReferenceReset);
    EXPECT_EQ(reference.decision.stepQ32, 512 * kOneQ32);
    // A reference is not a lock, and the output is muted until one arrives.
    EXPECT_FALSE(reference.decision.locked);
    EXPECT_TRUE(reference.outputMuted);
    EXPECT_TRUE(tx.writer.IsPcmMuted());

    // One clean window locks the loop, which puts the gain below into the
    // locked stage.
    const auto locked = tx.Apply(Window(generation, 2, windowFrames * 2, nominalTicks * 2));
    ASSERT_TRUE(locked.observationApplied);
    EXPECT_EQ(locked.decision.phaseErrorTicks, 0);
    EXPECT_TRUE(locked.decision.locked);
    EXPECT_TRUE(locked.unmuteActivated);
    EXPECT_FALSE(locked.outputMuted);
    EXPECT_FALSE(tx.writer.IsPcmMuted());

    // The device then advances one tick per frame slower. Feed-forward selects
    // 511, and the feedback takes a quarter of the accumulated 512-tick error
    // over the next 512 frames: a quarter tick per frame.
    const auto correction =
        tx.Apply(Window(generation, 3, windowFrames * 3, nominalTicks * 2 + deviceTicks));
    ASSERT_TRUE(correction.observationApplied);
    ASSERT_TRUE(correction.stepApplied);
    EXPECT_TRUE(correction.decision.feedbackUpdated);
    EXPECT_EQ(correction.decision.measuredStepQ32, 511 * kOneQ32);
    EXPECT_EQ(correction.decision.phaseErrorTicks, -512);
    EXPECT_EQ(correction.decision.stepQ32, 511 * kOneQ32 - kOneQ32 / 4);
    EXPECT_FALSE(correction.decision.hardResyncRequired);
    // Only the hard-resync valve drops the lock.
    EXPECT_TRUE(correction.decision.locked);
    EXPECT_FALSE(correction.outputMuted);

    for (uint32_t packet = 0; tx.engine.Counters().dataPacketsPrepared.load() < 64; ++packet) {
        ASSERT_LT(packet, 128u);
        ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(packet, DataTiming(1000)),
                  TxSlotPrepareResult::kPrepared);
    }

    // The stamper reports exactly what it executed, and subtracting that TX
    // movement leaves three quarters of the error standing: -384 of the -512
    // ticks. The step tracks it down by a further quarter.
    const auto settled =
        tx.Apply(Window(generation, 4, windowFrames * 4, nominalTicks * 2 + deviceTicks * 2));
    ASSERT_TRUE(settled.observationApplied);
    ASSERT_TRUE(settled.stepApplied);
    EXPECT_TRUE(settled.decision.feedbackUpdated);
    EXPECT_EQ(settled.decision.phaseErrorTicks, -384);
    EXPECT_EQ(settled.decision.stepQ32, 511 * kOneQ32 - (3 * kOneQ32) / 16);

    const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_TRUE(telemetry.configured);
    EXPECT_TRUE(telemetry.seeded);
    EXPECT_EQ(telemetry.decisions, 4u);
    EXPECT_TRUE(telemetry.locked);
    EXPECT_FALSE(telemetry.outputMuted);
    // The acquisition mute is the only one a clean run reports.
    EXPECT_EQ(telemetry.muteTransitions, 1u);
    EXPECT_EQ(telemetry.unmuteTransitions, 1u);
    // 512 frames at a quarter tick below the measured 511, against a nominal
    // 512: (511 - 0.25 - 512) * 512 = -640 ticks of executed TX movement.
    EXPECT_EQ(telemetry.txCorrectionQ32, -640 * kOneQ32);
    EXPECT_EQ(telemetry.phaseErrorTicks, -384);
    EXPECT_EQ(telemetry.stepQ32, 511 * kOneQ32 - (3 * kOneQ32) / 16);
    EXPECT_EQ(telemetry.nominalStepQ32, 512 * kOneQ32);
    EXPECT_EQ(telemetry.hardResyncRequests, 0u);
}

// The same loop at 96 kHz: decisions one 1024-frame interval apart, and the
// step follows the device's own period at that rate.
TEST(MotuTxTimingStamperTests, StepFollowsTheDeviceRateAtTheConfiguredRate) {
    MotuTxTimingStamper stamper{};
    ASSERT_TRUE(stamper.Configure(4, 96000, kDefaultPresentationLeadTicks, kRelativeLoop));
    constexpr uint64_t interval = 1024;
    ASSERT_EQ(stamper.DecisionIntervalFrames(), interval);
    constexpr uint64_t generation = 9;

    ASSERT_TRUE(stamper.ApplyServoInput(Window(generation, 1, interval, interval * 256))
                    .observationApplied);
    // Half an interval later: not yet a decision, and not a drop either.
    const auto early = stamper.ApplyServoInput(
        Window(generation, 2, interval + interval / 2, (interval + interval / 2) * 256));
    EXPECT_FALSE(early.observationApplied);
    const auto locked =
        stamper.ApplyServoInput(Window(generation, 3, interval * 2, interval * 2 * 256));
    ASSERT_TRUE(locked.observationApplied);
    EXPECT_TRUE(locked.decision.locked);

    // The device slows to 255 ticks per frame: the measured step follows it.
    const auto slower = stamper.ApplyServoInput(
        Window(generation, 4, interval * 3, interval * 2 * 256 + interval * 255));
    ASSERT_TRUE(slower.observationApplied);
    EXPECT_EQ(slower.decision.measuredStepQ32, 255 * kOneQ32);
    EXPECT_LT(slower.decision.stepQ32, 255 * kOneQ32);
    EXPECT_EQ(stamper.StepQ32(), slower.decision.stepQ32);
}

TEST(MotuTxTimingStamperTests, RejectsInvalidStaleDuplicateAndTooYoungRxState) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());

    MotuSphServoRuntimeInput input = Window(3, 1, 512, 512 * 512);
    ASSERT_TRUE(tx.Apply(input).observationApplied);
    const auto baseline = tx.stamper.ServoTelemetrySnapshot();

    input.valid = false;
    EXPECT_FALSE(tx.Apply(input).observationApplied);
    input.valid = true;
    // A sample of a generation the control block has left behind.
    input.expectedStreamGeneration = 4;
    EXPECT_FALSE(tx.Apply(input).observationApplied);
    input.expectedStreamGeneration = 3;
    // The same sample again.
    EXPECT_FALSE(tx.Apply(input).observationApplied);

    // Advanced, but by less than a decision interval.
    input.bridgeUpdates = 2;
    input.rxFrames += 256;
    input.rxTicks += 256 * 511;
    EXPECT_FALSE(tx.Apply(input).observationApplied);

    const auto afterRejected = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_EQ(afterRejected.decisions, baseline.decisions);
    EXPECT_EQ(afterRejected.stepQ32, baseline.stepQ32);
    EXPECT_EQ(afterRejected.phaseErrorTicks, baseline.phaseErrorTicks);
}

// The first absolute phase re-references a loop whose reference was built
// long before it was available. The setpoint is zero here: none is configured.
// The 828mk3 at 48 kHz, armed as the producer arms it: the loop holds the
// model's setpoint (+510) once `rel` arrives.
TEST(MotuTxTimingStamperTests, ReReferencesWhenAbsolutePhaseFirstArrives) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());
    ASSERT_TRUE(tx.stamper.HoldsAbsolutePhase());
    const int64_t setpoint = tx.stamper.AbsolutePhaseSetpointTicks();
    ASSERT_EQ(setpoint, 510);

    constexpr uint64_t generation = 11;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    constexpr int64_t relTicks = 900;

    auto input = [](uint64_t updates, uint64_t frames, int64_t ticks, bool haveRel, int64_t rel) {
        auto in = Window(generation, updates, frames, ticks);
        in.haveRelPhase = haveRel;
        in.relPhaseTicks = rel;
        in.relPhaseCenterTicks = rel;
        return in;
    };

    const auto bare = tx.Apply(input(1, windowFrames, nominalTicks, false, 0));
    ASSERT_TRUE(bare.observationApplied);
    EXPECT_TRUE(bare.decision.phaseReferenceReset);
    EXPECT_EQ(bare.decision.phaseErrorTicks, 0);

    const auto stillBare = tx.Apply(input(2, windowFrames * 2, nominalTicks * 2, false, 0));
    ASSERT_TRUE(stillBare.observationApplied);
    EXPECT_FALSE(stillBare.decision.phaseReferenceReset);
    ASSERT_TRUE(stillBare.decision.locked);

    // First absolute phase: re-reference and seed the error, setpoint - rel.
    // The seed does not drop the lock.
    const auto acquired = tx.Apply(input(3, windowFrames * 3, nominalTicks * 3, true, relTicks));
    ASSERT_TRUE(acquired.observationApplied);
    EXPECT_TRUE(acquired.absolutePhaseAcquired);
    EXPECT_TRUE(acquired.decision.phaseReferenceReset);
    EXPECT_EQ(acquired.decision.referenceCause, MotuSphReferenceCause::kAbsolutePhaseSeed);
    EXPECT_EQ(acquired.decision.phaseErrorTicks, setpoint - relTicks);
    EXPECT_TRUE(acquired.decision.locked);
    EXPECT_FALSE(acquired.outputMuted);
    EXPECT_FALSE(acquired.muteActivated);

    // Acquisition is an edge, not a level: later samples track.
    const auto tracking =
        tx.Apply(input(4, windowFrames * 4, nominalTicks * 4, true, relTicks + 2));
    ASSERT_TRUE(tracking.observationApplied);
    EXPECT_FALSE(tracking.absolutePhaseAcquired);
    EXPECT_FALSE(tracking.decision.phaseReferenceReset);
    EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().seedReReferences, 1u);
}

// The setpoint is per model and per rate: every model holds +510 at 48 kHz,
// and no rate but 48 kHz has one, so there the loop stays relative and a `rel`
// handed to it changes nothing.
TEST(MotuTxTimingStamperTests, HoldsTheSetpointAt48kAndStaysRelativeAtOtherRates) {
    for (const auto& model : kModels) {
        SCOPED_TRACE(model.name);
        EXPECT_EQ(AbsolutePhaseSetpointTicksAtRate(model.runtime.absolutePhaseSetpointTicks48k, 48000),
                  std::optional<int32_t>{510});
        for (const uint32_t rate : {44100u, 88200u, 96000u, 176400u, 192000u}) {
            EXPECT_FALSE(
                AbsolutePhaseSetpointTicksAtRate(model.runtime.absolutePhaseSetpointTicks48k, rate))
                << rate;
        }
    }

    constexpr uint64_t generation = 4;
    constexpr int64_t relTicks = 900;
    const auto feed = [](MotuTxTimingStamper& stamper, uint64_t frames, uint32_t ticksPerFrame) {
        MotuSphServoApplyResult last{};
        for (uint64_t update = 1; update <= 3; ++update) {
            auto in = Window(generation, update, frames * update,
                             static_cast<int64_t>(frames * update * ticksPerFrame));
            in.haveRelPhase = update == 3;
            in.relPhaseTicks = relTicks;
            in.relPhaseCenterTicks = relTicks;
            last = stamper.ApplyServoInput(in);
        }
        return last;
    };

    MotuTxTimingStamper at48k{};
    ASSERT_TRUE(at48k.Configure(4, 48000, kDefaultPresentationLeadTicks,
                                AbsolutePhaseSetpointTicksAtRate(510, 48000)));
    ASSERT_TRUE(at48k.HoldsAbsolutePhase());
    const auto seeded = feed(at48k, 512, 512);
    EXPECT_TRUE(seeded.absolutePhaseAcquired);
    EXPECT_EQ(seeded.decision.phaseErrorTicks, 510 - relTicks);

    MotuTxTimingStamper at96k{};
    ASSERT_TRUE(at96k.Configure(4, 96000, kDefaultPresentationLeadTicks,
                                AbsolutePhaseSetpointTicksAtRate(510, 96000)));
    EXPECT_FALSE(at96k.HoldsAbsolutePhase());
    const auto relative = feed(at96k, 1024, 256);
    ASSERT_TRUE(relative.observationApplied);
    EXPECT_FALSE(relative.absolutePhaseAcquired);
    EXPECT_FALSE(relative.decision.phaseReferenceReset);
    EXPECT_EQ(relative.decision.phaseErrorTicks, 0);
    EXPECT_EQ(at96k.ServoTelemetrySnapshot().seedReReferences, 0u);
}

TEST(MotuTxTimingStamperTests, MutesRepairsPhaseOnceAndUnmutesOnlyWhenSafe) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());

    constexpr uint64_t generation = 11;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    constexpr int64_t overThreshold = int64_t{4 * kTicksPerCycle} + 1;
    const MotuSphServoRuntimeInput reference = Window(generation, 1, windowFrames, nominalTicks);
    // Muted from the start, before any decision exists.
    EXPECT_TRUE(tx.stamper.ServoTelemetrySnapshot().outputMuted);
    EXPECT_TRUE(tx.writer.IsPcmMuted());
    ASSERT_TRUE(tx.Apply(reference).observationApplied);

    auto locking = reference;
    locking.bridgeUpdates = 2;
    locking.rxFrames += windowFrames;
    locking.rxTicks += nominalTicks;
    const auto locked = tx.Apply(locking);
    ASSERT_TRUE(locked.observationApplied);
    ASSERT_TRUE(locked.decision.locked);
    EXPECT_TRUE(locked.unmuteActivated);
    EXPECT_FALSE(locked.outputMuted);

    auto beyond = locking;
    beyond.bridgeUpdates = 3;
    beyond.rxFrames += windowFrames;
    beyond.rxTicks += nominalTicks + overThreshold;
    const auto firstCrossing = tx.Apply(beyond);
    ASSERT_TRUE(firstCrossing.observationApplied);
    ASSERT_TRUE(firstCrossing.stepApplied);
    EXPECT_TRUE(firstCrossing.decision.hardResyncRequired);
    EXPECT_TRUE(firstCrossing.hardResyncStarted);
    EXPECT_EQ(firstCrossing.decision.phaseErrorTicks, overThreshold);
    EXPECT_EQ(firstCrossing.decision.stepQ32, firstCrossing.decision.measuredStepQ32);
    EXPECT_TRUE(firstCrossing.muteActivated);
    EXPECT_TRUE(firstCrossing.phaseRepairApplied);
    EXPECT_TRUE(firstCrossing.outputMuted);
    EXPECT_FALSE(firstCrossing.decision.locked);
    EXPECT_TRUE(MotuSphHardResyncEventOccurred(firstCrossing));
    const auto afterCrossing = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_EQ(afterCrossing.hardResyncRequests, 1u);
    EXPECT_EQ(afterCrossing.phaseRepairs, 1u);
    EXPECT_EQ(afterCrossing.muteTransitions, 2u);
    EXPECT_EQ(afterCrossing.unmuteTransitions, 1u);

    // The repair happened before the first packet's seed and is folded into
    // it. While muted, a non-zero host buffer still produces PCM zero, as
    // DATA, without disturbing the SPH.
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(1000)),
              TxSlotPrepareResult::kPrepared);
    EXPECT_TRUE(tx.provider.published.isData);
    EXPECT_EQ(tx.stamper.LastFirstSph(), SphAt(1000 + kLead + overThreshold));

    const uint32_t channels = tx.txConfig.pcmChannels;
    std::vector<float> host(16 * channels, 0.0f);
    for (uint32_t frame = 0; frame < 16; ++frame) {
        host[frame * channels] = 1.0f;
    }
    tx.engine.FillFromHostOutput({host.data(), 0, 8, 16, channels}, 0);
    const size_t mainAt = tx.HostChannelZeroAt(0);
    EXPECT_EQ(tx.provider.bytes[mainAt], 0u);
    EXPECT_EQ(tx.provider.bytes[mainAt + 1], 0u);
    EXPECT_EQ(tx.provider.bytes[mainAt + 2], 0u);
    EXPECT_EQ(tx.writer.Counters().framesMuted.load(), 8u);
    EXPECT_EQ(tx.stamper.LastFirstSph(), tx.BlockSph(0));

    // Remaining above the threshold is the same crossing: stay muted and do
    // not repair again.
    constexpr int64_t nominalStepQ32 = 512 * kOneQ32;
    const int64_t txCorrectionAfterPacketQ32 =
        overThreshold * kOneQ32 + 8 * (firstCrossing.decision.stepQ32 - nominalStepQ32);
    const int64_t heldRxDeviationTicks =
        (txCorrectionAfterPacketQ32 + overThreshold * kOneQ32 + kOneQ32 - 1) / kOneQ32;
    auto stillBeyond = beyond;
    stillBeyond.bridgeUpdates = 4;
    stillBeyond.rxFrames = windowFrames * 4;
    stillBeyond.rxTicks = nominalTicks * 4 + heldRxDeviationTicks;
    const auto held = tx.Apply(stillBeyond);
    ASSERT_TRUE(held.observationApplied);
    EXPECT_TRUE(held.decision.hardResyncRequired);
    EXPECT_FALSE(held.hardResyncStarted);
    EXPECT_FALSE(held.muteActivated);
    EXPECT_FALSE(held.phaseRepairApplied);
    EXPECT_TRUE(held.outputMuted);
    EXPECT_FALSE(MotuSphHardResyncEventOccurred(held));
    const auto afterHeld = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_EQ(afterHeld.hardResyncRequests, afterCrossing.hardResyncRequests);
    EXPECT_EQ(afterHeld.phaseRepairs, afterCrossing.phaseRepairs);
    EXPECT_EQ(afterHeld.muteTransitions, afterCrossing.muteTransitions);
    EXPECT_EQ(afterHeld.unmuteTransitions, afterCrossing.unmuteTransitions);

    // Once the phase is back inside the lock threshold, unmute.
    auto safe = stillBeyond;
    safe.bridgeUpdates = 5;
    safe.rxFrames = windowFrames * 5;
    safe.rxTicks = nominalTicks * 5 + txCorrectionAfterPacketQ32 / kOneQ32;
    const auto recovered = tx.Apply(safe);
    ASSERT_TRUE(recovered.observationApplied);
    EXPECT_FALSE(recovered.decision.hardResyncRequired);
    EXPECT_TRUE(recovered.decision.locked);
    EXPECT_TRUE(recovered.unmuteActivated);
    EXPECT_FALSE(recovered.outputMuted);
    EXPECT_TRUE(MotuSphHardResyncEventOccurred(recovered));

    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(1, DataTiming(1000)),
              TxSlotPrepareResult::kPrepared);
    tx.engine.FillFromHostOutput({host.data(), 8, 8, 16, channels}, 0);
    EXPECT_EQ(tx.provider.bytes[mainAt], 0x7fu);
    EXPECT_EQ(tx.provider.bytes[mainAt + 1], 0xffu);
    EXPECT_EQ(tx.provider.bytes[mainAt + 2], 0xffu);

    const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_FALSE(telemetry.hardResyncRequired);
    EXPECT_EQ(telemetry.hardResyncRequests, 1u);
    EXPECT_EQ(telemetry.phaseRepairs, 1u);
    EXPECT_EQ(telemetry.muteTransitions, 2u);
    EXPECT_EQ(telemetry.unmuteTransitions, 2u);
    EXPECT_FALSE(telemetry.outputMuted);
    EXPECT_TRUE(telemetry.locked);
    EXPECT_EQ(telemetry.decisions, 5u);
}

// The repair folds into the clock's applied correction at once, so on the
// next decision the error is back to zero without any compensating input: the
// mute lasts one decision.
TEST(MotuTxTimingStamperTests, HardResyncMuteLastsOneDecisionWithoutAnyCompensation) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());

    constexpr uint64_t generation = 11;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    constexpr int64_t overThreshold = int64_t{4 * kTicksPerCycle} + 1;

    auto window = [&](uint64_t index, int64_t extraTicks) {
        return Window(generation, index, windowFrames * index,
                      nominalTicks * static_cast<int64_t>(index) + extraTicks);
    };

    ASSERT_TRUE(tx.Apply(window(1, 0)).observationApplied);
    const auto locked = tx.Apply(window(2, 0));
    ASSERT_TRUE(locked.decision.locked);
    ASSERT_FALSE(locked.outputMuted);

    const auto crossing = tx.Apply(window(3, overThreshold));
    ASSERT_TRUE(crossing.decision.hardResyncRequired);
    ASSERT_TRUE(crossing.phaseRepairApplied);
    ASSERT_TRUE(crossing.outputMuted);
    ASSERT_FALSE(crossing.decision.locked);

    uint32_t decisionsMuted = 0;
    MotuSphServoApplyResult step{};
    for (uint64_t index = 4; index <= 12; ++index) {
        step = tx.Apply(window(index, overThreshold));
        ASSERT_TRUE(step.observationApplied);
        if (!step.outputMuted) {
            break;
        }
        ++decisionsMuted;
    }

    EXPECT_EQ(decisionsMuted, 0u);
    EXPECT_TRUE(step.unmuteActivated);
    EXPECT_TRUE(step.decision.locked);
    EXPECT_FALSE(step.outputMuted);
    EXPECT_FALSE(step.decision.hardResyncRequired);
    EXPECT_FALSE(tx.writer.IsPcmMuted());

    const int64_t residual = step.decision.phaseErrorTicks < 0 ? -step.decision.phaseErrorTicks
                                                               : step.decision.phaseErrorTicks;
    EXPECT_LT(residual, (6144 * kOneQ32 / 100) / kOneQ32 + 1);

    const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_EQ(telemetry.hardResyncRequests, 1u);
    EXPECT_EQ(telemetry.phaseRepairs, 1u);
    EXPECT_EQ(telemetry.muteTransitions, 2u);
    EXPECT_EQ(telemetry.unmuteTransitions, 2u);
}

// The valve is symmetric in sign, and the drop gate discards a regression
// before the servo sees it -- so the servo's own regression counters stay
// unreachable in production.
TEST(MotuTxTimingStamperTests, ANewGenerationMutesForAFreshAcquisition) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());
    const auto first = tx.Apply(Window(3, 1, 512, 512 * 512));
    ASSERT_TRUE(first.observationApplied);
    // The first generation of a start is no restart: the start muted already.
    EXPECT_FALSE(first.acquisitionRestarted);
    ASSERT_TRUE(tx.Apply(Window(3, 2, 1024, 2 * 512 * 512)).decision.locked);
    ASSERT_FALSE(tx.writer.IsPcmMuted());

    const auto restarted = tx.Apply(Window(4, 1, 512, 512 * 512));
    EXPECT_TRUE(restarted.observationApplied);
    EXPECT_TRUE(restarted.acquisitionRestarted);
    EXPECT_TRUE(restarted.decision.phaseReferenceReset);
    EXPECT_TRUE(restarted.outputMuted);
    EXPECT_TRUE(tx.writer.IsPcmMuted());
    EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().muteTransitions, 2u);
}

// The telemetry's rate figure: parts per billion against nominal, positive
// for a shorter step (a faster clock).
TEST(MotuTxTimingStamperTests, ReadinessFollowsTheLockOfTheCurrentGeneration) {
    MotuTx tx;
    ASSERT_TRUE(tx.Arm());
    constexpr uint64_t generation = 5;
    constexpr int64_t nominalTicks = 512 * 512;
    EXPECT_EQ(tx.ready.load(), 0u);

    // The RX clock arrives and is referenced: not locked yet.
    ASSERT_TRUE(tx.Apply(Window(generation, 1, 512, nominalTicks)).observationApplied);
    EXPECT_EQ(tx.ready.load(), 0u);

    // Locked, but no DATA packet has seeded the clock yet.
    ASSERT_TRUE(tx.Apply(Window(generation, 2, 1024, 2 * nominalTicks)).decision.locked);
    EXPECT_EQ(tx.ready.load(), 0u);

    // The first DATA packet seeds the clock; the next servo call reports ready.
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(1000)),
              TxSlotPrepareResult::kPrepared);
    (void)tx.Apply(Window(generation, 2, 1024, 2 * nominalTicks));
    EXPECT_EQ(tx.ready.load(), 1u);

    // The control block moved on to a new start while this stream's sample is
    // still the latest: a lock from the old generation is not readiness.
    auto stale = Window(generation, 3, 1536, 3 * nominalTicks);
    stale.expectedStreamGeneration = generation + 1;
    EXPECT_FALSE(tx.Apply(stale).observationApplied);
    EXPECT_EQ(tx.ready.load(), 0u);
    ASSERT_TRUE(tx.Apply(Window(generation, 3, 1536, 3 * nominalTicks)).observationApplied);
    EXPECT_EQ(tx.ready.load(), 1u);

    // A hard resync takes it away until the loop relocks.
    constexpr int64_t overThreshold = int64_t{4 * kTicksPerCycle} + 1;
    ASSERT_TRUE(tx.Apply(Window(generation, 4, 2048, 4 * nominalTicks + overThreshold))
                    .decision.hardResyncRequired);
    EXPECT_EQ(tx.ready.load(), 0u);

    // And a new start clears it.
    tx.stamper.ResetForStart();
    EXPECT_EQ(tx.ready.load(), 0u);
}

} // namespace
