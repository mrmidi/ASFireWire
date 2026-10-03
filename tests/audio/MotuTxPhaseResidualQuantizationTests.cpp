// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Where the ~1024-tick quantization of `[RxPhaseRel]`'s `residualTicks`
// comes from.
//
// A hardware run found `residualTicks` sitting on discrete plateaus roughly
// 1024 ticks apart -- two audio frames at 48 kHz -- with a jump histogram
// containing -1028, while `packetLead` stayed practically constant at 677/678.
// A servo must not be handed an error signal that steps by two frames for a
// reason nobody has named.
//
// This file drives the PRODUCTION engine, blocking cadence and V3 SPH stamper with a
// perfectly nominal SPH clock and an exact hardware-completion projection --
// no drift, no jitter, no misframe, no device at all -- and the plateaus
// appear anyway. They are geometry of the instrument, not behaviour of the
// 828 Mk3:
//
//   * `ComputeMotuTxPhaseResidual` advances its EXPECTED SPH by
//     kTicksPerCycle = 3072 per unit of `packetLead`, and `packetLead` counts
//     PREPARED packets -- blocking mode prepares one per bus cycle, no-data
//     packets included.
//   * The ACTUAL SPH advances only on DATA packets, by
//     framesPerDataPacket * ticksPerFrame = 8 * 512 = 4096.
//
// The two agree only once per full cadence period (N,D,D,D at 48 kHz); in
// between, their difference walks the lattice gcd(4096, 3072) = 1024. That is
// the quantum, and it is present with a flawless clock on both sides.

#include "Audio/DriverKit/Config/MOTU/MOTU828Mk3Profile.hpp"
#include "Audio/DriverKit/Runtime/MotuPhaseTrace.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/AMDTP/AmdtpTypes.hpp"
#include "Audio/Wire/AMDTP/MotuV3WireFormat.hpp"
#include "Audio/Wire/MOTU/MotuV3TxTimingStamper.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Audio::Runtime::ComputeMotuTxPhaseResidual;
using ASFW::Audio::Runtime::MotuPhaseTraceSample;

class CapturingSlotProvider final : public IAmdtpTxSlotProvider {
public:
    std::array<uint8_t, 512> bytes{};
    PreparedTxPacket published{};

    bool AcquireWritableSlot(uint64_t packetIndex, TxPacketSlotView& outSlot) noexcept override {
        outSlot = {packetIndex, bytes.data(), static_cast<uint32_t>(bytes.size())};
        return true;
    }
    bool PublishSlot(const PreparedTxPacket& packet) noexcept override {
        published = packet;
        return true;
    }
    uint32_t SlotCount() const noexcept override { return 1; }
};

constexpr uint8_t kFramesPerDataPacket = 8;
constexpr uint8_t kMotuDbs = 13;
constexpr uint32_t kMotuDataPacketBytes = 424; // 8 CIP + 8 frames * 13 * 4
constexpr int64_t kTicksPerFrame48k = 512;     // kTicksPerSecond / 48000
constexpr int64_t kSphAdvancePerDataPacket =
    int64_t{kFramesPerDataPacket} * kTicksPerFrame48k; // 4096
constexpr int64_t kTicksPerCycle =
    static_cast<int64_t>(MotuV3Wire::kTicksPerCycle); // 3072

// One full 48 kHz blocking period: N,D,D,D -- four prepared packets, three of
// them DATA, 24 audio frames.
constexpr uint32_t kCyclesPerCadencePeriod = 4;
constexpr uint32_t kDataPacketsPerCadencePeriod = 3;
constexpr uint64_t kFramesPerCadencePeriod =
    uint64_t{kDataPacketsPerCadencePeriod} * kFramesPerDataPacket; // 24

// Long enough to cross many cadence periods, short enough to stay inside one
// SPH second so nothing here depends on the wrap (that is pinned elsewhere).
constexpr uint32_t kCycles = 4000;

// Hardware runs this far behind preparation. The value is the one the hardware
// run reported; the projection is supposed to be invariant to it, which
// `ResidualIsInvariantToPacketLeadJitter` below checks.
constexpr uint64_t kNominalLeadPackets = 678;

