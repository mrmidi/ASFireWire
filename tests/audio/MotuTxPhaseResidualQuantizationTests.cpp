// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Why the TX presentation residual is averaged over a cadence period.
//
// This drives the production engine, blocking cadence and MOTU SPH stamper with
// a perfectly nominal SPH clock and an exact completion projection -- no drift,
// no jitter, no device -- and a single packet's residual still sits on three
// levels 1024 ticks apart. That is geometry of the instrument, not behaviour of
// any device:
//
//   * ComputeMotuTxPhaseResidual advances its EXPECTED SPH by 3072 ticks per
//     unit of packetLead, and packetLead counts PREPARED packets -- blocking
//     mode prepares one per bus cycle, NO-DATA included.
//   * The ACTUAL SPH advances only on DATA packets, by 8 frames * 512 ticks =
//     4096 at 48 kHz.
//
// The two agree once per cadence period (N,D,D,D at 48 kHz); in between their
// difference walks the lattice gcd(4096, 3072) = 1024.

#include "Audio/DriverKit/Config/MOTU/MotuProfile.hpp"
#include "Audio/DriverKit/Runtime/MotuPhaseTrace.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/AMDTP/AmdtpTypes.hpp"
#include "Audio/Wire/MOTU/MotuDeviceTiming.hpp"
#include "Audio/Wire/MOTU/MotuModel.hpp"
#include "Audio/Wire/MOTU/MotuSph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <set>
#include <vector>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Audio::Runtime::ComputeMotuTxPhaseResidual;
using ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean;
using ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample;
using ASFW::Audio::Runtime::MotuPhaseTraceSample;
using ASFW::Audio::Runtime::MotuPhaseTraceSamplesThisPacket;
using ASFW::Audio::Runtime::PushMotuPhaseTraceCadencePacket;

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

constexpr uint32_t k828mk3 = 0x15;
constexpr uint8_t kFramesPerDataPacket = 8;
constexpr int64_t kTicksPerFrame48k = 512;
constexpr int64_t kSphAdvancePerDataPacket = int64_t{kFramesPerDataPacket} * kTicksPerFrame48k;
constexpr int64_t kTicksPerCycle = ASFW::Encoding::Motu::kTicksPerCycle;
constexpr uint32_t kLead = ASFW::Encoding::Motu::kDefaultPresentationLeadTicks;

// One 48 kHz blocking period: N,D,D,D.
constexpr uint32_t kCyclesPerCadencePeriod = 4;
constexpr uint32_t kDataPacketsPerCadencePeriod = 3;
constexpr uint64_t kFramesPerCadencePeriod =
    uint64_t{kDataPacketsPerCadencePeriod} * kFramesPerDataPacket;

// Many cadence periods, all inside one SPH second, so nothing here depends on
// the wrap.
constexpr uint32_t kCycles = 4000;

// How far preparation runs ahead of the newest completion. The projection is
// meant to be invariant to it (ResidualIsInvariantToPacketLeadJitter).
constexpr uint64_t kNominalLeadPackets = 678;

uint32_t CycleTimerAt(int64_t ticks) {
    return ASFW::Timing::encodeCycleTimer(static_cast<uint32_t>(ticks / 24'576'000),
                                          static_cast<uint32_t>((ticks / kTicksPerCycle) % 8000),
                                          static_cast<uint32_t>(ticks % kTicksPerCycle));
}

uint32_t SphAt(int64_t ticks) {
    return ASFW::Encoding::Motu::SphFromTick(ASFW::Encoding::Motu::WrapTick(ticks));
}

struct ResidualSample final {
    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint64_t packetLead{0};
    int64_t residualTicks{0};
};

struct IdealTxRun final {
    std::vector<ResidualSample> samples;
    // What the publisher publishes and the RX drain reads back: the
    // cadence-period mean, formed by the production window and gate.
    std::vector<int64_t> cadenceMeanTicks;
    uint32_t dataPackets{0};
    uint32_t noDataPackets{0};
    uint32_t prepareFailures{0};
};

