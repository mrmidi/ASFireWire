// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// TxRefillCoverageTests.cpp
//
// Decides whether the IT underrun ("slot N not committed, commitGen one lap
// behind") is a refill RANGE/GENERATION-coverage problem or merely a scheduling
// margin race. It models the exact producer/consumer generation contract from
// IsochTxDmaRing::Refill + PrepareTransmitSlots, with NO timing — the producer
// always runs between refills (field [TxPrep] telemetry shows <100 us latency,
// so this best-case ordering is the realistic one).
//
// Faithful model of the shipped code:
//   * Consumer refill checks slots [completion + ringAhead, +deltaConsumed),
//     where ringAhead == kTxHardwareRingPackets steady-state, and requires
//     commitGen[slot] == ExpectedCommitGen(absPacket) (IsochTxDmaRing.cpp:503).
//   * Producer tops the committed cursor to completion + lead each wake
//     (PrepareTransmitSlots, linear in absolute index → no modulo split).
//   * Coverage therefore holds iff a single coalesced deltaConsumed
//     <= lead - ringAhead. A coalesced IT completion larger than that overruns
//     the committed region regardless of latency.

#include "Shared/Isoch/AudioTimingGeometry.hpp"
#include "Shared/Isoch/IsochAudioTransport.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ASFW::IsochTransport::AudioTimingGeometry;
using ASFW::IsochTransport::ExpectedCommitGen;

constexpr uint32_t kNumSlots = AudioTimingGeometry::kTxSharedSlotPackets;     // 192
constexpr uint32_t kHwRing = AudioTimingGeometry::kTxHardwareRingPackets;     // 48
constexpr uint32_t kLead = AudioTimingGeometry::kTxPreparationLeadPackets;    // 84 (current)
constexpr uint32_t kGroup = AudioTimingGeometry::kTxPacketsPerGroup;          // 6

// The historical pre-fix lead (slack == 2*group) the hardware IT FATAL was
// captured at (deltaConsumed=13 holed it). Used to pin the bug independently
// of the now-corrected live geometry.
constexpr uint32_t kOldLead = kHwRing + 2 * kGroup;  // 60

constexpr uint64_t kNoMiss = UINT64_MAX;

// Faithful generation-coverage simulator. ringAhead is held at the hardware
// ring depth (CommitRefill restores it each pass when packetsFilled ==
// deltaConsumed, which is the steady state).
class RefillSim {
public:
    RefillSim(uint32_t numSlots, uint32_t lead, uint32_t ringAhead)
        : numSlots_(numSlots),
          lead_(lead),
          ringAhead_(ringAhead),
          commitGen_(numSlots, 0) {}

    // Producer tops the committed cursor to completion + lead (one wake).
    void RunProducer() {
        const uint64_t target = completion_ + lead_;
        for (uint64_t abs = expose_; abs < target; ++abs) {
            commitGen_[abs % numSlots_] = ExpectedCommitGen(abs, numSlots_);
        }
        if (target > expose_) {
            expose_ = target;
        }
    }

    // One IT refill ISR pass retiring `deltaConsumed` packets. Returns the first
    // absolute packet whose slot was NOT committed for the expected lap, or
    // kNoMiss if fully covered (= what the shipped refill ISR would FATAL on).
    uint64_t Refill(uint32_t deltaConsumed) {
        const uint64_t fillBase = completion_ + ringAhead_;  // softwareFillAbsIdx_
        uint64_t firstMiss = kNoMiss;
        for (uint32_t i = 0; i < deltaConsumed; ++i) {
            const uint64_t fillAbs = fillBase + i;
            if (commitGen_[fillAbs % numSlots_] !=
                ExpectedCommitGen(fillAbs, numSlots_)) {
                firstMiss = fillAbs;
                break;
            }
        }
        completion_ += deltaConsumed;
        return firstMiss;
    }

    [[nodiscard]] uint64_t Completion() const { return completion_; }
    [[nodiscard]] uint64_t Expose() const { return expose_; }
    [[nodiscard]] uint32_t CommittedMargin() const {
        return static_cast<uint32_t>(expose_ - completion_);
    }

private:
    uint32_t numSlots_;
    uint32_t lead_;
    uint32_t ringAhead_;
    std::vector<uint64_t> commitGen_;
    uint64_t completion_{0};
    uint64_t expose_{0};
};

// Drive `cycles` IT interrupts with the given coalesced delta, producer running
// between each. Returns the first missing absolute packet, or kNoMiss.
uint64_t DriveSteady(uint32_t lead, uint32_t ringAhead, uint32_t deltaPerRefill,
                     uint32_t cycles) {
    RefillSim sim(kNumSlots, lead, ringAhead);
    sim.RunProducer();  // pre-RUN prefill
    for (uint32_t c = 0; c < cycles; ++c) {
        const uint64_t miss = sim.Refill(deltaPerRefill);
        if (miss != kNoMiss) {
            return miss;
        }
        sim.RunProducer();
    }
    return kNoMiss;
}

// -----------------------------------------------------------------------------
// The coverage theorem: a single refill is safe iff deltaConsumed <= lead-ring.
// -----------------------------------------------------------------------------

TEST(TxRefillCoverage, SingleRefillSafeIffDeltaWithinLeadMinusRing) {
    const uint32_t budget = kLead - kHwRing;  // == slack
    ASSERT_EQ(budget, AudioTimingGeometry::kTxPreparationSlackPackets);
    for (uint32_t delta = 1; delta <= budget + 2 * kGroup; ++delta) {
        const uint64_t miss = DriveSteady(kLead, kHwRing, delta, /*cycles=*/64);
        if (delta <= budget) {
            EXPECT_EQ(miss, kNoMiss) << "delta=" << delta << " should be covered";
        } else {
            EXPECT_NE(miss, kNoMiss)
                << "delta=" << delta << " exceeds lead-ring budget but did not hole";
        }
    }
}

