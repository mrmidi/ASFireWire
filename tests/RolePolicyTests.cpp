// RolePolicyTests.cpp — Layer 1 of RoleCoordinator (FW-6).
//
// Pins the FW-9/FW-10 role decisions: root capability evidence becomes local
// cycleMaster, remote CMSTR, force-root, or defer/no-op decisions.

#include <cstdint>

#include <gtest/gtest.h>

#include "ASFWDriver/Bus/Role/RolePolicy.hpp"

using namespace ASFW::Driver;
using ASFW::Driver::Role::CycleObservation;
using ASFW::Driver::Role::EvaluateRolePolicy;
using ASFW::Driver::Role::RoleAction;
using ASFW::Driver::Role::RoleInputs;
using ASFW::Driver::Role::RoleResetFlavor;
using ASFW::Driver::Role::RootCapability;
using ASFW::Driver::Role::RootBibReadStatus;
using ASFW::Driver::Role::DeriveRootCapabilityVerdict;

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

TEST(RolePolicyTests, RemoteCmcRoot_EnablesRemoteCycleMaster) {
    const auto t = MakeTopo(0, 1, 1);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    const auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::EnableRemoteCycleMaster);
    EXPECT_EQ(a.targetRoot, 1U);
    EXPECT_EQ(a.reset, RoleResetFlavor::None);
}

TEST(RolePolicyTests, CycleStartOnlyRootAcceptanceDoesNotWriteCmstr) {
    const auto t = MakeTopo(0, 1, 1);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::FunctioningByCycleStart;
    in.cycles = CycleObservation{.cycleStartObserved = true, .cycleLostObserved = false};
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::None);
}

TEST(RolePolicyTests, LocalRootAndLocalCmc_EnableLocalCycleMaster) {
    const auto t = MakeTopo(0, 0, 0);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    in.localCmcCapable = true;
    const auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::EnableLocalCycleMaster);
    EXPECT_EQ(a.targetRoot, 0U);
}

TEST(RolePolicyTests, RemoteIncapableRoot_ForcesLocalOnlyWhenLocalCmc) {
    const auto t = MakeTopo(0, 1, 0);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::IncapableByBIB;
    in.localCmcCapable = true;
    auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::ForceRootAndReset);
    EXPECT_EQ(a.targetRoot, 0U);
    EXPECT_EQ(a.reset, RoleResetFlavor::Short);

    in.localCmcCapable = false;
    a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::MarkRootBadOrUnknown);
}

TEST(RolePolicyTests, RemoteBadRoot_ForcesLocalOnlyWhenLocalCmc) {
    const auto t = MakeTopo(0, 1, 0);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::BadOrNonResponsive;
    in.localCmcCapable = true;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::ForceRootAndReset);

    in.localCmcCapable = false;
    EXPECT_EQ(EvaluateRolePolicy(in).kind, RoleAction::Kind::MarkRootBadOrUnknown);
}

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
