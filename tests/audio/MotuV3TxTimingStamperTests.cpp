// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The V3 SPH clock and servo behind the ITxDeviceTimingStamper seam.
//
// The engine is the production DiceTxStreamEngine, the stamper is bound to it
// as the producer binds it, and `Mk3Tx::Apply` mirrors the producer's hook:
// feed the servo, then put the payload writer on the gate. The producer hands
// the stamper the packet's transmit time, and the stamper adds the
// presentation lead itself.

#include "Audio/DriverKit/Config/MOTU/MOTU828Mk3Profile.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Protocols/MOTU/MotuSphHardResyncGate.hpp"
#include "Audio/Wire/AMDTP/MotuSphQ32Accumulator.hpp"
#include "Audio/Wire/AMDTP/MotuV3WireFormat.hpp"
#include "Audio/Wire/MOTU/MotuV3PayloadWriter.hpp"
#include "Audio/Wire/MOTU/MotuV3TxTimingStamper.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Audio::TxTimingStampResult;
using ASFW::Audio::Wire::kMotuSphDropBridgeNotAdvanced;
using ASFW::Audio::Wire::kMotuSphDropBridgeRegressed;
using ASFW::Audio::Wire::kMotuSphDropRxFramesNotAdvanced;
using ASFW::Audio::Wire::kMotuSphDropRxTicksNotAdvanced;
using ASFW::Audio::Wire::kMotuSphDropRxTicksRegressed;
using ASFW::Audio::Wire::MotuSphHardResyncEventOccurred;
using ASFW::Audio::Wire::MotuSphServoApplyResult;
using ASFW::Audio::Wire::MotuSphServoRuntimeInput;
using ASFW::Audio::Wire::MotuV3PayloadWriter;
using ASFW::Audio::Wire::MotuV3TxTimingStamper;
using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;

constexpr int64_t kOneQ32 = MotuSphQ32Accumulator::kOneQ32;
constexpr int64_t kLead = MotuV3Wire::kPresentationLeadTicks;

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

// The 828 Mk3 TX path as ArmPrimaryTxProducer assembles it.
struct Mk3Tx final {
    ASFW::Isoch::Audio::MOTU::Profiles::MOTU828Mk3Profile profile{};
    ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
    DiceTxStreamEngine engine{};
    MotuV3TxTimingStamper stamper{};
    MotuV3PayloadWriter writer{};
    MotuSlotProvider provider{};

    [[nodiscard]] bool Arm() {
        if (!profile.BuildDefaultTxStreamConfig(txConfig) || !engine.Configure(profile, txConfig) ||
            !stamper.Configure(txConfig)) {
            return false;
        }
        writer.Configure({.pcmChunks = txConfig.pcmChannels,
                          .sourceChannelOffset = txConfig.sourceChannelOffset,
                          .ports = profile.TxStreamPolicy().motuPlaybackPorts});
        writer.SetPcmMuted(stamper.IsOutputMuted());
        writer.BindTimeline(&engine.Timeline());
        engine.SetPayloadWriter(&writer);
        engine.BindTimingStamper(&stamper);
        engine.BindSlotProvider(&provider);
        engine.ResetForStart(0, 0);
        return true;
    }

    // The producer's hook (ApplyMotuV3ServoInput).
    MotuSphServoApplyResult Apply(const MotuSphServoRuntimeInput& input) {
        const auto result = stamper.ApplyServoInput(input);
        writer.SetPcmMuted(stamper.IsOutputMuted());
        return result;
    }

    // One SPH per block of the packet the provider holds.
    [[nodiscard]] uint32_t BlockSph(uint32_t block) const {
        return ReadBE32(provider.bytes.data() + 8 + block * txConfig.dbs * 4);
    }
};

// A DATA packet as the producer's replay branch builds it for V3: a replayed
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
        (MotuV3Wire::NormalizeTicks(transmitTicks) / MotuV3Wire::kTicksPerCycle) %
        MotuV3Wire::kCyclesPerSecond);
    timing.transmitTicks = transmitTicks;
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

// ---------------------------------------------------------------------------
// The SPH clock on the wire.

