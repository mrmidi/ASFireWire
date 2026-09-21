// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025 ASFW Project
//
// TopologySpeedTests.cpp — Self-ID derived path speed.

#include "Bus/TopologySpeed.hpp"
#include "gtest/gtest.h"

using ASFW::Driver::PathSpeedCodeBetween;
using ASFW::Driver::TopologyGraphStatus;
using ASFW::Driver::TopologyNodeRecord;
using ASFW::Driver::TopologySnapshot;

namespace {

constexpr uint32_t kS100 = 0;
constexpr uint32_t kS200 = 1;
constexpr uint32_t kS400 = 2;
constexpr uint32_t kS800 = 3;

TopologyNodeRecord MakeNode(uint8_t physicalId, uint32_t speedCode, uint8_t portCount = 0) {
    TopologyNodeRecord node{};
    node.physicalId = physicalId;
    node.speedCode = speedCode;
    node.linkActive = true;
    node.portCount = portCount;
    return node;
}

void Connect(TopologyNodeRecord& a, uint8_t portA, TopologyNodeRecord& b, uint8_t portB) {
    a.links[portA].connected = true;
    a.links[portA].remoteNodeId = b.physicalId;
    b.links[portB].connected = true;
    b.links[portB].remoteNodeId = a.physicalId;
}

// Local node 0 <-> device node 1, both S400 — a TB2/FW800 adapter facing one
// audio interface, which is the topology the whole question came from.
TopologySnapshot TwoNodeBus(uint32_t localSpeed, uint32_t remoteSpeed) {
    TopologySnapshot topology{};
    topology.graphStatus = TopologyGraphStatus::Valid;
    topology.localNodeId = 0;
    topology.rootNodeId = 1;
    topology.nodeCount = 2;
    topology.physical.nodes = {MakeNode(0, localSpeed, 1), MakeNode(1, remoteSpeed, 1)};
    Connect(topology.physical.nodes[0], 0, topology.physical.nodes[1], 0);
    return topology;
}

} // namespace

TEST(TopologySpeedTests, TwoNodeS400BusResolvesS400) {
    const auto topology = TwoNodeBus(kS400, kS400);
    EXPECT_EQ(PathSpeedCodeBetween(topology, 0, 1), std::optional<uint8_t>{kS400});
    EXPECT_EQ(PathSpeedCodeBetween(topology, 1, 0), std::optional<uint8_t>{kS400});
}

// The defect this exists to prevent. SpeedPolicy demotes the *async* speed to
// S200 when a device times out a request — some devices genuinely need that —
// and the isochronous speed used to inherit it. Self-ID says S400 regardless of
// how the device behaves on the async path, and the isoch charge is
// `unitsAtS1600 >> speedCode`, so inheriting S200 doubled the bandwidth bill.
TEST(TopologySpeedTests, AsyncDemotionCannotLowerTheSelfIdPathSpeed) {
    const auto topology = TwoNodeBus(kS400, kS400);

    // Whatever SpeedPolicy concluded from timed-out async requests, Self-ID is
    // unchanged and this is what isochronous traffic must be charged at.
    EXPECT_EQ(PathSpeedCodeBetween(topology, 0, 1), std::optional<uint8_t>{kS400});
}

TEST(TopologySpeedTests, SlowerEndpointCapsThePath) {
    const auto fastLocal = TwoNodeBus(kS400, kS200);
    EXPECT_EQ(PathSpeedCodeBetween(fastLocal, 0, 1), std::optional<uint8_t>{kS200});

    const auto slowLocal = TwoNodeBus(kS100, kS800);
    EXPECT_EQ(PathSpeedCodeBetween(slowLocal, 0, 1), std::optional<uint8_t>{kS100});
}

// A repeating PHY cannot resend faster than it received, so a slow node in the
// middle caps everything behind it (IEEE 1394-2008 §4.2.3).
TEST(TopologySpeedTests, SlowMiddleHopCapsTheWholePath) {
    TopologySnapshot topology{};
    topology.graphStatus = TopologyGraphStatus::Valid;
    topology.localNodeId = 0;
    topology.nodeCount = 3;
    topology.physical.nodes = {
        MakeNode(0, kS800, 1),
        MakeNode(1, kS200, 2),
        MakeNode(2, kS800, 1),
    };
    Connect(topology.physical.nodes[0], 0, topology.physical.nodes[1], 0);
    Connect(topology.physical.nodes[1], 1, topology.physical.nodes[2], 0);

    EXPECT_EQ(PathSpeedCodeBetween(topology, 0, 1), std::optional<uint8_t>{kS200});
    EXPECT_EQ(PathSpeedCodeBetween(topology, 0, 2), std::optional<uint8_t>{kS200});
}