// The model is perfect: packet p is completed in bus cycle p, and preparation
// runs `leadPackets` ahead of the newest completion, so the projection
// reconstructs packet p's own cycle exactly. Whatever the residual does is not
// the device.
IdealTxRun RunIdealMotuTx(uint64_t leadPackets, bool jitterLead, uint64_t alignCursorTo = 0) {
    IdealTxRun run{};
    const ASFW::Isoch::Audio::MOTU::Profiles::MotuProfile profile{k828mk3};
    ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
    ASFW::Protocols::Audio::DICE::DiceTxStreamEngine engine{};
    ASFW::Audio::Wire::MotuTxTimingStamper stamper{};
    CapturingSlotProvider provider{};
    if (!profile.BuildDefaultTxStreamConfig(txConfig) || txConfig.sampleRate != 48000 ||
        !engine.Configure(profile, txConfig) ||
        !stamper.Configure(txConfig.dbs, txConfig.sampleRate, kLead)) {
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

    MotuPhaseTraceCadenceSample window{};
    window.presentationLeadTicks = kLead;

    // The cadence decides DATA vs NO-DATA (`replayValid` false). Every packet
    // claims transmit time 0, so the stamper seeds at exactly the lead.
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
            ++run.noDataPackets;
            continue;
        }
        ++run.dataPackets;
        const uint32_t firstSph = stamper.LastFirstSph();

        // Completion stamps arrive in bursts, so packetLead jitters by the
        // refill granularity. The projection absorbs that.
        const uint64_t lead = jitterLead ? leadPackets - (packet.packetIndex % 2) : leadPackets;
        if (packet.packetIndex < lead) {
            continue;
        }

        const MotuPhaseTraceSample sample{
            .packetIndex = packet.packetIndex,
            .outputLastPacketIndex = packet.packetIndex - lead,
            .firstSph = firstSph,
            .outputLastCycleTimer =
                CycleTimerAt(static_cast<int64_t>(packet.packetIndex - lead) * kTicksPerCycle),
            .presentationLeadTicks = kLead,
            .hasOutputLast = true,
        };
        const auto residual = ComputeMotuTxPhaseResidual(sample);
        if (!residual.valid) {
            ++run.prepareFailures;
            continue;
        }
        run.samples.push_back(
            {packet.firstAudioFrame, packet.framesInPacket, residual.packetLead, residual.residualTicks});

        // Mirror the publisher: slide every DATA packet in, take the mean
        // where the production gate fires.
        PushMotuPhaseTraceCadencePacket(window, packet.packetIndex, firstSph);
        if (MotuPhaseTraceSamplesThisPacket(packet.firstAudioFrame, packet.framesInPacket,
                                            ASFW::Audio::Runtime::kMotuPhaseTraceDataPacketStride)) {
            window.outputLastPacketIndex = sample.outputLastPacketIndex;
            window.outputLastCycleTimer = sample.outputLastCycleTimer;
            window.hasOutputLast = true;
            const auto mean = ComputeMotuTxPhaseResidualCadenceMean(window);
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

std::vector<ResidualSample> GatedByProductionRule(const std::vector<ResidualSample>& samples,
                                                  uint32_t stride) {
    std::vector<ResidualSample> gated;
    std::copy_if(samples.begin(), samples.end(), std::back_inserter(gated),
                 [stride](const ResidualSample& sample) {
                     return MotuPhaseTraceSamplesThisPacket(sample.firstAudioFrame,
                                                            sample.framesInPacket, stride);
                 });
    return gated;
}

// An actual clock stepping `a` against an expected clock stepping `b` can only
// differ by multiples of gcd(a, b). The same 1024 holds across the 48 kHz
// family; the 44.1 kHz family has no integer ticks per frame at all.
TEST(MotuTxPhaseResidualQuantizationTests, QuantumIsGcdOfSphAdvanceAndBusCycle) {
    EXPECT_EQ(std::gcd(kSphAdvancePerDataPacket, kTicksPerCycle), 1024);
    EXPECT_EQ(1024 / kTicksPerFrame48k, 2);  // two frames at 48 kHz
    EXPECT_EQ(std::gcd(int64_t{16} * 256, kTicksPerCycle), 1024);  // 96 kHz
    EXPECT_EQ(std::gcd(int64_t{32} * 128, kTicksPerCycle), 1024);  // 192 kHz
    EXPECT_NE(int64_t{ASFW::Encoding::Motu::kTicksPerSecond} % 44100, 0);

    EXPECT_TRUE(ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(48000));
    EXPECT_TRUE(ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(96000));
    EXPECT_TRUE(ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(192000));
    EXPECT_FALSE(ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(44100));
    EXPECT_FALSE(ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(88200));
    EXPECT_FALSE(ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(32000));
    EXPECT_FALSE(ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(0));
}

// With nothing wrong anywhere the single-packet residual takes exactly three
// values, 1024 apart.
TEST(MotuTxPhaseResidualQuantizationTests, IdealTransmitStillQuantizesResidualToTheCadenceLattice) {
    const IdealTxRun run = RunIdealMotuTx(kNominalLeadPackets, false);
    ASSERT_EQ(run.prepareFailures, 0u);
    EXPECT_EQ(run.dataPackets, kCycles / kCyclesPerCadencePeriod * kDataPacketsPerCadencePeriod);
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
    EXPECT_EQ(highest - lowest, 2048);
    // From the geometry alone: one, two and three cycles' worth of projection
    // ahead of the frames actually shipped.
    EXPECT_EQ(lowest, -3072);
    EXPECT_EQ(highest, -1024);
}

// The projection subtracts the cycles it adds, so a jittering packetLead
// changes no residual.
TEST(MotuTxPhaseResidualQuantizationTests, ResidualIsInvariantToPacketLeadJitter) {
    const IdealTxRun steady = RunIdealMotuTx(kNominalLeadPackets, false);
    const IdealTxRun jittered = RunIdealMotuTx(kNominalLeadPackets, true);
    ASSERT_EQ(steady.prepareFailures, 0u);
    ASSERT_EQ(jittered.prepareFailures, 0u);

    std::set<uint64_t> observedLeads;
    for (const auto& sample : jittered.samples) {
        observedLeads.insert(sample.packetLead);
    }
    EXPECT_EQ(observedLeads.size(), 2u);  // the jitter really happened
    EXPECT_EQ(DistinctResiduals(jittered.samples), DistinctResiduals(steady.samples));
}

// Sampling once per whole cadence period leaves one level; a stride that is not
// a whole period (or publishing every DATA packet) leaves the lattice.
TEST(MotuTxPhaseResidualQuantizationTests, ProductionGateLeavesOneLevelAtCadencePeriodStrides) {
    const IdealTxRun run = RunIdealMotuTx(kNominalLeadPackets, false);
    ASSERT_EQ(run.prepareFailures, 0u);

    for (const uint32_t stride : {kDataPacketsPerCadencePeriod, kDataPacketsPerCadencePeriod * 2}) {
        SCOPED_TRACE(stride);
        const auto gated = GatedByProductionRule(run.samples, stride);
        ASSERT_FALSE(gated.empty());
        EXPECT_EQ(DistinctResiduals(gated).size(), 1u);
        EXPECT_NEAR(static_cast<double>(gated.size()),
                    static_cast<double>(run.samples.size()) / stride, 1.0);
    }
    for (const uint32_t stride : {0u, 1u, 2u, 4u}) {
        SCOPED_TRACE(stride);
        const auto gated = GatedByProductionRule(run.samples, stride);
        ASSERT_FALSE(gated.empty());
        EXPECT_GT(DistinctResiduals(gated).size(), 1u);
    }
}

// The gate pins ONE level per stream but not WHICH: the start's frame alignment
// decides that, so a single-packet `rel` could differ by up to 2048 ticks
// between two starts of the same stream. The cadence mean does not move.
TEST(MotuTxPhaseResidualQuantizationTests, CadenceMeanIsInvariantToWhereAlignmentLanded) {
    // 0 and 4 land on the same level (the SPH advances per DATA packet, not
    // per frame); 8, 16 and 1745 walk the other two.
    constexpr std::array<uint64_t, 5> kAlignments{0, 4, 8, 16, 1745};

    std::set<int64_t> singleBucketLevels;
    std::set<int64_t> cadenceMeans;
    for (const uint64_t align : kAlignments) {
        SCOPED_TRACE(align);
        const IdealTxRun run = RunIdealMotuTx(kNominalLeadPackets, false, align);
        ASSERT_EQ(run.prepareFailures, 0u);

        const std::set<int64_t> levels =
            DistinctResiduals(GatedByProductionRule(run.samples, kDataPacketsPerCadencePeriod));
        ASSERT_EQ(levels.size(), 1u);
        singleBucketLevels.insert(*levels.begin());

        const std::set<int64_t> means(run.cadenceMeanTicks.begin(), run.cadenceMeanTicks.end());
        ASSERT_FALSE(means.empty());
        EXPECT_EQ(means.size(), 1u);
        cadenceMeans.insert(*means.begin());
    }

    EXPECT_EQ(singleBucketLevels.size(), 3u);
    EXPECT_EQ(*singleBucketLevels.rbegin() - *singleBucketLevels.begin(), 2048);
    ASSERT_EQ(cadenceMeans.size(), 1u);
    EXPECT_EQ(*cadenceMeans.begin(), -2048);
}

// A partial window yields no number: two thirds of a period carries exactly
// the bias the mean removes, and at stream start that is the state the window
// is in until the third DATA packet.
TEST(MotuTxPhaseResidualQuantizationTests, CadenceMeanRefusesAPartialPeriod) {
    MotuPhaseTraceCadenceSample window{};
    window.hasOutputLast = true;
    window.outputLastPacketIndex = 100;
    window.outputLastCycleTimer = CycleTimerAt(int64_t{100} * kTicksPerCycle);
    window.presentationLeadTicks = kLead;

    for (uint32_t packets = 0; packets < kDataPacketsPerCadencePeriod; ++packets) {
        SCOPED_TRACE(packets);
        EXPECT_FALSE(ComputeMotuTxPhaseResidualCadenceMean(window).valid);
        PushMotuPhaseTraceCadencePacket(window, 778 + packets,
                                        SphAt(int64_t{778 + packets} * kSphAdvancePerDataPacket));
    }
    EXPECT_TRUE(ComputeMotuTxPhaseResidualCadenceMean(window).valid);
    EXPECT_EQ(window.count, kDataPacketsPerCadencePeriod);

    // A reference newer than a packet is no projection at all.
    window.outputLastPacketIndex = 779;
    EXPECT_FALSE(ComputeMotuTxPhaseResidualCadenceMean(window).valid);
    window.outputLastPacketIndex = 100;
    window.hasOutputLast = false;
    EXPECT_FALSE(ComputeMotuTxPhaseResidualCadenceMean(window).valid);
}

// The lead is the model's: a stream stamped with another lead reads the same
// residual when the projection uses that lead, and is off by the difference
// when it does not.
TEST(MotuTxPhaseResidualQuantizationTests, ProjectionAddsTheModelLead) {
    constexpr uint32_t kOtherLead = 2 * kTicksPerCycle;
    MotuPhaseTraceSample sample{
        .packetIndex = 40,
        .outputLastPacketIndex = 30,
        .firstSph = SphAt(int64_t{40} * kTicksPerCycle + kOtherLead + 17),
        .outputLastCycleTimer = CycleTimerAt(int64_t{30} * kTicksPerCycle),
        .presentationLeadTicks = kOtherLead,
        .hasOutputLast = true,
    };
    auto residual = ComputeMotuTxPhaseResidual(sample);
    ASSERT_TRUE(residual.valid);
    EXPECT_EQ(residual.packetLead, 10u);
    EXPECT_EQ(residual.residualTicks, 17);

    sample.presentationLeadTicks = kLead;
    residual = ComputeMotuTxPhaseResidual(sample);
    ASSERT_TRUE(residual.valid);
    EXPECT_EQ(residual.residualTicks, 17 - static_cast<int64_t>(kLead - kOtherLead));
}

// The cycle timer counts whole seconds up to 128, the SPH does not: the
// projection must not read those seconds as phase, across the SPH rollover too.
TEST(MotuTxPhaseResidualQuantizationTests, ResidualIgnoresWholeSecondsAndTheRollover) {
    constexpr int64_t kSecond = ASFW::Encoding::Motu::kTicksPerSecond;
    for (const int64_t second : {int64_t{0}, int64_t{5}, int64_t{127}}) {
        SCOPED_TRACE(second);
        // Completed one cycle before the rollover; the packet lands after it.
        const int64_t completed = second * kSecond + kSecond - kTicksPerCycle;
        const MotuPhaseTraceSample sample{
            .packetIndex = 3,
            .outputLastPacketIndex = 1,
            .firstSph = SphAt(completed + 2 * kTicksPerCycle + kLead - 100),
            .outputLastCycleTimer = CycleTimerAt(completed),
            .presentationLeadTicks = kLead,
            .hasOutputLast = true,
        };
        const auto residual = ComputeMotuTxPhaseResidual(sample);
        ASSERT_TRUE(residual.valid);
        EXPECT_EQ(residual.residualTicks, -100);
    }

    // The receive side's absolute phase folds the same way.
    for (const uint32_t hostSeconds : {0u, 7u, 93u, 127u}) {
        SCOPED_TRACE(hostSeconds);
        const uint32_t cycleTimer = ASFW::Timing::encodeCycleTimer(hostSeconds, 7999, 0);
        EXPECT_EQ(ASFW::Audio::Runtime::MotuRxSphMinusCycleTimerTicks(SphAt(9216 - kTicksPerCycle),
                                                                      cycleTimer),
                  9216);
    }
}

// AlignFrameCursorOnce can leave the frame cursor off a packet multiple. A gate
// on the frame cursor itself would then never fire; the production rule keeps
// firing and still leaves one level.
TEST(MotuTxPhaseResidualQuantizationTests, ProductionGateSurvivesAFrameCursorOffThePacketGrid) {
    constexpr uint64_t kOffGridCursor = 1745;  // 1745 % 8 != 0
    const IdealTxRun run = RunIdealMotuTx(kNominalLeadPackets, false, kOffGridCursor);
    ASSERT_EQ(run.prepareFailures, 0u);
    ASSERT_FALSE(run.samples.empty());
    EXPECT_EQ(run.samples.front().firstAudioFrame % kFramesPerDataPacket,
              kOffGridCursor % kFramesPerDataPacket);

    const auto naive = std::count_if(run.samples.begin(), run.samples.end(),
                                     [](const ResidualSample& sample) {
                                         return sample.firstAudioFrame % kFramesPerCadencePeriod == 0;
                                     });
    EXPECT_EQ(naive, 0);

    const auto gated = GatedByProductionRule(run.samples, kDataPacketsPerCadencePeriod);
    ASSERT_FALSE(gated.empty());
    EXPECT_EQ(DistinctResiduals(gated).size(), 1u);
    EXPECT_GT(DistinctResiduals(run.samples).size(), 1u);
}

} // namespace