TEST(MotuV3TxTimingStamperTests, StampsFreeRunningSphFromTheFirstTransmitTimeAndNeverReseeds) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());
    EXPECT_TRUE(tx.engine.IsSytUnaware());

    // The anchor sits one cycle before a second boundary with a sub-cycle
    // phase, so the seed both keeps that phase and wraps the SPH second.
    const int64_t transmitTicks = static_cast<int64_t>(
        MotuV3Wire::kTicksPerSecond * 7 + 7999 * MotuV3Wire::kTicksPerCycle + 2800);
    const int64_t seed = transmitTicks + kLead;
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(transmitTicks)),
              TxSlotPrepareResult::kPrepared);
    ASSERT_TRUE(tx.provider.published.isData);
    EXPECT_EQ(tx.provider.published.syt, 0xFFFFu);
    EXPECT_EQ(tx.stamper.LastFirstSph(), MotuV3Wire::EncodeSph(seed));
    for (uint32_t block = 0; block < 8; ++block) {
        EXPECT_EQ(tx.BlockSph(block), MotuV3Wire::EncodeSph(seed + block * 512));
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
    EXPECT_EQ(tx.BlockSph(0), MotuV3Wire::EncodeSph(seed + 8 * 512));
    EXPECT_EQ(tx.stamper.LastFirstSph(), tx.BlockSph(0));
    EXPECT_EQ(tx.provider.published.dbc, 8u);

    // A new start seeds again.
    tx.engine.ResetForStart(0, 0);
    tx.stamper.ResetForStart();
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(1000)),
              TxSlotPrepareResult::kPrepared);
    EXPECT_EQ(tx.BlockSph(0), MotuV3Wire::EncodeSph(1000 + kLead));
}

// The packetizer arms every DATA packet with AM824 silence (0x40000000 per quadlet).
// In a V3 block that lands in the SPH quadlet, both message chunks and a byte
// of most PCM chunks, and stays wherever the payload writer does not reach --
// the top byte of every fourth chunk is a half-scale offset. The V3 layout
// keeps the zero the packetizer clears first.
TEST(MotuV3TxTimingStamperTests, ArmsTheV3PayloadWithZeroNotAm824Silence) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());
    tx.provider.bytes.fill(0xEE);
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(0)),
              TxSlotPrepareResult::kPrepared);
    const uint32_t blockBytes = tx.txConfig.dbs * 4;
    for (uint32_t block = 0; block < 8; ++block) {
        for (uint32_t byte = MotuV3Wire::kSphBytes; byte < blockBytes; ++byte) {
            ASSERT_EQ(tx.provider.bytes[8 + block * blockBytes + byte], 0u)
                << "block " << block << " byte " << byte;
        }
    }
}

// The packetizer turns kTimingUnavailable into NO-DATA, which on V3 would
// change the cadence the device sees. A packet before the clock has a seed
// goes out as DATA with SPH zero.
TEST(MotuV3TxTimingStamperTests, NeverRevertsDataWhenTheClockHasNoTransmitTimeYet) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());
    tx.provider.bytes.fill(0xEE);
    AmdtpTimingState untimed = DataTiming(0);
    untimed.transmitCycleValid = false;
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, untimed), TxSlotPrepareResult::kPrepared);
    EXPECT_TRUE(tx.provider.published.isData);
    EXPECT_EQ(tx.engine.Counters().timingUnavailableReverts.load(), 0u);
    for (uint32_t block = 0; block < 8; ++block) {
        EXPECT_EQ(tx.BlockSph(block), 0u);
    }

    // And directly at the seam: never kTimingUnavailable, kNotApplicable only
    // for a packet with nothing to stamp.
    PreparedTxPacket data = tx.provider.published;
    const TxPacketSlotView slot{0, tx.provider.bytes.data(),
                                static_cast<uint32_t>(tx.provider.bytes.size())};
    EXPECT_EQ(tx.stamper.StampPacket(slot, data, untimed), TxTimingStampResult::kOk);
    PreparedTxPacket empty{};
    EXPECT_EQ(tx.stamper.StampPacket(slot, empty, untimed), TxTimingStampResult::kNotApplicable);
}