TEST(TopologySpeedTests, S800SurvivesTheLegacySpeedMapClamp) {
    // SpeedMapService clamps its matrix to S400 because the legacy SPEED_MAP CSR
    // is a conservative diagnostic surface. A transmit-speed decision must not
    // inherit that clamp, which is why this helper is uncapped.
    const auto topology = TwoNodeBus(kS800, kS800);
    EXPECT_EQ(PathSpeedCodeBetween(topology, 0, 1), std::optional<uint8_t>{kS800});
}

TEST(TopologySpeedTests, SameNodeReturnsItsOwnSpeed) {
    const auto topology = TwoNodeBus(kS400, kS200);
    EXPECT_EQ(PathSpeedCodeBetween(topology, 1, 1), std::optional<uint8_t>{kS200});
}

TEST(TopologySpeedTests, InvalidTopologyAnswersNothing) {
    auto topology = TwoNodeBus(kS400, kS400);
    topology.graphStatus = TopologyGraphStatus::Unknown;
    EXPECT_FALSE(PathSpeedCodeBetween(topology, 0, 1).has_value());
}

TEST(TopologySpeedTests, UnknownOrDisconnectedNodesAnswerNothing) {
    const auto topology = TwoNodeBus(kS400, kS400);
    EXPECT_FALSE(PathSpeedCodeBetween(topology, 0, 9).has_value());
    EXPECT_FALSE(PathSpeedCodeBetween(topology, 0, 63).has_value());

    TopologySnapshot split{};
    split.graphStatus = TopologyGraphStatus::Valid;
    split.localNodeId = 0;
    split.nodeCount = 2;
    split.physical.nodes = {MakeNode(0, kS400, 1), MakeNode(1, kS400, 1)};
    EXPECT_FALSE(PathSpeedCodeBetween(split, 0, 1).has_value());
}

TEST(TopologySpeedTests, LinkInactiveNodeIsNotReachable) {
    auto topology = TwoNodeBus(kS400, kS400);
    topology.physical.nodes[1].linkActive = false;
    EXPECT_FALSE(PathSpeedCodeBetween(topology, 0, 1).has_value());
}

TEST(TopologySpeedTests, IntermediateRepeatingPhyTraversedEvenIfLinkInactive) {
    // 3-node bus: Node 0 (S400, active) <-> Node 1 (repeating PHY S400, link inactive) <-> Node 2 (S400, active).
    // Linux core-topology.c:263-320: repeating PHY forwards traffic regardless of link layer state.
    TopologySnapshot topology{};
    topology.graphStatus = TopologyGraphStatus::Valid;
    topology.localNodeId = 0;
    topology.nodeCount = 3;
    topology.physical.nodes = {
        MakeNode(0, kS400, 1),
        MakeNode(1, kS400, 2),
        MakeNode(2, kS400, 1),
    };
    topology.physical.nodes[1].linkActive = false;
    Connect(topology.physical.nodes[0], 0, topology.physical.nodes[1], 0);
    Connect(topology.physical.nodes[1], 1, topology.physical.nodes[2], 0);

    // Endpoints are 0 and 2; intermediate node 1 repeats traffic.
    EXPECT_EQ(PathSpeedCodeBetween(topology, 0, 2), std::optional<uint8_t>{kS400});
    EXPECT_EQ(PathSpeedCodeBetween(topology, 2, 0), std::optional<uint8_t>{kS400});

    // Directly addressing the link-inactive node 1 as an endpoint still yields nothing.
    EXPECT_FALSE(PathSpeedCodeBetween(topology, 0, 1).has_value());
    EXPECT_FALSE(PathSpeedCodeBetween(topology, 1, 2).has_value());

    // If the intermediate repeating PHY is slower (e.g. S200), the path is capped at S200.
    topology.physical.nodes[1].speedCode = kS200;
    EXPECT_EQ(PathSpeedCodeBetween(topology, 0, 2), std::optional<uint8_t>{kS200});
}
