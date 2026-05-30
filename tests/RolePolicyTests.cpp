// RolePolicyTests.cpp — Layer 1 of RoleCoordinator (FW-6).
//
// Pins the Apple-compatible default policy (FW-17): root CMC is diagnostics-only,
// remote STATE_SET.cmstr is experimental-only, and every mutating action is gated
// by FullBMActivityLevel. Linux-style force-root on verified CMC=0 is opt-in.
// See Linear FW-16 (Linux) / FW-17 (Apple) and [[apple-ignores-cmc-irm-probing]].

#include <cstdint>

#include <gtest/gtest.h>

#include "ASFWDriver/Bus/Role/RolePolicy.hpp"

using namespace ASFW::Driver;
using ASFW::Driver::Role::CycleObservation;
using ASFW::Driver::Role::DeriveRootCapabilityVerdict;
using ASFW::Driver::Role::EvaluateRolePolicy;
using ASFW::Driver::Role::RoleAction;
using ASFW::Driver::Role::RoleInputs;
using ASFW::Driver::Role::RoleResetFlavor;
using ASFW::Driver::Role::RootBibReadStatus;
using ASFW::Driver::Role::RootCapability;
using Level = ASFW::FW::FullBMActivityLevel;

namespace {

TopologySnapshot MakeTopo(uint8_t local, uint8_t root, uint8_t irm, uint8_t nodeCount = 2) {
    TopologySnapshot t{};
    t.localNodeId = local;
    t.rootNodeId = root;
    t.irmNodeId = irm;
    t.nodeCount = nodeCount;
    return t;
}

} // namespace

// ---- Boundary / evidence gating ------------------------------------------------

TEST(RolePolicyTests, NoTopologyPtr_ReturnsNone) {
    RoleInputs in{};
    in.topo = nullptr;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::None);
}

TEST(RolePolicyTests, MissingRoot_ReturnsNone) {
    TopologySnapshot t{};
    t.localNodeId = 0; // rootNodeId left unset
    RoleInputs in{};
    in.topo = &t;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::None);
}

TEST(RolePolicyTests, UnknownCapabilityNoCycles_Defers) {
    const auto t = MakeTopo(0, 1, 1);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::Unknown;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::DeferForEvidence);
}

// ---- Apple-compatible default (ObserveOnly): CMC is diagnostics-only -----------

// Remote CMC=1 root: accept it. Apple ignores CMC; neither reference writes a
// remote STATE_SET.cmstr as the normal cycle-master path.
TEST(RolePolicyTests, Saffire_VerifiedCmcRoot_AcceptedNoRemoteCmstr) {
    const auto t = MakeTopo(/*local*/ 0, /*root*/ 2, /*irm*/ 2);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    in.activity = Level::ObserveOnly;
    const auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::None);
}

// Remote CMC=1 root stays accept even with force-root unlocked: only the top rung
// (RemoteCmstrAllowed) may ever emit a remote CMSTR write.
TEST(RolePolicyTests, CmcRoot_NoRemoteCmstrBelowTopRung) {
    const auto t = MakeTopo(0, 2, 2);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    in.activity = Level::ForceRootAllowed; // unlocked for force-root, NOT cmstr
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::None);
}

// Apogee scenario: verified CMC=0 AND cycle starts observed → record verdict,
// take NO bus action in the Apple-compatible default.
TEST(RolePolicyTests, Apogee_Cmc0PlusCycleStart_NoBusMutation) {
    const auto t = MakeTopo(0, 2, 2);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::IncapableByBIB;
    in.cycles = CycleObservation{.cycleStartObserved = true, .cycleLostObserved = false};
    in.activity = Level::ObserveOnly;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::MarkRootBadOrUnknown);
}

// CycleStart-only acceptance (BIB unread but cycles seen) is diagnostic; never a
// trigger and never a remote CMSTR.
TEST(RolePolicyTests, CycleStartOnlyRootAcceptance_NoAction) {
    const auto t = MakeTopo(0, 1, 1);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::FunctioningByCycleStart;
    in.cycles = CycleObservation{.cycleStartObserved = true, .cycleLostObserved = false};
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::None);
}

// Even with force-root unlocked, Apple-compatible default does NOT act on CMC=0.
TEST(RolePolicyTests, Cmc0_AppleDefaultNeverForceRootEvenWhenUnlocked) {
    const auto t = MakeTopo(0, 2, 0); // local==IRM, capable
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::IncapableByBIB;
    in.localCmcCapable = true;
    in.activity = Level::ForceRootAllowed;
    in.linuxStyleCmcForceRoot = false; // Apple default
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::MarkRootBadOrUnknown);
}

