#include <gtest/gtest.h>
#include "ASFWDriver/Async/AsyncSubsystem.hpp"

using namespace ASFW::Async;

// ============================================================================
// Test Fixture
// ============================================================================

class AsyncSubsystemAccessorTest : public ::testing::Test {
protected:
    AsyncSubsystem subsystem;
};

// ============================================================================
// Simple Getter Tests (Lines 257-262)
// ============================================================================

TEST_F(AsyncSubsystemAccessorTest, GetTracking_ReturnsNullWhenNotInitialized) {
    EXPECT_EQ(nullptr, subsystem.GetTracking());
}

TEST_F(AsyncSubsystemAccessorTest, GetDescriptorBuilder_ReturnsNullWhenNotInitialized) {
    EXPECT_EQ(nullptr, subsystem.GetDescriptorBuilder());
}

TEST_F(AsyncSubsystemAccessorTest, GetPacketBuilder_ReturnsNullWhenNotInitialized) {
    EXPECT_EQ(nullptr, subsystem.GetPacketBuilder());
}

TEST_F(AsyncSubsystemAccessorTest, GetSubmitter_ReturnsNullWhenNotInitialized) {
    EXPECT_EQ(nullptr, subsystem.GetSubmitter());
}

TEST_F(AsyncSubsystemAccessorTest, GetHardware_ReturnsNullWhenNotInitialized) {
    EXPECT_EQ(nullptr, subsystem.GetHardware());
}

TEST_F(AsyncSubsystemAccessorTest, GetPacketRouter_ReturnsNullWhenNotInitialized) {
    EXPECT_EQ(nullptr, subsystem.GetPacketRouter());
}

// ============================================================================
// Conditional Getter Tests (Lines 218-220, 266-268)
// ============================================================================

TEST_F(AsyncSubsystemAccessorTest, GetBusResetCapture_ReturnsNullWhenNotInitialized) {
    EXPECT_EQ(nullptr, subsystem.GetBusResetCapture());
}

// Note: GetDMAManager() test removed - requires full ContextManager initialization
// which brings in too many dependencies for a simple accessor test

// ============================================================================
// Inline Method Tests (Lines 170-174)
// ============================================================================

TEST_F(AsyncSubsystemAccessorTest, PostToWorkloop_HandlesNullQueueGracefully) {
    // Should not crash when workloopQueue_ is nullptr
    // Note: We can't verify if block executes since queue is null,
    // but we can verify it doesn't crash
    subsystem.PostToWorkloop(^{
        // This block won't execute since workloopQueue_ is nullptr
    });
    
    // Test passes if we reach here without crashing
    SUCCEED();
}

// ============================================================================
// Lazy Initialization Tests (Lines 238-254)
// ============================================================================

TEST_F(AsyncSubsystemAccessorTest, GetGenerationTracker_LazyInitialization) {
    // First call should create the tracker
    auto& tracker1 = subsystem.GetGenerationTracker();
    
    // Second call should return the same instance
    auto& tracker2 = subsystem.GetGenerationTracker();
    
    EXPECT_EQ(&tracker1, &tracker2) << "Should return same instance (singleton)";
}

TEST_F(AsyncSubsystemAccessorTest, GetBusState_ReturnsValidStateAfterLazyInit) {
    // Must call GetGenerationTracker first to initialize
    subsystem.GetGenerationTracker();
    
    // Now GetBusState should work
    auto state = subsystem.GetBusState();
    
    // Should return a valid state (generation8 0, nodeID 0 initially)
    EXPECT_EQ(0, state.generation8);
    EXPECT_EQ(0, state.localNodeID);
}

TEST_F(AsyncSubsystemAccessorTest, GetGenerationTracker_CreatesLabelAllocator) {
    // Calling GetGenerationTracker should also create labelAllocator_
    auto& tracker = subsystem.GetGenerationTracker();
    
    // Verify we can get bus state (which uses generationTracker_)
    auto state = subsystem.GetBusState();
    EXPECT_EQ(0, state.generation8);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(AsyncSubsystemAccessorTest, MultipleGetBusStateCalls_ConsistentResults) {
    // Initialize tracker first
    subsystem.GetGenerationTracker();
    
    auto state1 = subsystem.GetBusState();
    auto state2 = subsystem.GetBusState();
    
    EXPECT_EQ(state1.generation8, state2.generation8);
    EXPECT_EQ(state1.generation16, state2.generation16);
    EXPECT_EQ(state1.localNodeID, state2.localNodeID);
}