struct ResidualSample final {
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint64_t packetLead{0};
    int64_t residualTicks{0};
};

struct IdealTxRun final {
    std::vector<ResidualSample> samples;
    // What production actually publishes and reads back: the
    // cadence-period mean, formed by the production rolling window and the
    // production gate rather than by anything this test re-implements.
    std::vector<int64_t> cadenceMeanTicks;
    uint32_t dataPackets{0};
    uint32_t noDataPackets{0};
    uint32_t prepareFailures{0};
};

// Drives the production engine and V3 stamper for `kCycles` bus cycles. The model is
// deliberately perfect: packet p is completed by hardware in bus cycle p, and
// preparation runs `lead` packets ahead of the newest completion, so
// `completedTicks + packetLead * kTicksPerCycle` reconstructs packet p's own
// cycle exactly. Whatever the residual does is therefore not the device.
IdealTxRun RunIdealMotuTx(uint64_t leadPackets, bool jitterLead,
                          uint64_t alignCursorTo = 0) {
    IdealTxRun run{};
    ASFW::Isoch::Audio::MOTU::Profiles::MOTU828Mk3Profile profile;
    ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
    ASFW::Protocols::Audio::DICE::DiceTxStreamEngine engine{};
    ASFW::Audio::Wire::MotuV3TxTimingStamper stamper{};
    CapturingSlotProvider provider{};
    if (!profile.BuildDefaultTxStreamConfig(txConfig) || txConfig.dbs != kMotuDbs ||
        !engine.Configure(profile, txConfig) || !stamper.Configure(txConfig)) {
        run.prepareFailures = kCycles;
        return run;
    }
    engine.BindSlotProvider(&provider);
    engine.BindTimingStamper(&stamper);
    engine.ResetForStart(0, 0);

    if (alignCursorTo != 0 && !engine.AlignFrameCursorOnce(alignCursorTo)) {
        run.prepareFailures = kCycles;
        return run;
    }

    ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample cadenceWindow{};

    // The cadence, not the caller, decides DATA vs no-data: `replayValid` is
    // false, so the engine falls through to the cadence. The transmit time of
    // the first packet is 0, so the stamper seeds at exactly the presentation
    // lead.
    AmdtpTimingState timing{};
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = false;
    timing.transmitCycleValid = true;
    timing.transmitTicks = 0;

    for (uint32_t packetIndex = 0; packetIndex < kCycles; ++packetIndex) {
        if (engine.PrepareNextTransmitSlot(packetIndex, timing) !=
            ASFW::Protocols::Audio::DICE::TxSlotPrepareResult::kPrepared) {
            ++run.prepareFailures;
            continue;
        }
        const PreparedTxPacket packet = provider.published;
        if (!packet.isData) {
            ++run.noDataPackets; // nothing is published for a no-data packet
            continue;
        }
        ++run.dataPackets;
        const uint32_t firstSph = stamper.LastFirstSph();

        // Completion stamps arrive in bursts, so `packetLead` jitters by the
        // refill granularity. The projection is built to absorb that.
        const uint64_t lead =
            jitterLead ? leadPackets - (packet.packetIndex % 2) : leadPackets;
        if (packet.packetIndex < lead) {
            continue;
        }

        MotuPhaseTraceSample sample{};
        sample.packetIndex = packet.packetIndex;
        sample.outputLastPacketIndex = packet.packetIndex - lead;
        sample.firstSph = firstSph;
        sample.outputLastCycleTimer = MotuV3Wire::EncodeSph(
            static_cast<int64_t>(sample.outputLastPacketIndex) * kTicksPerCycle);
        sample.hasOutputLast = true;

        const auto residual = ComputeMotuTxPhaseResidual(sample);
        if (!residual.valid) {
            ++run.prepareFailures;
            continue;
        }
        run.samples.push_back({packet.firstAudioFrame, packet.framesInPacket,
                               residual.packetLead, residual.residualTicks});

        // Mirror the production publisher: slide every DATA packet into the
        // window, and take the mean where the production gate fires.
        ASFW::Audio::Runtime::PushMotuPhaseTraceCadencePacket(
            cadenceWindow, packet.packetIndex, firstSph);
        if (ASFW::Audio::Runtime::MotuPhaseTraceSamplesThisPacket(
                packet.firstAudioFrame, packet.framesInPacket,
                kDataPacketsPerCadencePeriod)) {
            cadenceWindow.outputLastPacketIndex = sample.outputLastPacketIndex;
            cadenceWindow.outputLastCycleTimer = sample.outputLastCycleTimer;
            cadenceWindow.hasOutputLast = sample.hasOutputLast;
            const auto mean =
                ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean(cadenceWindow);
            if (mean.valid) {
                run.cadenceMeanTicks.push_back(mean.residualTicks);
            }
        }
    }
    return run;
}

