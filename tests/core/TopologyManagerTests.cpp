#include <gtest/gtest.h>
#include "../ASFWDriver/Bus/TopologyManager.hpp"
#include "../ASFWDriver/Bus/SelfIDCapture.hpp"

using namespace ASFW::Driver;

namespace {

// Helper to create a Self-ID sequence for testing
SelfIDCapture::Result CreateSelfIDResult(
    uint32_t generation,
    const std::vector<uint32_t>& quads,
    const std::vector<std::pair<size_t, unsigned int>>& sequences
) {
    SelfIDCapture::Result result;
    result.valid = true;
    result.generation = generation;
    result.quads = quads;
    result.sequences = sequences;
    result.crcError = false;
    result.timedOut = false;
    return result;
}

// Helper to create node base Self-ID quadlet
uint32_t MakeBaseSelfID(
    uint8_t phyId,
    bool linkActive,
    bool contender,
    uint8_t gapCount,
    uint8_t speedCode,
    uint8_t powerClass,
    bool initiatedReset = false,
    uint8_t port0 = 0,
    uint8_t port1 = 0,
    uint8_t port2 = 0
) {
    uint32_t quad = 0x80000000;  // tag=2 (Self-ID)
    quad |= (uint32_t(phyId) & 0x3F) << 24;
    quad |= linkActive ? (1u << 22) : 0;
    quad |= (uint32_t(gapCount) & 0x3F) << 16;
    quad |= (uint32_t(speedCode) & 0x7) << 14;
    quad |= contender ? (1u << 11) : 0;
    quad |= (uint32_t(powerClass) & 0x7) << 8;
    quad |= (uint32_t(port0) & 0x3) << 6;
    quad |= (uint32_t(port1) & 0x3) << 4;
    quad |= (uint32_t(port2) & 0x3) << 2;
    quad |= initiatedReset ? (1u << 1) : 0;
    return quad;
}

std::optional<TopologySnapshot> AsOptional(
    const std::expected<TopologySnapshot, TopologyBuildError>& snapshot
) {
    if (!snapshot.has_value()) {
        return std::nullopt;
    }
    return *snapshot;
}

} // anonymous namespace

// ============================================================================
// IRM Detection Tests
// ============================================================================

TEST(TopologyManager, IRMDetection_MultipleContenders_SelectsHighestNodeID) {
    // Create 3 nodes: node 0 (contender), node 1 (non-contender), node 2 (contender)
    auto result = CreateSelfIDResult(
        42,
        {
            0x002A0000,
            MakeBaseSelfID(0, true, true, 63, 2, 4, false, 2),
            MakeBaseSelfID(1, true, false, 63, 2, 4, false, 3, 2),
            MakeBaseSelfID(2, true, true, 63, 2, 4, false, 3),   // node 2: contender
        },
        {{1, 1}, {2, 1}, {3, 1}}
    );
    
    TopologyManager manager;
    const uint32_t nodeIDReg = 0x80000000;
    auto snapshot = manager.UpdateFromSelfID(result, 123456, nodeIDReg);
    
    ASSERT_TRUE(snapshot.has_value()) << "Error: " << TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code);
    EXPECT_EQ(snapshot->irmNodeId, 2);
}

TEST(TopologyManager, IRMDetection_NoContenders_ReturnsInvalidId) {
    auto result = CreateSelfIDResult(
        10,
        {
            0x000A0000,
            MakeBaseSelfID(0, true, false, 63, 2, 4, false, 2),
            MakeBaseSelfID(1, true, false, 63, 2, 4, false, 3),
        },
        {{1, 1}, {2, 1}}
    );
    
    TopologyManager manager;
    auto snapshot = manager.UpdateFromSelfID(result, 200000, 0x80000001);
    
    ASSERT_TRUE(snapshot.has_value()) << "Error: " << TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code);
    EXPECT_EQ(snapshot->irmNodeId, kInvalidPhysicalId);
}

// ============================================================================
// Root Node Selection Tests
// ============================================================================

TEST(TopologyManager, RootSelection_HighestPhysicalIdIsRoot) {
    auto result = CreateSelfIDResult(
        20,
        {
            0x00140000,
            MakeBaseSelfID(0, true, false, 63, 2, 4, false, 2),
            MakeBaseSelfID(1, true, false, 63, 2, 4, false, 3, 2),
            MakeBaseSelfID(2, true, false, 63, 2, 4, false, 3),
        },
        {{1, 1}, {2, 1}, {3, 1}}
    );
    
    TopologyManager manager;
    auto snapshot = manager.UpdateFromSelfID(result, 400000, 0x80000000);
    
    ASSERT_TRUE(snapshot.has_value()) << "Error: " << TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code);
    EXPECT_EQ(snapshot->rootNodeId, 2);
}

// ============================================================================
// Gap Count Tests
// ============================================================================