TEST(MotuV3TxTimingStamperTests, ConfigureRefusesGeometriesWithoutAServoPolicy) {
    ASFW::Isoch::Audio::MOTU::Profiles::MOTU828Mk3Profile profile;
    ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(txConfig));
    MotuV3TxTimingStamper stamper{};
    ASSERT_TRUE(stamper.Configure(txConfig));
    EXPECT_TRUE(stamper.IsConfigured());
    EXPECT_TRUE(stamper.IsOutputMuted());  // the acquisition mute, from the start

    auto at44k = txConfig;
    at44k.sampleRate = 44100;
    EXPECT_FALSE(MotuV3TxTimingStamper{}.Configure(at44k));
    auto shortPackets = txConfig;
    shortPackets.framesPerDataPacket = 6;
    EXPECT_FALSE(MotuV3TxTimingStamper{}.Configure(shortPackets));
}

// ---------------------------------------------------------------------------
// The accumulator itself.

// The accumulator carries a modular one-second phase, because MOTU's SPH has
// no seconds field, so a four-second run crosses the wrap four times. What has
// to hold across those crossings is the position on the circle, not a
// monotonic total -- hence every comparison below is taken the short way round.
TEST(MotuV3TxTimingStamperTests, AccumulatorKeepsFractionalTimeAtAllSixRates) {
    constexpr std::array<uint32_t, 6> kSupportedRates = {
        44100, 48000, 88200, 96000, 176400, 192000,
    };
    constexpr int64_t kTestSeconds = 4;
    constexpr int64_t kDomainTicks = static_cast<int64_t>(MotuV3Wire::kTickDomain);
    constexpr int64_t kDomainQ32 = MotuSphQ32Accumulator::kTickDomainQ32;
    constexpr int64_t kExpectedTicks =
        static_cast<int64_t>(MotuV3Wire::kTicksPerSecond) * kTestSeconds;

    const auto shortestLag = [](int64_t expected, int64_t actual, int64_t domain) {
        return ((expected - actual) % domain + domain) % domain;
    };

    for (const uint32_t rate : kSupportedRates) {
        SCOPED_TRACE(rate);
        MotuSphQ32Accumulator clock{};
        ASSERT_TRUE(clock.Configure(rate));
        ASSERT_TRUE(clock.SeedOnce(0));
        EXPECT_EQ(clock.StepQ32(), MotuSphQ32Accumulator::NominalStepQ32ForRate(rate));

        const uint32_t frames = rate * kTestSeconds;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            clock.Advance();
        }

        // The step is floor(Q32.32), so accumulated truncation stays below one
        // Q32 unit per frame. Even the 44.1 kHz family remains within one
        // integer SPH tick after four seconds.
        const int64_t expectedPhaseQ32 = (kExpectedTicks % kDomainTicks) * kOneQ32;
        EXPECT_LT(shortestLag(expectedPhaseQ32, clock.PhaseQ32(), kDomainQ32),
                  static_cast<int64_t>(frames));
        EXPECT_LE(shortestLag(kExpectedTicks, clock.CurrentTicks(), kDomainTicks), 1);

        // Wrapping is not steering: a free-running clock reports no applied
        // correction however many times its phase crossed a second boundary.
        EXPECT_EQ(clock.AppliedCorrectionQ32(), 0);
    }
}

TEST(MotuV3TxTimingStamperTests, AccumulatorRejectsUnsupportedRateAndInvalidSteps) {
    MotuSphQ32Accumulator clock{};
    EXPECT_FALSE(clock.Configure(32000));
    EXPECT_FALSE(clock.SetStepQ32(1));

    ASSERT_TRUE(clock.Configure(48000));
    EXPECT_FALSE(clock.SetStepQ32(0));
    EXPECT_FALSE(clock.SetStepQ32(-1));
    EXPECT_FALSE(clock.SetStepQ32(MotuSphQ32Accumulator::kTickDomainQ32));
    EXPECT_EQ(clock.StepQ32(), 512 * kOneQ32);
}

TEST(MotuV3TxTimingStamperTests, AccumulatorAppliesPreSeedRepairAndAccountsForIt) {
    MotuSphQ32Accumulator clock{};
    ASSERT_TRUE(clock.Configure(48000));

    constexpr int64_t correctionQ32 = -2 * kOneQ32;
    ASSERT_TRUE(clock.ApplyPhaseCorrectionQ32(correctionQ32));
    EXPECT_EQ(clock.AppliedCorrectionQ32(), correctionQ32);
    ASSERT_TRUE(clock.SeedOnce(1));
    EXPECT_EQ(clock.CurrentTicks(), MotuV3Wire::kTickDomain - 1);

    clock.Reset();
    EXPECT_EQ(clock.AppliedCorrectionQ32(), 0);
    ASSERT_TRUE(clock.SeedOnce(1));
    EXPECT_EQ(clock.CurrentTicks(), 1);
}

