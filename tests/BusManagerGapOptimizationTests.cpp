#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "ASFWDriver/Bus/BusManager.hpp"

using namespace ASFW::Driver;

namespace {

uint32_t MakeBaseSelfID(const uint8_t phyId, const uint8_t gapCount, const bool contender = false) {
    uint32_t quadlet = 0x80000000U;
    quadlet |= (static_cast<uint32_t>(phyId) & 0x3FU) << 24U;
    quadlet |= 1U << 22U;
    quadlet |= (static_cast<uint32_t>(gapCount) & 0x3FU) << 16U;
    quadlet |= 0x2U << 14U;
    if (contender) {
        quadlet |= 1U << 11U;
    }
    return quadlet;
}

TopologySnapshot MakeTopology(const std::optional<uint8_t> localNodeId,
                              const std::optional<uint8_t> irmNodeId,
                              const uint8_t maxHopsFromRoot) {
    TopologySnapshot topology{};
    topology.localNodeId = localNodeId;
    topology.irmNodeId = irmNodeId;
    topology.maxHopsFromRoot = maxHopsFromRoot;
    return topology;
}

} // namespace

TEST(BusManagerGapOptimizationTests, InconsistentObservedBaseGapsForceConservative63) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);

    const auto topology = MakeTopology(1U, 1U, 4U);
    const auto decision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 10U), MakeBaseSelfID(1U, 20U)});

    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->reason, BusManager::GapDecisionReason::MismatchForce63);
    EXPECT_EQ(decision->gapCount, 63U);
}

TEST(BusManagerGapOptimizationTests, ObservedZeroGapRetoolsToCurrentTargetGap) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);

    const auto topology = MakeTopology(0U, 0U, 4U);
    const auto decision =
        busManager.EvaluateGapPolicy(topology, {MakeBaseSelfID(0U, 0U), MakeBaseSelfID(1U, 0U)});

    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->reason, BusManager::GapDecisionReason::ZeroObservedGap);
    EXPECT_EQ(decision->gapCount, 10U);
}

TEST(BusManagerGapOptimizationTests, ObservedGapsMatchingPreviousGapNeedNoAction) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);

    const auto topology = MakeTopology(0U, 0U, 4U);
    const auto decision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 63U), MakeBaseSelfID(1U, 63U)});

    EXPECT_FALSE(decision.has_value());
}

TEST(BusManagerGapOptimizationTests, ObservedGapsMatchingTargetGapNeedNoAction) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);

    const auto topology = MakeTopology(0U, 0U, 4U);
    const auto decision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 10U), MakeBaseSelfID(1U, 10U)});

    EXPECT_FALSE(decision.has_value());
}

TEST(BusManagerGapOptimizationTests, ForcedGapDifferentFromPreviousReturnsDecision) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);
    busManager.SetForcedGapCount(21U);

    const auto topology = MakeTopology(0U, 0U, 4U);
    const auto decision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 63U), MakeBaseSelfID(1U, 63U)});

    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->reason, BusManager::GapDecisionReason::ForcedGap);
    EXPECT_EQ(decision->gapCount, 21U);
}

TEST(BusManagerGapOptimizationTests, NonManagerNodeSkipsGapOptimization) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);

    const auto topology = MakeTopology(0U, 1U, 4U);
    const auto decision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 10U), MakeBaseSelfID(1U, 20U)});

    EXPECT_FALSE(decision.has_value());
}

TEST(BusManagerGapOptimizationTests, FailedDispatchDoesNotAdvanceConfirmedGap) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);
    busManager.SetForcedGapCount(21U);

    const auto topology = MakeTopology(0U, 0U, 4U);
    const auto initialDecision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 63U), MakeBaseSelfID(1U, 63U)});

    ASSERT_TRUE(initialDecision.has_value());
    ASSERT_EQ(initialDecision->reason, BusManager::GapDecisionReason::ForcedGap);
    EXPECT_EQ(initialDecision->gapCount, 21U);

    busManager.NoteGapResetIssued(21U, BusManager::GapDecisionReason::ForcedGap);
    busManager.ClearInFlightGapReset();

    const auto retryDecision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 63U), MakeBaseSelfID(1U, 63U)});

    ASSERT_TRUE(retryDecision.has_value());
    EXPECT_EQ(retryDecision->reason, BusManager::GapDecisionReason::ForcedGap);
    EXPECT_EQ(retryDecision->gapCount, 21U);
}

TEST(BusManagerGapOptimizationTests, StableAcceptedGapCommitsOnlyAfterConsistentObservation) {
    BusManager busManager;
    busManager.SetGapOptimizationEnabled(true);
    busManager.SetForcedGapCount(21U);

    const auto topology = MakeTopology(0U, 0U, 4U);
    const auto initialDecision =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 63U), MakeBaseSelfID(1U, 63U)});

    ASSERT_TRUE(initialDecision.has_value());
    busManager.NoteGapResetIssued(21U, BusManager::GapDecisionReason::ForcedGap);

    const auto beforeStableCommit =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 63U), MakeBaseSelfID(1U, 63U)});
    ASSERT_TRUE(beforeStableCommit.has_value());
    EXPECT_EQ(beforeStableCommit->reason, BusManager::GapDecisionReason::ForcedGap);

    busManager.NoteStableGapObserved(21U);

    const auto afterStableCommit =
        busManager.EvaluateGapPolicy(topology,
                                     {MakeBaseSelfID(0U, 21U), MakeBaseSelfID(1U, 21U)});
    EXPECT_FALSE(afterStableCommit.has_value());
}