TEST(TopologyManager, GapCount_MultipleNodes_SelectsMaximum) {
    auto result = CreateSelfIDResult(
        30,
        {
            0x001E0000,
            MakeBaseSelfID(0, true, false, 10, 2, 4, false, 2),
            MakeBaseSelfID(1, true, false, 63, 2, 4, false, 3, 2),
            MakeBaseSelfID(2, true, false, 20, 2, 4, false, 3),
        },
        {{1, 1}, {2, 1}, {3, 1}}
    );
    
    TopologyManager manager;
    auto snapshot = manager.UpdateFromSelfID(result, 600000, 0x80000001);
    
    ASSERT_TRUE(snapshot.has_value()) << "Error: " << TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code);
    EXPECT_EQ(snapshot->gapCount, 63);
}

// ============================================================================
// Initiated Reset Tracking Tests
// ============================================================================

TEST(TopologyManager, InitiatedReset_NodeSetsBit_MarkedInTopology) {
    auto result = CreateSelfIDResult(
        50,
        {
            0x00320000,
            MakeBaseSelfID(0, true, false, 63, 2, 4, true, 2),
            MakeBaseSelfID(1, true, false, 63, 2, 4, false, 3),
        },
        {{1, 1}, {2, 1}}
    );
    
    TopologyManager manager;
    auto snapshot = manager.UpdateFromSelfID(result, 800000, 0x80000001);
    
    ASSERT_TRUE(snapshot.has_value()) << "Error: " << TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code);
    ASSERT_EQ(snapshot->physical.nodes.size(), 2u);
    EXPECT_TRUE(snapshot->physical.nodes[0].initiatedReset);
    EXPECT_FALSE(snapshot->physical.nodes[1].initiatedReset);
}

// ============================================================================
// Local Node ID Tests
// ============================================================================

TEST(TopologyManager, LocalNodeID_iDValidSet_ExtractsNodeNumber) {
    auto result = CreateSelfIDResult(
        8,
        {
            0x00080000,
            MakeBaseSelfID(0, true, false, 63, 2, 4, false, 2),
            MakeBaseSelfID(1, true, false, 63, 2, 4, false, 3, 2),
            MakeBaseSelfID(2, true, false, 63, 2, 4, false, 3),
        },
        {{1, 1}, {2, 1}, {3, 1}}
    );
    
    TopologyManager manager;
    auto snapshot = manager.UpdateFromSelfID(result, 900000, 0x80000002);
    
    ASSERT_TRUE(snapshot.has_value()) << "Error: " << TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code);
    EXPECT_EQ(snapshot->localNodeId, 2);
}

// ============================================================================
// Invalid Input Handling Tests
// ============================================================================

TEST(TopologyManager, InvalidSelfID_DoesNotReusePreviousSnapshot) {
    TopologyManager manager;
    
    auto validResult = CreateSelfIDResult(
        10,
        {
            0x000A0000,
            MakeBaseSelfID(0, true, false, 63, 2, 4),
        },
        {{1, 1}}
    );
    
    auto snapshot1 = manager.UpdateFromSelfID(validResult, 1400000, 0x80000000);
    ASSERT_TRUE(snapshot1.has_value()) << "Error: " << TopologyManager::TopologyBuildErrorCodeString(snapshot1.error().code);
    
    SelfIDCapture::Result invalidResult;
    invalidResult.valid = false;
    
    auto snapshot2 = manager.UpdateFromSelfID(invalidResult, 1500000, 0x80000000);
    ASSERT_FALSE(snapshot2.has_value());
}

// ============================================================================
// Reset and State Management Tests
// ============================================================================

TEST(TopologyManager, Reset_ClearsSnapshot) {
    TopologyManager manager;
    auto result = CreateSelfIDResult(
        15,
        {
            0x000F0000,
            MakeBaseSelfID(0, true, false, 63, 2, 4),
        },
        {{1, 1}}
    );
    
    (void)manager.UpdateFromSelfID(result, 1700000, 0x80000000);
    ASSERT_TRUE(manager.LatestSnapshot().has_value());
    
    manager.Reset();
    EXPECT_FALSE(manager.LatestSnapshot().has_value());
}

TEST(TopologyManager, CompareAndSwap_SameTimestamp_ReturnsNullopt) {
    TopologyManager manager;
    auto result = CreateSelfIDResult(
        20,
        {
            0x00140000,
            MakeBaseSelfID(0, true, false, 63, 2, 4),
        },
        {{1, 1}}
    );
    
    const uint64_t timestamp = 1800000;
    auto snapshot1 = manager.UpdateFromSelfID(result, timestamp, 0x80000000);
    
    auto snapshot2 = manager.CompareAndSwap(AsOptional(snapshot1));
    EXPECT_FALSE(snapshot2.has_value());
}