// -----------------------------------------------------------------------------
// Nominal cadence (one group per IT interrupt) is clean over thousands of
// packets — proving the range/generation math has NO off-by-one or wrap hole.
// -----------------------------------------------------------------------------

TEST(TxRefillCoverage, NominalSingleGroupNeverHolesOverThousands) {
    // 6 packets/refill for >> one ring lap. 8000 refills = 48000 packets.
    EXPECT_EQ(DriveSteady(kLead, kHwRing, kGroup, /*cycles=*/8000), kNoMiss);
}

TEST(TxRefillCoverage, TwoGroupCoalesceCoveredAtOldLead) {
    // 12 packets/refill == the exact OLD budget (lead 60); covered with zero
    // spare — matches the field [TxPrep] margin pinned at 48 before the fix.
    EXPECT_EQ(DriveSteady(kOldLead, kHwRing, 2 * kGroup, /*cycles=*/8000), kNoMiss);
}

// -----------------------------------------------------------------------------
// THE BUG (historical, at the pre-fix lead=60): a coalesced IT completion of
// >=3 groups overruns the committed region even with zero producer latency.
// This is the field FATAL ("slot N not committed, commitGen one lap behind").
// -----------------------------------------------------------------------------

TEST(TxRefillCoverage, ThreeGroupCoalesceHolesAtOldLead) {
    const uint64_t miss = DriveSteady(kOldLead, kHwRing, 3 * kGroup, /*cycles=*/8);
    EXPECT_NE(miss, kNoMiss)
        << "18-packet coalesced refill must overrun the old 60-packet lead";
}

TEST(TxRefillCoverage, FourGroupCoalesceHolesAtOldLead) {
    EXPECT_NE(DriveSteady(kOldLead, kHwRing, 4 * kGroup, /*cycles=*/8), kNoMiss);
}

TEST(TxRefillCoverage, HardwareCapturedDeltaThirteenHolesAtOldLead) {
    // The exact captured IT FATAL: deltaConsumed=13 > old budget 12.
    EXPECT_NE(DriveSteady(kOldLead, kHwRing, 13, /*cycles=*/8), kNoMiss);
    // 12 (== old budget) is the largest that still fit at the old lead.
    EXPECT_EQ(DriveSteady(kOldLead, kHwRing, 12, /*cycles=*/8000), kNoMiss);
}

TEST(TxRefillCoverage, OccasionalCoalesceAmongNominalStillHolesAtOldLead) {
    // Mostly single-group, with a periodic 3-group coalesce — the realistic
    // stochastic pattern that killed the field run after a while at lead=60.
    RefillSim sim(kNumSlots, kOldLead, kHwRing);
    sim.RunProducer();
    uint64_t miss = kNoMiss;
    for (uint32_t c = 0; c < 200 && miss == kNoMiss; ++c) {
        const uint32_t delta = (c % 20 == 19) ? 3 * kGroup : kGroup;
        miss = sim.Refill(delta);
        sim.RunProducer();
    }
    EXPECT_NE(miss, kNoMiss)
        << "a single 3-group coalesce among nominal traffic must hole";
}

// -----------------------------------------------------------------------------
// Fix-direction validation (NOT applied to the shipped geometry yet): sizing
// the lead for the worst expected coalescing closes the hole. If IT can
// coalesce K groups, lead must be >= ring + K*group.
// -----------------------------------------------------------------------------

TEST(TxRefillCoverage, LeadSizedForMaxCoalesceIsHoleFree) {
    constexpr uint32_t kMaxCoalesceGroups = 6;  // matches observed RX coalescing
    const uint32_t safeLead = kHwRing + kMaxCoalesceGroups * kGroup;  // 84
    for (uint32_t groups = 1; groups <= kMaxCoalesceGroups; ++groups) {
        EXPECT_EQ(DriveSteady(safeLead, kHwRing, groups * kGroup, /*cycles=*/4000),
                  kNoMiss)
            << "safeLead=" << safeLead << " groups=" << groups;
    }
    // And it still fails one group beyond the budget — the bound is tight.
    EXPECT_NE(DriveSteady(safeLead, kHwRing, (kMaxCoalesceGroups + 1) * kGroup,
                          /*cycles=*/8),
              kNoMiss);
}

// The fix: the CURRENT geometry (lead 84, slack 36) covers the captured
// hardware worst case and a full six-group coalesced completion.
TEST(TxRefillCoverage, CurrentGeometryCoversCapturedAndSixGroupCoalesce) {
    EXPECT_EQ(DriveSteady(kLead, kHwRing, 13, /*cycles=*/8000), kNoMiss);
    for (uint32_t groups = 1; groups <= 6; ++groups) {
        EXPECT_EQ(DriveSteady(kLead, kHwRing, groups * kGroup, /*cycles=*/4000),
                  kNoMiss)
            << "groups=" << groups;
    }
    // Still holes one group beyond the slack budget — the bound stays tight.
    EXPECT_NE(DriveSteady(kLead, kHwRing, 7 * kGroup, /*cycles=*/8), kNoMiss);
}

// Documents the exact relationship the geometry must satisfy.
TEST(TxRefillCoverage, CoverageBoundMatchesGeometryConstants) {
    EXPECT_EQ(kLead - kHwRing, AudioTimingGeometry::kTxPreparationSlackPackets);
    EXPECT_EQ(AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets,
              AudioTimingGeometry::kTxPreparationSlackPackets);
    // Current geometry tolerates six groups of coalescing.
    EXPECT_EQ((kLead - kHwRing) / kGroup, 6u);
}

} // namespace
