// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025 ASFW Project
//
// Host tests for the Milestone 2 post-reset timing core. Pure logic: the clock
// (nowNs) is supplied to every call, so boundaries are exact and no real time
// passes. See ASFWDriver/Bus/Timing/PostResetTiming.hpp for the model.

#include <gtest/gtest.h>

#include "ASFWDriver/Bus/Timing/IsoAllocationGate.hpp"
#include "ASFWDriver/Bus/Timing/PostResetTimingCoordinator.hpp"

using namespace ASFW::Bus::Timing;

namespace {
constexpr uint32_t kGen = 7;
constexpr uint64_t kT0 = 10'000'000'000ULL; // arbitrary Self-ID completion anchor (ns)
constexpr uint64_t kMs = kNanosecondsPerMillisecond;
} // namespace

// ---------------------------------------------------------------------------
// Anchor lifecycle
// ---------------------------------------------------------------------------

TEST(PostResetTiming, MissingState_GatesClosed) {
    PostResetTimingCoordinator c;
    const auto r = c.CheckGate(kGen, TimingGate::BMIncumbentContention, kT0);
    EXPECT_EQ(r.state, TimingGateState::Closed);
    EXPECT_FALSE(r.allowed);
}

TEST(PostResetTiming, OnBusResetStarted_InvalidatesPreviousState) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    EXPECT_TRUE(c.State().valid);

    c.OnBusResetStarted(kGen, kT0 + 5 * kMs);
    EXPECT_FALSE(c.State().valid);
    EXPECT_FALSE(c.State().selfIdComplete);

    // Even the incumbent (T+0) gate is Closed with no Self-ID completion.
    const auto r = c.CheckGate(kGen, TimingGate::BMIncumbentContention, kT0 + 100 * kMs);
    EXPECT_EQ(r.state, TimingGateState::Closed);
}

TEST(PostResetTiming, OnSelfIDComplete_ComputesExpectedGateTimes) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto& s = c.State();
    EXPECT_EQ(s.bmIncumbentAllowedNs, kT0);
    EXPECT_EQ(s.bmNonIncumbentAllowedNs, kT0 + 125 * kMs);
    EXPECT_EQ(s.irmFallbackAllowedNs, kT0 + 625 * kMs);
    EXPECT_EQ(s.newIsoAllocationAllowedNs, kT0 + 1000 * kMs);
}

// ---------------------------------------------------------------------------
// Per-gate boundaries
// ---------------------------------------------------------------------------

TEST(PostResetTiming, IncumbentGate_OpenImmediately) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = c.CheckGate(kGen, TimingGate::BMIncumbentContention, kT0);
    EXPECT_EQ(r.state, TimingGateState::Open);
    EXPECT_TRUE(r.allowed);
    EXPECT_EQ(r.remainingNs, 0u);
}

TEST(PostResetTiming, NonIncumbentGate_ClosedBefore125ms) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = c.CheckGate(kGen, TimingGate::BMNonIncumbentContention, kT0 + 124 * kMs);
    EXPECT_EQ(r.state, TimingGateState::Closed);
    EXPECT_FALSE(r.allowed);
    EXPECT_EQ(r.remainingNs, kMs);
}

TEST(PostResetTiming, NonIncumbentGate_OpenAt125ms) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = c.CheckGate(kGen, TimingGate::BMNonIncumbentContention, kT0 + 125 * kMs);
    EXPECT_EQ(r.state, TimingGateState::Open);
    EXPECT_TRUE(r.allowed);
}

TEST(PostResetTiming, IRMFallbackGate_ClosedBefore625ms) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    EXPECT_FALSE(c.CheckGate(kGen, TimingGate::IRMFallbackCheck, kT0 + 624 * kMs).allowed);
}

TEST(PostResetTiming, IRMFallbackGate_OpenAt625ms) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    EXPECT_TRUE(c.CheckGate(kGen, TimingGate::IRMFallbackCheck, kT0 + 625 * kMs).allowed);
}

TEST(PostResetTiming, NewIsoGate_ClosedBefore1000ms) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = c.CheckNewIsoAllocationGate(kGen, kT0 + 999 * kMs);
    EXPECT_FALSE(r.allowed);
    EXPECT_EQ(r.remainingNs, kMs);
}

TEST(PostResetTiming, NewIsoGate_OpenAt1000ms) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    EXPECT_TRUE(c.CheckNewIsoAllocationGate(kGen, kT0 + 1000 * kMs).allowed);
}

TEST(PostResetTiming, GenerationMismatch_ReturnsExpiredGeneration) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = c.CheckGate(kGen + 1, TimingGate::BMIncumbentContention, kT0 + 2000 * kMs);
    EXPECT_EQ(r.state, TimingGateState::ExpiredGeneration);
    EXPECT_FALSE(r.allowed);
}

// ---------------------------------------------------------------------------
// BM candidate class
// ---------------------------------------------------------------------------

TEST(PostResetTiming, BMCandidate_NotCandidate_SuppressedByRolePolicy) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = c.CheckBMGate(kGen, BMCandidateClass::NotCandidate, kT0 + 5000 * kMs);
    EXPECT_EQ(r.state, TimingGateState::SuppressedByRolePolicy);
    EXPECT_FALSE(r.allowed);
}

TEST(PostResetTiming, BMCandidate_Incumbent_UsesImmediateGate) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = c.CheckBMGate(kGen, BMCandidateClass::Incumbent, kT0);
    EXPECT_EQ(r.state, TimingGateState::Open);
    EXPECT_TRUE(r.allowed);
}

