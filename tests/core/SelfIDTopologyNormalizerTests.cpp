// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025 ASFW Project

#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

#include "ASFWDriver/Bus/SelfIDTopologyNormalizer.hpp"
#include "ASFWDriver/Bus/GapCountOptimizer.hpp"
#include "ASFWDriver/Bus/TopologyTypes.hpp"

using namespace ASFW::Driver;

namespace {

// Build a Self-ID node record with explicit port states. Records must be supplied
// to BuildPhysicalGraph ordered by physical_ID, 0..root (root = highest ID).
SelfIDNodeRecord MakeRecord(uint8_t physicalId, std::initializer_list<PortState> ports) {
    SelfIDNodeRecord record{};
    record.physicalId = physicalId;
    record.hasBasePacket = true;
    record.linkActive = true;
    uint8_t index = 0;
    for (const PortState state : ports) {
        record.ports[index++] = state;
    }
    record.portCount = static_cast<uint8_t>(ports.size());
    return record;
}

uint8_t DiameterOf(const std::vector<SelfIDNodeRecord>& records) {
    auto graph = SelfIDTopologyNormalizer::BuildPhysicalGraph(records, /*localPhysicalId=*/0);
    EXPECT_TRUE(graph.has_value());
    return graph->busDiameterHops;
}

SelfIDNodeRecord& SetRole(SelfIDNodeRecord& record, bool contender, bool linkActive) {
    record.contender = contender;
    record.linkActive = linkActive;
    return record;
}

} // namespace

// IRM is the highest physical-ID node that is BOTH a contender and link-active.
TEST(SelfIDTopologyNormalizerIRM, HighestLinkActiveContenderWins) {
    // Star: root=2 with two leaf children 0 and 1. Nodes 1 and 2 are contenders.
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),
        MakeRecord(1, {PortState::Parent}),
        MakeRecord(2, {PortState::Child, PortState::Child}),
    };
    SetRole(records[1], /*contender=*/true, /*linkActive=*/true);
    SetRole(records[2], /*contender=*/true, /*linkActive=*/true);

    auto graph = SelfIDTopologyNormalizer::BuildPhysicalGraph(records, /*localPhysicalId=*/0);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->irmId, 2);  // highest link-active contender
}

// A link-inactive PHY cannot host the IRM even if it asserts the contender bit;
// the election must fall through to the next-highest link-active contender.
TEST(SelfIDTopologyNormalizerIRM, LinkInactiveContenderIsSkipped) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),
        MakeRecord(1, {PortState::Parent}),
        MakeRecord(2, {PortState::Child, PortState::Child}),
    };
    SetRole(records[1], /*contender=*/true, /*linkActive=*/true);
    SetRole(records[2], /*contender=*/true, /*linkActive=*/false);  // root, link off

    auto graph = SelfIDTopologyNormalizer::BuildPhysicalGraph(records, /*localPhysicalId=*/0);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->irmId, 1);  // node 2 skipped (link inactive)
}

// No link-active contender on the bus -> no IRM.
TEST(SelfIDTopologyNormalizerIRM, NoEligibleContenderYieldsInvalidIRM) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),
        MakeRecord(1, {PortState::Child}),
    };
    SetRole(records[0], /*contender=*/false, /*linkActive=*/true);
    SetRole(records[1], /*contender=*/true, /*linkActive=*/false);  // contender but link off

    auto graph = SelfIDTopologyNormalizer::BuildPhysicalGraph(records, /*localPhysicalId=*/0);
    ASSERT_TRUE(graph.has_value());
    EXPECT_EQ(graph->irmId, kInvalidPhysicalId);
}

// A single-node bus has no cable hops -> gap optimization falls back to default 63.
TEST(SelfIDTopologyNormalizerDiameter, SingleNode_IsZeroHops) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {}),  // root, no connected ports
    };
    EXPECT_EQ(DiameterOf(records), 0);
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(DiameterOf(records)), 63);
}

// Two nodes directly connected = 1 hop (Table E.1: 1 hop -> gap 5).
TEST(SelfIDTopologyNormalizerDiameter, TwoNodes_IsOneHop) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),  // child of root
        MakeRecord(1, {PortState::Child}),   // root
    };
    EXPECT_EQ(DiameterOf(records), 1);
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(DiameterOf(records)), 5);
}

// Linear chain 0-1-2 (root=2): diameter 2.
TEST(SelfIDTopologyNormalizerDiameter, LinearThreeNodes_IsTwoHops) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),                  // leaf
        MakeRecord(1, {PortState::Child, PortState::Parent}),// middle
        MakeRecord(2, {PortState::Child}),                   // root
    };
    EXPECT_EQ(DiameterOf(records), 2);
}

// Star: root=2 with two children 0 and 1 (the Saffire ClientOnly topology).
// Longest path 0-2-1 = 2 hops (Table E.1: 2 hops -> gap 7).
TEST(SelfIDTopologyNormalizerDiameter, StarThreeNodes_IsTwoHops) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),                  // leaf child
        MakeRecord(1, {PortState::Parent}),                  // leaf child
        MakeRecord(2, {PortState::Child, PortState::Child}), // root, two children
    };
    EXPECT_EQ(DiameterOf(records), 2);
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(DiameterOf(records)), 7);
}

// Linear chain of five nodes (root=4 at one end): diameter 4.
TEST(SelfIDTopologyNormalizerDiameter, LinearFiveNodes_IsFourHops) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),
        MakeRecord(1, {PortState::Child, PortState::Parent}),
        MakeRecord(2, {PortState::Child, PortState::Parent}),
        MakeRecord(3, {PortState::Child, PortState::Parent}),
        MakeRecord(4, {PortState::Child}),  // root
    };
    EXPECT_EQ(DiameterOf(records), 4);
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(DiameterOf(records)), 10);
}

// The differentiating case: the root sits in the MIDDLE of the longest path, so
// the bus diameter (4) is strictly greater than the depth-from-root (2).
// Physical tree:  leaf(0)-(1)-ROOT(4)-(3)-leaf(2)
//   id0 leaf -> parent id1
//   id1      -> child id0,  parent id4
//   id2 leaf -> parent id3
//   id3      -> child id2,  parent id4
//   id4 root -> child id1,  child id3
// Depth from root(4): {4:0, 1:1, 3:1, 0:2, 2:2} -> max 2.
// Diameter: 0-1-4-3-2 = 4 hops. A depth-from-root implementation would wrongly
// report 2 here and pick gap 7 instead of the correct gap 10.
TEST(SelfIDTopologyNormalizerDiameter, RootInMiddle_DiameterExceedsDepthFromRoot) {
    std::vector<SelfIDNodeRecord> records = {
        MakeRecord(0, {PortState::Parent}),                  // leaf, parent = id1
        MakeRecord(1, {PortState::Child, PortState::Parent}),// child id0, parent id4
        MakeRecord(2, {PortState::Parent}),                  // leaf, parent = id3
        MakeRecord(3, {PortState::Child, PortState::Parent}),// child id2, parent id4
        MakeRecord(4, {PortState::Child, PortState::Child}), // root, children id1 & id3
    };
    EXPECT_EQ(DiameterOf(records), 4);
    EXPECT_EQ(GapCountOptimizer::CalculateFromHops(DiameterOf(records)), 10);
}