// A packet the timeline refuses never reaches the stamper, so only frames that
// were actually stamped count as executed TX correction.
TEST(MotuV3TxTimingStamperTests, AppliedCorrectionCountsOnlyStampedFramesAndResetClearsIt) {
    Mk3Tx tx;
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

TEST(MotuV3TxTimingStamperTests, ClosesRxMinusTxPhaseWithOneContinuousServo) {
    Mk3Tx tx;
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
    // A reference is not a lock, and the output is muted until one
    // arrives.
    EXPECT_FALSE(reference.decision.locked);
    EXPECT_TRUE(reference.outputMuted);
    EXPECT_TRUE(tx.writer.IsPcmMuted());

    // One clean window locks the loop, which is what puts the gain below into
    // the locked stage.
    const auto locked = tx.Apply(Window(generation, 2, windowFrames * 2, nominalTicks * 2));
    ASSERT_TRUE(locked.observationApplied);
    EXPECT_EQ(locked.decision.phaseErrorTicks, 0);
    EXPECT_TRUE(locked.decision.locked);
    EXPECT_TRUE(locked.unmuteActivated);
    EXPECT_FALSE(locked.outputMuted);
    EXPECT_FALSE(tx.writer.IsPcmMuted());

    // The device then advances one tick/frame slower. Feed-forward selects 511,
    // and the closed-loop term takes a quarter of the accumulated 512-tick
    // error over the next 512 frames -- a quarter tick per frame, not a whole
    // one (P3 stopped deadbeat correction).
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
    // movement leaves three quarters of the phase error standing: -384 of the
    // original -512 ticks. The step tracks it down by a further quarter.
    const auto settled =
        tx.Apply(Window(generation, 4, windowFrames * 4, nominalTicks * 2 + deviceTicks * 2));
    ASSERT_TRUE(settled.observationApplied);
    ASSERT_TRUE(settled.stepApplied);
    EXPECT_TRUE(settled.decision.feedbackUpdated);
    EXPECT_EQ(settled.decision.phaseErrorTicks, -384);
    EXPECT_EQ(settled.decision.stepQ32, 511 * kOneQ32 - (3 * kOneQ32) / 16);

    const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_TRUE(telemetry.configured);
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
    EXPECT_EQ(telemetry.hardResyncRequests, 0u);
}

TEST(MotuV3TxTimingStamperTests, RejectsInvalidStaleDuplicateAndTooYoungRxState) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());

    MotuSphServoRuntimeInput input = Window(3, 1, 512, 512 * 512);
    ASSERT_TRUE(tx.Apply(input).observationApplied);
    const auto baseline = tx.stamper.ServoTelemetrySnapshot();

    input.valid = false;
    EXPECT_FALSE(tx.Apply(input).observationApplied);
    input.valid = true;
    input.expectedStreamGeneration = 4;
    EXPECT_FALSE(tx.Apply(input).observationApplied);
    input.expectedStreamGeneration = 3;
    EXPECT_FALSE(tx.Apply(input).observationApplied);

    input.bridgeUpdates = 2;
    input.rxFrames += 256;
    input.rxTicks += 256 * 511;
    EXPECT_FALSE(tx.Apply(input).observationApplied);

    const auto afterRejected = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_EQ(afterRejected.decisions, baseline.decisions);
    EXPECT_EQ(afterRejected.stepQ32, baseline.stepQ32);
    EXPECT_EQ(afterRejected.phaseErrorTicks, baseline.phaseErrorTicks);
}

