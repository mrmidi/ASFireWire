// RoleCoordinatorTests.cpp — Layer 2 of RoleCoordinator (FW-6).
//
// Exercises the thin stateful actor: generation-safety (stale evidence dropped),
// executor dispatch, and the same-topology ping-pong guard. A synthetic policy
// (captureless lambda → function pointer) drives reset/CMSTR actions so the
// guard and dispatch can be tested before FW-9 fills in EvaluateRolePolicy.

#include <cstdint>

#include <gtest/gtest.h>

#include "ASFWDriver/Bus/Role/RoleCoordinator.hpp"

using namespace ASFW::Driver;
using namespace ASFW::Driver::Role;
using Level = ASFW::FW::FullBMActivityLevel;

namespace {

TopologySnapshot MakeTopo(uint8_t local, uint8_t root, uint8_t irm, uint8_t nodeCount) {
    TopologySnapshot t{};
    t.localNodeId = local;
    t.rootNodeId = root;
    t.irmNodeId = irm;
    t.nodeCount = nodeCount;
    return t;
}

struct FakeReset : IPhyConfigReset {
    int calls{0};
    uint8_t lastTarget{0};
    uint32_t lastGen{0};
    RoleResetFlavor lastFlavor{RoleResetFlavor::None};
    void ForceRootAndReset(uint8_t target, RoleResetFlavor flavor, uint8_t /*gap*/,
                           uint32_t generation) override {
        ++calls;
        lastTarget = target;
        lastFlavor = flavor;
        lastGen = generation;
    }
};

struct FakeCsr : IRemoteCsrWriter {
    int calls{0};
    uint8_t lastNode{0};
    uint32_t lastGen{0};
    void EnableRemoteCycleMaster(uint8_t node, uint32_t generation) override {
        ++calls;
        lastNode = node;
        lastGen = generation;
    }
};

struct FakeContender : IContenderControl {
    int localCycleMasterCalls{0};
    int delegateCalls{0};
    uint8_t lastTarget{0};
    uint32_t lastGen{0};
    void EnableLocalCycleMaster(uint32_t generation) override {
        ++localCycleMasterCalls;
        lastGen = generation;
    }
    void ClearLocalContenderAndDelegate(uint8_t targetRoot, uint32_t generation) override {
        ++delegateCalls;
        lastTarget = targetRoot;
        lastGen = generation;
    }
};

// Synthetic policies (captureless → convert to PolicyFn).
RoleAction AlwaysForceRoot(const RoleInputs& /*in*/) noexcept {
    RoleAction a{};
    a.kind = RoleAction::Kind::ForceRootAndReset;
    a.targetRoot = 2;
    a.reset = RoleResetFlavor::Long;
    a.reason = "test: force root";
    return a;
}

RoleAction AlwaysRemoteCmstr(const RoleInputs& in) noexcept {
    RoleAction a{};
    a.kind = RoleAction::Kind::EnableRemoteCycleMaster;
    a.targetRoot = (in.topo != nullptr) ? in.topo->rootNodeId.value_or(0) : 0;
    return a;
}

} // namespace

TEST(RoleCoordinatorTests, DefaultPolicy_DefersWhenNoEvidence) {
    RoleCoordinator rc;
    rc.OnTopologyChanged(1, MakeTopo(0, 1, 1, 2));
    EXPECT_EQ(rc.Generation(), 1u);
    EXPECT_TRUE(rc.HaveTopology());
    EXPECT_EQ(rc.LastAction().kind, RoleAction::Kind::DeferForEvidence);
}

TEST(RoleCoordinatorTests, StaleEvidenceDropped_CurrentApplied) {
    RoleCoordinator rc;
    rc.OnTopologyChanged(5, MakeTopo(0, 1, 1, 2));

    // Evidence tagged with an older generation must be ignored.
    rc.OnRootCapability(4, RootCapability::CapableByBIB);
    EXPECT_EQ(rc.LastAction().kind, RoleAction::Kind::DeferForEvidence);

    // Current-generation evidence is applied: Apple-default accepts a remote CMC
    // root with no action (no remote CMSTR). The Defer→None transition proves the
    // current evidence was applied (vs. the stale one being dropped above).
    rc.OnRootCapability(5, RootCapability::CapableByBIB);
    EXPECT_EQ(rc.LastAction().kind, RoleAction::Kind::None);
}

