#include "Discovery/DiscoveryConvergence.hpp"

#include <gtest/gtest.h>

namespace {

ASFW::Driver::TopologySnapshot MakeTopologyWithRemoteLinkActiveNode() {
    ASFW::Driver::TopologySnapshot snapshot{};
    snapshot.generation = 42;
    snapshot.localNodeId = 1;
    snapshot.nodeCount = 2;
    snapshot.nodes = {
        ASFW::Driver::TopologyNode{.nodeId = 0, .linkActive = true},
        ASFW::Driver::TopologyNode{.nodeId = 1, .linkActive = true},
    };
    return snapshot;
}

TEST(DiscoveryConvergenceTests, ZeroRomScanIsInconclusiveWhenTopologyStillHasRemoteNode) {
    const auto snapshot = MakeTopologyWithRemoteLinkActiveNode();

    EXPECT_TRUE(ASFW::Discovery::IsZeroRomScanInconclusive(
        ASFW::Discovery::Generation{42}, /*romCount=*/0, snapshot));
}

TEST(DiscoveryConvergenceTests, NonEmptyRomScanIsConclusive) {
    const auto snapshot = MakeTopologyWithRemoteLinkActiveNode();

    EXPECT_FALSE(ASFW::Discovery::IsZeroRomScanInconclusive(
        ASFW::Discovery::Generation{42}, /*romCount=*/1, snapshot));
}

TEST(DiscoveryConvergenceTests, ZeroRomScanIsConclusiveWhenTopologyHasNoRemoteNode) {
    ASFW::Driver::TopologySnapshot snapshot{};
    snapshot.generation = 42;
    snapshot.localNodeId = 1;
    snapshot.nodeCount = 1;
    snapshot.nodes = {
        ASFW::Driver::TopologyNode{.nodeId = 1, .linkActive = true},
    };

    EXPECT_FALSE(ASFW::Discovery::IsZeroRomScanInconclusive(
        ASFW::Discovery::Generation{42}, /*romCount=*/0, snapshot));
}

TEST(DiscoveryConvergenceTests, StaleTopologyGenerationIsConclusive) {
    auto snapshot = MakeTopologyWithRemoteLinkActiveNode();
    snapshot.generation = 41;

    EXPECT_FALSE(ASFW::Discovery::IsZeroRomScanInconclusive(
        ASFW::Discovery::Generation{42}, /*romCount=*/0, snapshot));
}

} // namespace
