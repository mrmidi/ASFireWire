// RolePolicyTests.cpp — Layer 1 of RoleCoordinator (FW-6).
//
// Pins the conservative SKELETON behavior of EvaluateRolePolicy: it never issues
// a reset or cycle-master change yet; it only returns None or DeferForEvidence.
// FW-9 will extend/replace these with the full bm_work-style decision matrix.

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

TEST(RolePolicyTests, EvidencePresent_SkeletonIsInert_NoReset) {
    const auto t = MakeTopo(0, 1, 1);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::CapableByBIB;
    const auto a = EvaluateRolePolicy(in);
    EXPECT_EQ(a.kind, RoleAction::Kind::None);          // skeleton must not act yet
    EXPECT_EQ(a.reset, RoleResetFlavor::None);          // and never issues a reset
}

TEST(RolePolicyTests, CycleStartObserved_ClearsDefer) {
    const auto t = MakeTopo(0, 1, 1);
    RoleInputs in{};
    in.topo = &t;
    in.rootCap = RootCapability::Unknown;
    in.cycles = CycleObservation{.cycleStartObserved = true, .cycleLostObserved = false};
    EXPECT_NE(EvaluateRolePolicy(in).kind, RoleAction::Kind::DeferForEvidence);
}