std::set<int64_t> DistinctResiduals(const std::vector<ResidualSample>& samples) {
    std::set<int64_t> levels;
    for (const auto& sample : samples) {
        levels.insert(sample.residualTicks);
    }
    return levels;
}

// The lattice statement, stated as arithmetic rather than as a magic number:
// an actual clock stepping `a` against an expected clock stepping `b` can only
// ever differ by multiples of gcd(a, b).
TEST(MotuTxPhaseResidualQuantizationTests, QuantumIsGcdOfSphAdvanceAndBusCycle) {
    EXPECT_EQ(std::gcd(kSphAdvancePerDataPacket, kTicksPerCycle), 1024);

    // Two audio frames at 48 kHz -- the size recorded on hardware.
    EXPECT_EQ(1024 / kTicksPerFrame48k, 2);

    // 96k and 192k land on the same quantum; the 44.1 family does not belong
    // here at all, because its ticks-per-frame (24576000/44100) is fractional,
    // so the accumulator never sits on an integer lattice. Any claim about
    // those rates needs its own measurement.
    EXPECT_EQ(std::gcd(int64_t{kFramesPerDataPacket} * 256, kTicksPerCycle), 1024);
    EXPECT_EQ(std::gcd(int64_t{kFramesPerDataPacket} * 128, kTicksPerCycle), 1024);
    EXPECT_NE(int64_t{ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerSecond} %
                  44100,
              0);
}

// The observation this whole file exists for: with nothing wrong anywhere, the
// residual still takes exactly three values, 1024 apart.
TEST(MotuTxPhaseResidualQuantizationTests,
     IdealTransmitStillQuantizesResidualToTheCadenceLattice) {
    const IdealTxRun run = RunIdealMotuTx(kNominalLeadPackets, false);
    ASSERT_EQ(run.prepareFailures, 0u);

    // N,D,D,D: three DATA packets per four prepared packets.
    EXPECT_EQ(run.dataPackets,
              kCycles / kCyclesPerCadencePeriod * kDataPacketsPerCadencePeriod);
    EXPECT_EQ(run.noDataPackets, kCycles / kCyclesPerCadencePeriod);
    ASSERT_FALSE(run.samples.empty());

    const std::set<int64_t> levels = DistinctResiduals(run.samples);
    ASSERT_EQ(levels.size(), 3u);

    const int64_t lowest = *levels.begin();
    const int64_t highest = *levels.rbegin();
    for (const int64_t level : levels) {
        SCOPED_TRACE(level);
        EXPECT_EQ((level - lowest) % 1024, 0);
    }
    // One full cadence period spans one packet's worth of SPH advance minus
    // one cycle, taken three times: 4096 - 3072 = 1024 per step, two steps.
    EXPECT_EQ(highest - lowest, 2048);

    // The absolute values follow from the geometry alone, so pin them: the
    // three branches are one, two and three cycles' worth of projection ahead
    // of the frames actually shipped.
    EXPECT_EQ(lowest, -3072);
    EXPECT_EQ(highest, -1024);
}