// The stamper carries `rel` across, and the FIRST arrival of it
// re-references a loop whose reference was built long before the conditioner
// had warmed up.
TEST(MotuV3TxTimingStamperTests, ReReferencesWhenAbsolutePhaseFirstArrives) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());

    constexpr uint64_t generation = 11;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    constexpr int64_t relTicks = 900;

    auto Input = [](uint64_t updates, uint64_t frames, int64_t ticks, bool haveRel, int64_t rel) {
        auto in = Window(generation, updates, frames, ticks);
        in.haveRelPhase = haveRel;
        in.relPhaseTicks = rel;
        in.relPhaseCenterTicks = rel;
        return in;
    };

    const auto bare = tx.Apply(Input(1, windowFrames, nominalTicks, false, 0));
    ASSERT_TRUE(bare.observationApplied);
    EXPECT_TRUE(bare.decision.phaseReferenceReset);
    EXPECT_EQ(bare.decision.phaseErrorTicks, 0);

    const auto stillBare = tx.Apply(Input(2, windowFrames * 2, nominalTicks * 2, false, 0));
    ASSERT_TRUE(stillBare.observationApplied);
    EXPECT_FALSE(stillBare.decision.phaseReferenceReset);
    EXPECT_EQ(stillBare.decision.phaseErrorTicks, 0);
    ASSERT_TRUE(stillBare.decision.locked);

    // First `rel`: re-reference and seed against the measured setpoint. The
    // seed does not drop the lock.
    const auto acquired = tx.Apply(Input(3, windowFrames * 3, nominalTicks * 3, true, relTicks));
    ASSERT_TRUE(acquired.observationApplied);
    EXPECT_TRUE(acquired.decision.phaseReferenceReset);
    EXPECT_EQ(acquired.decision.phaseErrorTicks, 510 - relTicks);
    EXPECT_TRUE(acquired.decision.locked);
    EXPECT_FALSE(acquired.outputMuted);
    EXPECT_FALSE(acquired.muteActivated);

    // Acquisition is an edge, not a level: later samples track.
    const auto tracking =
        tx.Apply(Input(4, windowFrames * 4, nominalTicks * 4, true, relTicks + 2));
    ASSERT_TRUE(tracking.observationApplied);
    EXPECT_FALSE(tracking.decision.phaseReferenceReset);
}

TEST(MotuV3TxTimingStamperTests, WithoutARelPhaseKeepsTheHistoricalLoop) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());
    const auto reference = tx.Apply(Window(5, 1, 512, 512 * 512));
    ASSERT_TRUE(reference.observationApplied);
    EXPECT_TRUE(reference.decision.phaseReferenceReset);
    EXPECT_EQ(reference.decision.phaseErrorTicks, 0);
}

TEST(MotuV3TxTimingStamperTests, HardResyncGateMutesWhileUnlockedAndRepairsOncePerCrossing) {
    // The gate owns no threshold: it turns the servo's lock state
    // into muting, and the hard-resync edge into the one-shot repair.
    ASFW::Audio::MOTU::MotuSphHardResyncGate gate{};
    gate.Reset(true);
    EXPECT_TRUE(gate.IsMuted());

    auto action = gate.Update(false, false);
    EXPECT_TRUE(action.muted);
    EXPECT_FALSE(action.unmute);
    EXPECT_FALSE(action.phaseRepairRequired);

    action = gate.Update(false, true);
    EXPECT_TRUE(action.unmute);
    EXPECT_FALSE(action.muted);

    // A crossing: muting is reported together with the repair, ahead of it.
    action = gate.Update(true, false);
    EXPECT_TRUE(action.muteBeforeRepair);
    EXPECT_TRUE(action.phaseRepairRequired);
    EXPECT_TRUE(action.muted);

    // Still beyond the valve is the same crossing, not a second one.
    action = gate.Update(true, false);
    EXPECT_FALSE(action.muteBeforeRepair);
    EXPECT_FALSE(action.phaseRepairRequired);
    EXPECT_TRUE(action.muted);

    // Back under the valve but not yet locked: the repair re-arms, the mute
    // holds.
    action = gate.Update(false, false);
    EXPECT_FALSE(action.unmute);
    EXPECT_TRUE(action.muted);

    action = gate.Update(false, true);
    EXPECT_TRUE(action.unmute);
    EXPECT_FALSE(action.muted);

    action = gate.Update(true, false);
    EXPECT_TRUE(action.muteBeforeRepair);
    EXPECT_TRUE(action.phaseRepairRequired);

    // A gate nobody armed starts open.
    ASFW::Audio::MOTU::MotuSphHardResyncGate unarmed{};
    EXPECT_FALSE(unarmed.IsMuted());
    unarmed.Reset();
    EXPECT_FALSE(unarmed.IsMuted());
}