TEST(PostResetTiming, BMCandidate_NonIncumbent_Uses125msGate) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    EXPECT_FALSE(c.CheckBMGate(kGen, BMCandidateClass::NonIncumbent, kT0 + 124 * kMs).allowed);
    EXPECT_TRUE(c.CheckBMGate(kGen, BMCandidateClass::NonIncumbent, kT0 + 125 * kMs).allowed);
}

// ---------------------------------------------------------------------------
// ISO allocation gate helper (diagnostics-only in M2)
// ---------------------------------------------------------------------------

TEST(IsoAllocationGate, TopologyInvalid_SuppressesEvenIfTimeOpen) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = CheckIsoAllocationAllowed(c, kGen, /*topologyValid=*/false, kT0 + 5000 * kMs);
    EXPECT_EQ(r.status, IsoAllocationGateStatus::TopologyInvalid);
}

TEST(IsoAllocationGate, NoSelfID_ReportsNoSelfIDCompletion) {
    PostResetTimingCoordinator c;
    c.OnBusResetStarted(kGen, kT0);
    const auto r = CheckIsoAllocationAllowed(c, kGen, /*topologyValid=*/true, kT0 + 5000 * kMs);
    EXPECT_EQ(r.status, IsoAllocationGateStatus::NoSelfIDCompletion);
}

TEST(IsoAllocationGate, WaitsForOneSecond) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = CheckIsoAllocationAllowed(c, kGen, /*topologyValid=*/true, kT0 + 999 * kMs);
    EXPECT_EQ(r.status, IsoAllocationGateStatus::WaitingForOneSecondGate);
    EXPECT_EQ(r.remainingNs, kMs);
}

TEST(IsoAllocationGate, AllowsAfterOneSecond) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = CheckIsoAllocationAllowed(c, kGen, /*topologyValid=*/true, kT0 + 1000 * kMs);
    EXPECT_EQ(r.status, IsoAllocationGateStatus::Allowed);
}

TEST(IsoAllocationGate, GenerationMismatchRejected) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto r = CheckIsoAllocationAllowed(c, kGen + 1, /*topologyValid=*/true, kT0 + 2000 * kMs);
    EXPECT_EQ(r.status, IsoAllocationGateStatus::GenerationMismatch);
}

// ---------------------------------------------------------------------------
// Bus-reset races
// ---------------------------------------------------------------------------

TEST(PostResetTiming, SecondBusResetClearsPreviousSelfIdGate) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    EXPECT_TRUE(c.CheckGate(kGen, TimingGate::BMIncumbentContention, kT0).allowed);

    // A newer reset edge arrives; gates must close until the next Self-ID.
    c.OnBusResetStarted(kGen + 1, kT0 + 10 * kMs);
    EXPECT_FALSE(c.CheckGate(kGen, TimingGate::BMIncumbentContention, kT0 + 20 * kMs).allowed);
    EXPECT_FALSE(c.CheckGate(kGen + 1, TimingGate::BMIncumbentContention, kT0 + 20 * kMs).allowed);
}

TEST(PostResetTiming, OldGenerationTimerDoesNotFireAction) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    // A newer generation completes; an old-generation check must report expired
    // rather than (re)opening, so a stale deferred action never runs.
    c.OnSelfIDComplete(kGen + 1, kT0 + 200 * kMs);
    const auto r = c.CheckGate(kGen, TimingGate::NewIsoAllocation, kT0 + 5000 * kMs);
    EXPECT_EQ(r.state, TimingGateState::ExpiredGeneration);
    EXPECT_FALSE(r.allowed);
}

TEST(PostResetTiming, SelfIDCompleteWithoutTopologyStillArmsGates) {
    // Topology graph build is NOT a precondition: OnSelfIDComplete alone arms the
    // gates (the coordinator never sees topology). Documents invariants 1 and 2.
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    EXPECT_TRUE(c.State().valid);
    EXPECT_TRUE(c.CheckGate(kGen, TimingGate::BMIncumbentContention, kT0).allowed);
    EXPECT_TRUE(c.CheckNewIsoAllocationGate(kGen, kT0 + 1000 * kMs).allowed);
}

// ---------------------------------------------------------------------------
// Diagnostics snapshot
// ---------------------------------------------------------------------------

TEST(PostResetTiming, Snapshot_ReportsGateStatesAndRemaining) {
    PostResetTimingCoordinator c;
    c.OnSelfIDComplete(kGen, kT0);
    const auto d = c.Snapshot(kT0 + 200 * kMs); // 200 ms after Self-ID
    EXPECT_TRUE(d.valid);
    EXPECT_TRUE(d.selfIdComplete);
    EXPECT_EQ(d.generation, kGen);
    EXPECT_EQ(d.ageSinceSelfIdNs, 200 * kMs);
    EXPECT_EQ(d.incumbentBMGate, TimingGateState::Open);
    EXPECT_EQ(d.nonIncumbentBMGate, TimingGateState::Open);     // 200 >= 125
    EXPECT_EQ(d.irmFallbackGate, TimingGateState::Closed);      // 200 < 625
    EXPECT_EQ(d.newIsoAllocationGate, TimingGateState::Closed); // 200 < 1000
    EXPECT_EQ(d.irmFallbackRemainingNs, (625 - 200) * kMs);
    EXPECT_EQ(d.newIsoAllocationRemainingNs, (1000 - 200) * kMs);
}

TEST(PostResetTiming, Snapshot_MissingState_AllClosed) {
    PostResetTimingCoordinator c;
    const auto d = c.Snapshot(kT0);
    EXPECT_FALSE(d.valid);
    EXPECT_EQ(d.incumbentBMGate, TimingGateState::Closed);
    EXPECT_EQ(d.newIsoAllocationGate, TimingGateState::Closed);
}