// Hardware recorded `packetLead` at 677/678 during the plateaus, which ruled out
// a changing lead as the explanation. That conclusion was read off
// hardware; here it is a property of the projection, which subtracts the same
// cycles it adds.
TEST(MotuTxPhaseResidualQuantizationTests, ResidualIsInvariantToPacketLeadJitter) {
    const IdealTxRun steady = RunIdealMotuTx(kNominalLeadPackets, false);
    const IdealTxRun jittered = RunIdealMotuTx(kNominalLeadPackets, true);
    ASSERT_EQ(steady.prepareFailures, 0u);
    ASSERT_EQ(jittered.prepareFailures, 0u);

    std::set<uint64_t> observedLeads;
    for (const auto& sample : jittered.samples) {
        observedLeads.insert(sample.packetLead);
    }
    EXPECT_EQ(observedLeads.size(), 2u); // the jitter really happened
    EXPECT_EQ(DistinctResiduals(jittered.samples),
              DistinctResiduals(steady.samples));
}

std::vector<ResidualSample> GatedByProductionRule(
    const std::vector<ResidualSample>& samples, uint32_t stride) {
    std::vector<ResidualSample> gated;
    std::copy_if(samples.begin(), samples.end(), std::back_inserter(gated),
                 [stride](const ResidualSample& sample) {
                     return ASFW::Audio::Runtime::MotuPhaseTraceSamplesThisPacket(
                         sample.firstAudioFrame, sample.framesInPacket, stride);
                 });
    return gated;
}

// The consequence for P5, and the production gate: sampling the trace only
// where a whole cadence period has closed removes the quantization, leaving one
// level. Stride 3 (three DATA packets = one N,D,D,D period) is the minimum that
// works; any multiple of it works for the same reason.
TEST(MotuTxPhaseResidualQuantizationTests,
     ProductionGateLeavesOneLevelAtCadencePeriodStrides) {
    const IdealTxRun run = RunIdealMotuTx(kNominalLeadPackets, false);
    ASSERT_EQ(run.prepareFailures, 0u);

    for (const uint32_t stride : {kDataPacketsPerCadencePeriod,
                                  kDataPacketsPerCadencePeriod * 2}) {
        SCOPED_TRACE(stride);
        const auto gated = GatedByProductionRule(run.samples, stride);
        ASSERT_FALSE(gated.empty());
        EXPECT_EQ(DistinctResiduals(gated).size(), 1u);
        // The gate really thinned the series rather than passing everything.
        EXPECT_NEAR(static_cast<double>(gated.size()),
                    static_cast<double>(run.samples.size()) / stride, 1.0);
    }

    // A stride that is a packet count but not a cadence period does NOT help --
    // this is why "publish every second DATA packet" is not the fix, and why
    // stride 0/1 (publishing every DATA packet) shows the full lattice.
    for (const uint32_t stride : {0u, 1u, 2u, 4u}) {
        SCOPED_TRACE(stride);
        const auto gated = GatedByProductionRule(run.samples, stride);
        ASSERT_FALSE(gated.empty());
        EXPECT_GT(DistinctResiduals(gated).size(), 1u);
    }
}

// ---------------------------------------------------------------------------
// The gate above pins ONE bucket per stream -- it does not pin WHICH.
// ---------------------------------------------------------------------------

std::set<int64_t> DistinctValues(const std::vector<int64_t>& values) {
    return std::set<int64_t>(values.begin(), values.end());
}