TEST(MotuV3TxTimingStamperTests, MutesRepairsPhaseOnceAndUnmutesOnlyWhenSafe) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());

    constexpr uint64_t generation = 11;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    constexpr int64_t overThreshold =
        ASFW::Audio::MOTU::MotuSphClockServo::kHardResyncThresholdTicks + 1;
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
    // it. While the gate is muted, a non-zero host buffer must still produce
    // PCM zero, as DATA, without disturbing the SPH.
    ASSERT_EQ(tx.engine.PrepareNextTransmitSlot(0, DataTiming(1000)),
              TxSlotPrepareResult::kPrepared);
    EXPECT_TRUE(tx.provider.published.isData);
    EXPECT_EQ(tx.stamper.LastFirstSph(), MotuV3Wire::EncodeSph(1000 + kLead + overThreshold));

    std::array<float, 16 * 14> host{};
    for (uint32_t frame = 0; frame < 16; ++frame) {
        host[frame * 14] = 1.0f;
    }
    tx.engine.FillFromHostOutput({host.data(), 0, 8, 16, 14}, 0);
    constexpr size_t mainL = 8 + 4 + 12 * 3;
    EXPECT_EQ(tx.provider.bytes[mainL], 0u);
    EXPECT_EQ(tx.provider.bytes[mainL + 1], 0u);
    EXPECT_EQ(tx.provider.bytes[mainL + 2], 0u);
    EXPECT_EQ(tx.writer.FramesIntentionallyMuted(), 8u);
    EXPECT_EQ(tx.stamper.LastFirstSph(), tx.BlockSph(0));

    // Remaining above the hard threshold is the same crossing: stay muted and
    // do not execute another phase repair.
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
    EXPECT_FALSE(held.muteActivated);
    EXPECT_FALSE(held.phaseRepairApplied);
    EXPECT_TRUE(held.outputMuted);
    EXPECT_FALSE(MotuSphHardResyncEventOccurred(held));
    const auto afterHeld = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_EQ(afterHeld.hardResyncRequests, afterCrossing.hardResyncRequests);
    EXPECT_EQ(afterHeld.phaseRepairs, afterCrossing.phaseRepairs);
    EXPECT_EQ(afterHeld.muteTransitions, afterCrossing.muteTransitions);
    EXPECT_EQ(afterHeld.unmuteTransitions, afterCrossing.unmuteTransitions);

    // Once RX-TX phase returns below the safe band, unmute.
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
    tx.engine.FillFromHostOutput({host.data(), 8, 8, 16, 14}, 0);
    EXPECT_EQ(tx.provider.bytes[mainL], 0x7fu);
    EXPECT_EQ(tx.provider.bytes[mainL + 1], 0xffu);
    EXPECT_EQ(tx.provider.bytes[mainL + 2], 0xffu);

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

// The repair folds into the clock's applied correction immediately, so
// on the next update the error comes back to zero without any compensating
// input: the mute lasts one decision, not a drift-dependent wait.
TEST(MotuV3TxTimingStamperTests, HardResyncMuteLastsOneDecisionWithoutAnyCompensation) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());

    constexpr uint64_t generation = 11;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    constexpr int64_t overThreshold =
        ASFW::Audio::MOTU::MotuSphClockServo::kHardResyncThresholdTicks + 1;

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
    EXPECT_LT(residual, ASFW::Audio::MOTU::MotuSphClockServo::kLockThresholdQ32 / kOneQ32 + 1);

    const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
    EXPECT_EQ(telemetry.hardResyncRequests, 1u);
    EXPECT_EQ(telemetry.phaseRepairs, 1u);
    EXPECT_EQ(telemetry.muteTransitions, 2u);
    EXPECT_EQ(telemetry.unmuteTransitions, 2u);
}