TEST(RoleCoordinatorTests, StructuredEvidence_CurrentApplied_StaleDropped) {
    RoleCoordinator rc;
    rc.OnTopologyChanged(6, MakeTopo(0, 1, 1, 2));

    RootCapabilityEvidence stale{};
    stale.generation = 5;
    stale.rootNodeId = 1;
    stale.bibReadStatus = RootBibReadStatus::Success;
    stale.cmcKnown = true;
    stale.cmc = true;
    rc.OnRootCapabilityEvidence(5, stale);
    EXPECT_EQ(rc.LastAction().kind, RoleAction::Kind::DeferForEvidence);

    RootCapabilityEvidence current = stale;
    current.generation = 6;
    rc.OnRootCapabilityEvidence(6, current);
    EXPECT_EQ(rc.LastRootEvidence().verdict, RootCapability::CapableByBIB);
    // Apple-default: verified remote CMC root is accepted with no action.
    EXPECT_EQ(rc.LastAction().kind, RoleAction::Kind::None);
}

TEST(RoleCoordinatorTests, StructuredEvidence_DerivesCycleFallbacks) {
    RoleCoordinator rc;
    rc.OnTopologyChanged(8, MakeTopo(0, 1, 1, 2));

    RootCapabilityEvidence functioning{};
    functioning.generation = 8;
    functioning.rootNodeId = 1;
    functioning.bibReadStatus = RootBibReadStatus::Timeout;
    functioning.cycleObservationComplete = true;
    functioning.cycles = CycleObservation{.cycleStartObserved = true, .cycleLostObserved = false};
    rc.OnRootCapabilityEvidence(8, functioning);
    EXPECT_EQ(rc.LastRootEvidence().verdict, RootCapability::FunctioningByCycleStart);

    RootCapabilityEvidence bad = functioning;
    bad.cycles = CycleObservation{.cycleStartObserved = false, .cycleLostObserved = true};
    rc.OnRootCapabilityEvidence(8, bad);
    EXPECT_EQ(rc.LastRootEvidence().verdict, RootCapability::BadOrNonResponsive);
}

TEST(RoleCoordinatorTests, RemoteCmstrDispatchedToExecutor) {
    FakeCsr csr;
    RoleCoordinator::Executors ex{};
    ex.csr = &csr;
    RoleCoordinator rc(ex, &AlwaysRemoteCmstr);

    rc.OnTopologyChanged(7, MakeTopo(0, 2, 2, 2));
    EXPECT_EQ(csr.calls, 1);
    EXPECT_EQ(csr.lastNode, 2);
    EXPECT_EQ(csr.lastGen, 7u);
}

TEST(RoleCoordinatorTests, LocalCmcRootDispatchedToLocalCycleMasterExecutor) {
    FakeContender contender;
    RoleCoordinator::Executors ex{};
    ex.contender = &contender;
    RoleCoordinator rc(ex);
    // Enabling local cycleMaster is gated; unlock active root/gap policy.
    rc.SetActivityLevel(Level::GapPolicyAllowed);

    rc.OnTopologyChanged(10, MakeTopo(0, 0, 0, 1));
    rc.OnLocalCycleMasterCapability(10, true);
    rc.OnRootCapability(10, RootCapability::CapableByBIB);

    EXPECT_EQ(contender.localCycleMasterCalls, 1);
    EXPECT_EQ(contender.lastGen, 10U);
}

TEST(RoleCoordinatorTests, PingPongGuardStopsAfterMax) {
    FakeReset reset;
    RoleCoordinator::Executors ex{};
    ex.reset = &reset;
    RoleCoordinator rc(ex, &AlwaysForceRoot);

    const auto topoA = MakeTopo(0, 1, 1, 2);
    // Same physical topology across successive generations → retries accumulate.
    for (uint32_t gen = 1; gen <= 8; ++gen) {
        rc.OnTopologyChanged(gen, topoA);
    }
    EXPECT_EQ(reset.calls, static_cast<int>(RoleCoordinator::kMaxSameTopologyResets));
    EXPECT_EQ(reset.lastFlavor, RoleResetFlavor::Long);
    EXPECT_EQ(rc.LastAction().kind, RoleAction::Kind::None); // suppressed by guard
}

TEST(RoleCoordinatorTests, GuardResetsWhenTopologyChanges) {
    FakeReset reset;
    RoleCoordinator::Executors ex{};
    ex.reset = &reset;
    RoleCoordinator rc(ex, &AlwaysForceRoot);

    const auto topoA = MakeTopo(0, 1, 1, 2);
    for (uint32_t gen = 1; gen <= 8; ++gen) {
        rc.OnTopologyChanged(gen, topoA);
    }
    EXPECT_EQ(reset.calls, static_cast<int>(RoleCoordinator::kMaxSameTopologyResets));

    // A different physical topology resets the guard; resets resume.
    rc.OnTopologyChanged(9, MakeTopo(0, 2, 2, 3));
    EXPECT_EQ(reset.calls, static_cast<int>(RoleCoordinator::kMaxSameTopologyResets) + 1);
    EXPECT_EQ(rc.ResetRetriesThisTopology(), 1u);
}