// The defect: `AlignFrameCursorOnce` decides which of the three cadence buckets
// the gate lands on, so the same stream restarted reports a `rel` up to 2048
// ticks different for no physical reason. That is the +/-1024 that makes the
// setpoint measured off the official driver untransferable, and would make an
// injected 1024-tick step indistinguishable from a bucket change.
//
// The fix and its proof in one test: the single-bucket level moves across all
// three buckets with alignment, and the production cadence mean does not move
// at all.
TEST(MotuTxPhaseResidualQuantizationTests, CadenceMeanIsInvariantToWhereAlignmentLanded) {
    // 0 and 4 land on the same bucket (sub-packet cursor offsets do not move
    // the SPH, which advances per DATA packet); 8, 16 and 1745 walk the other
    // two. Together they cover all three buckets.
    constexpr std::array<uint64_t, 5> kAlignments{0, 4, 8, 16, 1745};

    std::set<int64_t> singleBucketLevels;
    std::set<int64_t> cadenceMeans;
    for (const uint64_t align : kAlignments) {
        SCOPED_TRACE(align);
        const IdealTxRun run = RunIdealMotuTx(kNominalLeadPackets, false, align);
        ASSERT_EQ(run.prepareFailures, 0u);

        const auto gated = GatedByProductionRule(run.samples, kDataPacketsPerCadencePeriod);
        const std::set<int64_t> levels = DistinctResiduals(gated);
        ASSERT_EQ(levels.size(), 1u); // the gate still holds one bucket
        singleBucketLevels.insert(*levels.begin());

        const std::set<int64_t> means = DistinctValues(run.cadenceMeanTicks);
        ASSERT_FALSE(means.empty());
        EXPECT_EQ(means.size(), 1u); // and the mean is steady within a run too
        cadenceMeans.insert(*means.begin());
    }

    // The ambiguity is real and it is the full lattice: three distinct levels,
    // 1024 apart, from alignment alone.
    EXPECT_EQ(singleBucketLevels.size(), 3u);
    EXPECT_EQ(*singleBucketLevels.rbegin() - *singleBucketLevels.begin(), 2048);

    // And the mean removes it completely: one value across every alignment,
    // sitting exactly at the middle bucket.
    ASSERT_EQ(cadenceMeans.size(), 1u);
    EXPECT_EQ(*cadenceMeans.begin(), -2048);
}

// A partial window must not produce a number: two thirds of a period carries
// exactly the bias the mean exists to remove, and at stream start that is the
// state the trace is in until the third DATA packet arrives.
TEST(MotuTxPhaseResidualQuantizationTests, CadenceMeanRefusesAPartialPeriod) {
    ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample window{};
    window.hasOutputLast = true;
    window.outputLastPacketIndex = 100;
    window.outputLastCycleTimer =
        MotuV3Wire::EncodeSph(int64_t{100} * kTicksPerCycle);

    for (uint32_t packets = 0; packets < kDataPacketsPerCadencePeriod; ++packets) {
        SCOPED_TRACE(packets);
        EXPECT_FALSE(
            ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean(window).valid);
        ASFW::Audio::Runtime::PushMotuPhaseTraceCadencePacket(
            window, 778 + packets,
            MotuV3Wire::EncodeSph(int64_t{778 + packets} * kSphAdvancePerDataPacket));
    }
    EXPECT_TRUE(ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean(window).valid);
    EXPECT_EQ(window.count, kDataPacketsPerCadencePeriod);
}

// The frame cursor is not guaranteed to sit on a packet multiple:
// AlignFrameCursorOnce can push it off the grid, and a `firstAudioFrame % 24 == 0` gate
// would then never fire at all -- silently killing [RxPhaseRel] instead of
// cleaning it. The production rule divides by the packet's own frame count
// first, so the DATA packet ordinal still advances by one per packet.
TEST(MotuTxPhaseResidualQuantizationTests,
     ProductionGateSurvivesAFrameCursorOffThePacketGrid) {
    constexpr uint64_t kOffGridCursor = 1745; // 1745 % 8 != 0
    const IdealTxRun run =
        RunIdealMotuTx(kNominalLeadPackets, false, kOffGridCursor);
    ASSERT_EQ(run.prepareFailures, 0u);
    ASSERT_FALSE(run.samples.empty());
    EXPECT_EQ(run.samples.front().firstAudioFrame % kFramesPerDataPacket,
              kOffGridCursor % kFramesPerDataPacket);

    // A naive frame-modulo gate never fires here...
    const auto naive = std::count_if(
        run.samples.begin(), run.samples.end(),
        [](const ResidualSample& sample) {
            return sample.firstAudioFrame % kFramesPerCadencePeriod == 0;
        });
    EXPECT_EQ(naive, 0);

    // ...while the production rule keeps firing, and still leaves one level.
    const auto gated =
        GatedByProductionRule(run.samples, kDataPacketsPerCadencePeriod);
    ASSERT_FALSE(gated.empty());
    EXPECT_EQ(DistinctResiduals(gated).size(), 1u);
    EXPECT_GT(DistinctResiduals(run.samples).size(), 1u);
}

} // namespace