// The valve is symmetric in sign, and the stamper's drop
// gate discards a regression before the servo ever sees it -- so the servo's
// own regression counters stay unreachable in production.
TEST(MotuV3TxTimingStamperTests, ValveIsSymmetricButTheDropGateStopsARegressionBeforeTheServo) {
    constexpr uint64_t generation = 11;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    auto window = [&](uint64_t index, int64_t ticks) {
        return Window(generation, index, windowFrames * index, ticks);
    };

    // Case 1: ten cycles short of nominal -- large NEGATIVE error, valve opens.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        ASSERT_TRUE(tx.Apply(window(1, nominalTicks)).observationApplied);
        ASSERT_TRUE(tx.Apply(window(2, nominalTicks * 2)).decision.locked);

        const auto slow = tx.Apply(window(3, nominalTicks * 3 - 10 * 3072));
        ASSERT_TRUE(slow.observationApplied);
        EXPECT_TRUE(slow.decision.hardResyncRequired);
        EXPECT_TRUE(slow.phaseRepairApplied);
        EXPECT_LT(slow.decision.phaseErrorTicks, 0);
        EXPECT_EQ(slow.decision.referenceCause, ASFW::Audio::MOTU::MotuSphReferenceCause::kNone);
        EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().hardResyncRequests, 1u);
    }

    // Case 2: the counter goes backwards; dropped before the servo runs.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        ASSERT_TRUE(tx.Apply(window(1, nominalTicks)).observationApplied);
        ASSERT_TRUE(tx.Apply(window(2, nominalTicks * 2)).decision.locked);

        const auto regressed = tx.Apply(window(3, nominalTicks));
        EXPECT_FALSE(regressed.observationApplied);
        EXPECT_FALSE(regressed.decision.hardResyncRequired);
        EXPECT_FALSE(regressed.phaseRepairApplied);
        const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
        EXPECT_EQ(telemetry.rxTickRegressions, 0u);
        EXPECT_EQ(telemetry.rxFrameRegressions, 0u);
        EXPECT_EQ(telemetry.hardResyncRequests, 0u);
        EXPECT_EQ(telemetry.phaseRepairs, 0u);
    }

    // Case 3: a stalled bridge is dropped by the same gate.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        ASSERT_TRUE(tx.Apply(window(1, nominalTicks)).observationApplied);
        ASSERT_TRUE(tx.Apply(window(2, nominalTicks * 2)).decision.locked);
        const auto stalled = tx.Apply(window(2, nominalTicks * 2));
        EXPECT_FALSE(stalled.observationApplied);
        EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().bridgeStalls, 0u);
    }
}