// ---- Local node is root --------------------------------------------------------

// Below GapPolicyAllowed the local-cycle-master enable is reported, not executed.
TEST(RolePolicyTests, LocalRoot_ObserveOnly_ReportsButDoesNotEnable) {
    const auto t = MakeTopo(0, 0, 0);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    in.localCmcCapable = true;
    in.activity = Level::ObserveOnly;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::None);
}

TEST(RolePolicyTests, LocalRoot_GapPolicyAllowed_EnablesLocalCycleMaster) {
    const auto t = MakeTopo(0, 0, 0);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    in.localCmcCapable = true;
    in.activity = Level::GapPolicyAllowed;
    const auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::EnableLocalCycleMaster);
    EXPECT_EQ(a.targetRoot, 0U);
}

// ---- Experimental / opt-in paths ----------------------------------------------

// Remote CMSTR is reachable ONLY at the top rung (RemoteCmstrAllowed).
TEST(RolePolicyTests, RemoteCmstr_OnlyAtTopRung) {
    const auto t = MakeTopo(0, 1, 1);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    in.activity = Level::RemoteCmstrAllowed;
    const auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::EnableRemoteCycleMaster);
    EXPECT_EQ(a.targetRoot, 1U);
    EXPECT_EQ(a.reset, RoleResetFlavor::None);
}

// Linux-style CMC=0 force-root: requires the experiment flag AND ForceRootAllowed
// AND local==IRM AND local CMC-capable. Renamed to make non-Apple status explicit.
TEST(RolePolicyTests, LinuxStyle_Cmc0_ForceRootOnlyWhenExplicitlyUnlockedAndLocalIRM) {
    const auto t = MakeTopo(/*local*/ 0, /*root*/ 1, /*irm*/ 0);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::IncapableByBIB;
    in.localCmcCapable = true;
    in.linuxStyleCmcForceRoot = true;
    in.activity = Level::ForceRootAllowed;
    in.irmNodeId = 0; // local==IRM (the policy reads in.irmNodeId, not topo)
    auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::ForceRootAndReset);
    EXPECT_EQ(a.targetRoot, 0U);
    EXPECT_EQ(a.reset, RoleResetFlavor::Short);

    // Not the IRM → no force-root even with the experiment on.
    in.irmNodeId = 1;
    a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::MarkRootBadOrUnknown);
}

// Apple-shaped force-root: a bad/nonresponsive root, local is IRM + capable,
// force-root unlocked. No experiment flag needed (this mirrors fBadIRMsKnown).
TEST(RolePolicyTests, BadRoot_ForceRootWhenLocalIRMCapableAndUnlocked) {
    const auto t = MakeTopo(0, 1, 0); // local==IRM
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::BadOrNonResponsive;
    in.localCmcCapable = true;
    in.activity = Level::ForceRootAllowed;
    in.irmNodeId = 0; // local==IRM
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::ForceRootAndReset);
}

TEST(RolePolicyTests, BadRoot_ObserveOnly_NoMutation) {
    const auto t = MakeTopo(0, 1, 0);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::BadOrNonResponsive;
    in.localCmcCapable = true;
    in.activity = Level::ObserveOnly;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::MarkRootBadOrUnknown);
}

// ---- Verdict derivation (FW-8) -------------------------------------------------

TEST(RolePolicyTests, RootCapabilityEvidence_DerivesBibVerdicts) {
    EXPECT_EQ(DeriveRootCapabilityVerdict(RootBibReadStatus::Success, true, true, false, {}),
              RootCapability::CapableByBIB);
    EXPECT_EQ(DeriveRootCapabilityVerdict(RootBibReadStatus::Success, true, false, false, {}),
              RootCapability::IncapableByBIB);
}

TEST(RolePolicyTests, RootCapabilityEvidence_DerivesCycleWindowVerdicts) {
    EXPECT_EQ(DeriveRootCapabilityVerdict(
                  RootBibReadStatus::Timeout, false, false, true,
                  CycleObservation{.cycleStartObserved = true, .cycleLostObserved = false}),
              RootCapability::FunctioningByCycleStart);
    EXPECT_EQ(DeriveRootCapabilityVerdict(
                  RootBibReadStatus::Failed, false, false, true,
                  CycleObservation{.cycleStartObserved = false, .cycleLostObserved = true}),
              RootCapability::BadOrNonResponsive);
    EXPECT_EQ(DeriveRootCapabilityVerdict(RootBibReadStatus::AbortedByReset, false, false, true,
                                          {}),
              RootCapability::Unknown);
}