// Each dropped observation says which counter rejected it, and a repeat
// is counted apart from a regression.
TEST(MotuV3TxTimingStamperTests, AttributesEveryDroppedObservationToItsCause) {
    constexpr uint64_t generation = 12;
    constexpr uint64_t windowFrames = 512;
    constexpr int64_t nominalTicks = windowFrames * 512;
    auto window = [&](uint64_t index, int64_t ticks) {
        return Window(generation, index, windowFrames * index, ticks);
    };
    auto settle = [&](Mk3Tx& tx) {
        ASSERT_TRUE(tx.Apply(window(1, nominalTicks)).observationApplied);
        ASSERT_TRUE(tx.Apply(window(2, nominalTicks * 2)).observationApplied);
        ASSERT_EQ(tx.stamper.ServoTelemetrySnapshot().observationDrops, 0u);
    };

    // Case 1: a healthy stream never touches these counters.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        settle(tx);
        const auto third = tx.Apply(window(3, nominalTicks * 3));
        EXPECT_TRUE(third.observationApplied);
        EXPECT_EQ(third.dropCauses, 0u);
        const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
        EXPECT_EQ(telemetry.observationDrops, 0u);
        EXPECT_EQ(telemetry.dropBridgeNotAdvanced, 0u);
        EXPECT_EQ(telemetry.dropBridgeRegressed, 0u);
        EXPECT_EQ(telemetry.dropRxFramesNotAdvanced, 0u);
        EXPECT_EQ(telemetry.dropRxFramesRegressed, 0u);
        EXPECT_EQ(telemetry.dropRxTicksNotAdvanced, 0u);
        EXPECT_EQ(telemetry.dropRxTicksRegressed, 0u);
    }

    // Case 2: an identical repeat fails all three counters at once.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        settle(tx);
        const auto repeat = tx.Apply(window(2, nominalTicks * 2));
        EXPECT_FALSE(repeat.observationApplied);
        EXPECT_EQ(repeat.dropCauses,
                  static_cast<uint8_t>(kMotuSphDropBridgeNotAdvanced |
                                       kMotuSphDropRxFramesNotAdvanced |
                                       kMotuSphDropRxTicksNotAdvanced));
        const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
        EXPECT_EQ(telemetry.observationDrops, 1u);
        EXPECT_EQ(telemetry.dropBridgeNotAdvanced, 1u);
        EXPECT_EQ(telemetry.dropRxFramesNotAdvanced, 1u);
        EXPECT_EQ(telemetry.dropRxTicksNotAdvanced, 1u);
        EXPECT_EQ(telemetry.dropBridgeRegressed, 0u);
        EXPECT_EQ(telemetry.dropRxFramesRegressed, 0u);
        EXPECT_EQ(telemetry.dropRxTicksRegressed, 0u);
    }

    // Case 3: only the device's SPH stamp goes backwards. Exactly one bit.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        settle(tx);
        const auto regressed = tx.Apply(window(3, nominalTicks));
        EXPECT_FALSE(regressed.observationApplied);
        EXPECT_EQ(regressed.dropCauses, kMotuSphDropRxTicksRegressed);
        const auto telemetry = tx.stamper.ServoTelemetrySnapshot();
        EXPECT_EQ(telemetry.observationDrops, 1u);
        EXPECT_EQ(telemetry.dropRxTicksRegressed, 1u);
        EXPECT_EQ(telemetry.dropRxTicksNotAdvanced, 0u);
        EXPECT_EQ(telemetry.dropBridgeNotAdvanced, 0u);
        EXPECT_EQ(telemetry.dropRxFramesNotAdvanced, 0u);
    }

    // Case 4: the bridge's own publication count going backwards -- a
    // host-side fault, not filed under the device.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        settle(tx);
        auto stale = window(3, nominalTicks * 3);
        stale.bridgeUpdates = 1;
        const auto dropped = tx.Apply(stale);
        EXPECT_FALSE(dropped.observationApplied);
        EXPECT_EQ(dropped.dropCauses, kMotuSphDropBridgeRegressed);
        EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().dropBridgeRegressed, 1u);
        EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().dropRxTicksRegressed, 0u);
    }

    // Case 5: the decision-cadence gate one layer below is NOT a drop.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        ASSERT_TRUE(tx.Apply(window(1, nominalTicks)).observationApplied);
        auto early = window(1, nominalTicks);
        early.bridgeUpdates = 2;
        early.rxFrames = windowFrames + (windowFrames / 2);
        early.rxTicks = nominalTicks + (nominalTicks / 2);
        const auto tooSoon = tx.Apply(early);
        EXPECT_FALSE(tooSoon.observationApplied);
        EXPECT_EQ(tooSoon.dropCauses, 0u);
        EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().observationDrops, 0u);
    }

    // Case 6: per start, like `decisions`.
    {
        Mk3Tx tx;
        ASSERT_TRUE(tx.Arm());
        settle(tx);
        ASSERT_FALSE(tx.Apply(window(2, nominalTicks * 2)).observationApplied);
        ASSERT_EQ(tx.stamper.ServoTelemetrySnapshot().observationDrops, 1u);
        tx.stamper.ResetForStart();
        EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().observationDrops, 0u);
        EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().dropRxTicksNotAdvanced, 0u);
    }
}

// A new control generation is a new acquisition, muted like a start, and an
// audible stream counts that mute.
TEST(MotuV3TxTimingStamperTests, ANewGenerationMutesForAFreshAcquisition) {
    Mk3Tx tx;
    ASSERT_TRUE(tx.Arm());
    ASSERT_TRUE(tx.Apply(Window(3, 1, 512, 512 * 512)).observationApplied);
    ASSERT_TRUE(tx.Apply(Window(3, 2, 1024, 2 * 512 * 512)).decision.locked);
    ASSERT_FALSE(tx.writer.IsPcmMuted());

    const auto restarted = tx.Apply(Window(4, 1, 512, 512 * 512));
    EXPECT_TRUE(restarted.observationApplied);
    EXPECT_TRUE(restarted.decision.phaseReferenceReset);
    EXPECT_TRUE(restarted.outputMuted);
    EXPECT_TRUE(tx.writer.IsPcmMuted());
    EXPECT_EQ(tx.stamper.ServoTelemetrySnapshot().muteTransitions, 2u);
}

} // namespace

// The guard for the phase-injection instrument build. A build carrying a
// non-zero injection is an instrument, not a driver; this fails the next
// --test-only until the constant is restored.
TEST(MotuV3TxTimingStamperTests, ShippedBuildCarriesNoPhaseInjection) {
    EXPECT_EQ(ASFW::Audio::Wire::kMotuSphPhaseInjectionTicks, 0)
        << "Phase-injection instrument build detected. If the run is over, set "
           "kMotuSphPhaseInjectionTicks back to 0 in MotuV3TxTimingStamper.hpp; "
           "if it is still under way, this failure is expected.";
}
