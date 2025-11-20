#include <gtest/gtest.h>
#include <vector>
#include <functional>
#include <span>
#include "../ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "../ASFWDriver/Discovery/SpeedPolicy.hpp"
#include "../ASFWDriver/Discovery/DiscoveryTypes.hpp"
#include "../ASFWDriver/Controller/ControllerTypes.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;

namespace {

// Mock AsyncSubsystem for testing ROM reads without hardware
class MockAsyncSubsystem {
public:
    using ReadCallback = std::function<void(ASFW::Async::AsyncHandle,
                                           ASFW::Async::AsyncStatus,
                                           std::span<const uint8_t>)>;

    struct PendingRead {
        ASFW::Async::ReadParams params;
        ReadCallback callback;
        uint32_t handleValue;
    };

    std::vector<PendingRead> pendingReads_;
    uint32_t nextHandle_ = 1;

    // Mock ReadWithRetry - stores callback for later simulation
    ASFW::Async::AsyncHandle ReadWithRetry(const ASFW::Async::ReadParams& params,
                                           const ASFW::Async::RetryPolicy& policy,
                                           ReadCallback callback) {
        PendingRead read;
        read.params = params;
        read.callback = callback;
        read.handleValue = nextHandle_++;
        pendingReads_.push_back(read);
        return ASFW::Async::AsyncHandle{read.handleValue};
    }

    // Simulate successful read completion with provided data
    void SimulateReadSuccess(size_t readIndex, const std::vector<uint32_t>& quadlets) {
        if (readIndex >= pendingReads_.size()) {
            return;
        }

        // Convert quadlets to bytes (big-endian)
        std::vector<uint8_t> bytes;
        bytes.reserve(quadlets.size() * 4);
        for (uint32_t q : quadlets) {
            bytes.push_back((q >> 24) & 0xFF);
            bytes.push_back((q >> 16) & 0xFF);
            bytes.push_back((q >> 8) & 0xFF);
            bytes.push_back(q & 0xFF);
        }

        auto& read = pendingReads_[readIndex];
        read.callback(ASFW::Async::AsyncHandle{read.handleValue},
                     ASFW::Async::AsyncStatus::kSuccess,
                     std::span(bytes.data(), bytes.size()));
    }

    // Simulate timeout
    void SimulateReadTimeout(size_t readIndex) {
        if (readIndex >= pendingReads_.size()) {
            return;
        }

        auto& read = pendingReads_[readIndex];
        std::vector<uint8_t> empty;
        read.callback(ASFW::Async::AsyncHandle{read.handleValue},
                     ASFW::Async::AsyncStatus::kTimeout,
                     std::span(empty.data(), 0));
    }

    size_t GetPendingReadCount() const {
        return pendingReads_.size();
    }
};

// Helper to create minimal BIB (Bus Info Block) for testing
// Q0: info_length=1 (minimal ROM), crc_length=1, crc=valid
// Format: [31:24]=info_length, [23:16]=crc_length, [15:0]=crc
std::vector<uint32_t> CreateMinimalBIB() {
    // Minimal BIB: just header quadlet
    // info_length=1, crc_length=1, crc=0x0000 (we don't validate CRC in tests)
    return {0x04040000};  // 0x04=4 bytes info + 4 bytes crc = 1 quadlet each
}

// Helper to create full BIB with GUID
std::vector<uint32_t> CreateFullBIB(uint64_t guid = 0x0123456789ABCDEF) {
    return {
        0x0404B95A,  // Q0: header with valid CRC
        0x31333934,  // Q1: "1394" bus name
        0x8000A002,  // Q2: capabilities (link_speed=S400, etc.)
        static_cast<uint32_t>(guid >> 32),   // Q3: GUID high
        static_cast<uint32_t>(guid & 0xFFFFFFFF)  // Q4: GUID low
    };
}

} // anonymous namespace

// ============================================================================
// Manual Read Completion Tests - Verifies Apple-style immediate completion
// ============================================================================

TEST(ROMScannerCompletion, ManualRead_MinimalROM_InvokesCallbackImmediately) {
    // This test verifies the fix for the missing completion notification bug
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    bool callbackInvoked = false;
    Generation completedGen = 0;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        callbackInvoked = true;
        completedGen = gen;
    };

    ROMScannerParams params{
        .startSpeed = FwSpeed::S100,
        .maxInflight = 1,
        .perStepRetries = 0,
        .maxRootDirQuadlets = 16
    };

    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    // Create topology with one remote node
    TopologySnapshot topology;
    topology.generation = 42;
    topology.busBase16 = 0xFFC0;  // Standard bus address
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    // Trigger manual ROM read
    bool initiated = scanner.TriggerManualRead(1, 42, topology);
    ASSERT_TRUE(initiated);
    EXPECT_EQ(mockAsync.GetPendingReadCount(), 1);  // BIB read started

    // Simulate BIB read completion with minimal ROM
    mockAsync.SimulateReadSuccess(0, CreateMinimalBIB());

    // CRITICAL: Callback should be invoked immediately (Apple pattern)
    EXPECT_TRUE(callbackInvoked) << "Completion callback should be invoked immediately after ROM read completes";
    EXPECT_EQ(completedGen, 42);

    // Verify ROM is available
    EXPECT_TRUE(scanner.IsIdleFor(42));
    auto roms = scanner.DrainReady(42);
    EXPECT_EQ(roms.size(), 1);
    EXPECT_EQ(roms[0].nodeId, 1);
    EXPECT_EQ(roms[0].gen, 42);
}

TEST(ROMScannerCompletion, ManualRead_FullROM_InvokesCallbackAfterBothReads) {
    // Test full ROM read (BIB + root directory)
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    int callbackCount = 0;
    Generation lastCompletedGen = 0;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        callbackCount++;
        lastCompletedGen = gen;
    };

    ROMScannerParams params{
        .startSpeed = FwSpeed::S100,
        .maxInflight = 1,
        .perStepRetries = 0,
        .maxRootDirQuadlets = 4
    };

    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.generation = 10;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 2, .linkActive = true});

    bool initiated = scanner.TriggerManualRead(2, 10, topology);
    ASSERT_TRUE(initiated);

    // Simulate BIB read (full ROM)
    mockAsync.SimulateReadSuccess(0, CreateFullBIB());
    EXPECT_EQ(callbackCount, 0) << "Callback should not fire after BIB, waiting for root dir";
    EXPECT_EQ(mockAsync.GetPendingReadCount(), 2);  // Now reading root dir

    // Simulate root directory read (4 quadlets)
    std::vector<uint32_t> rootDir = {
        0x00040000,  // Length=4, CRC=0
        0x03000001,  // Vendor ID entry
        0x17000002,  // Model ID entry
        0x81000003   // Text descriptor entry
    };
    mockAsync.SimulateReadSuccess(1, rootDir);

    // Now callback should fire
    EXPECT_EQ(callbackCount, 1) << "Callback should fire after both BIB and root dir complete";
    EXPECT_EQ(lastCompletedGen, 10);

    auto roms = scanner.DrainReady(10);
    EXPECT_EQ(roms.size(), 1);
}

TEST(ROMScannerCompletion, ManualRead_WithoutCallback_DoesNotCrash) {
    // Verify backward compatibility - scanner works without callback
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    ROMScannerParams params{
        .startSpeed = FwSpeed::S100,
        .maxInflight = 1,
        .perStepRetries = 0,
        .maxRootDirQuadlets = 16
    };

    // Create scanner WITHOUT callback
    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 5;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 3, .linkActive = true});

    bool initiated = scanner.TriggerManualRead(3, 5, topology);
    ASSERT_TRUE(initiated);

    // Simulate completion - should not crash
    mockAsync.SimulateReadSuccess(0, CreateMinimalBIB());

    // Verify scan completed without callback
    EXPECT_TRUE(scanner.IsIdleFor(5));
    auto roms = scanner.DrainReady(5);
    EXPECT_EQ(roms.size(), 1);
}

TEST(ROMScannerCompletion, ManualRead_Timeout_InvokesCallbackAfterRetryExhaustion) {
    // Test that callback is invoked even on failure
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    bool callbackInvoked = false;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        callbackInvoked = true;
    };

    ROMScannerParams params{
        .startSpeed = FwSpeed::S100,
        .maxInflight = 1,
        .perStepRetries = 0,  // No retries
        .maxRootDirQuadlets = 16
    };

    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.generation = 7;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 4, .linkActive = true});

    bool initiated = scanner.TriggerManualRead(4, 7, topology);
    ASSERT_TRUE(initiated);

    // Simulate timeout
    mockAsync.SimulateReadTimeout(0);

    // Callback should still be invoked (node marked as Failed)
    EXPECT_TRUE(callbackInvoked) << "Callback should be invoked even on failure";
    EXPECT_TRUE(scanner.IsIdleFor(7));

    // No ROMs should be available (read failed)
    auto roms = scanner.DrainReady(7);
    EXPECT_EQ(roms.size(), 0);
}

TEST(ROMScannerCompletion, AutomaticScan_InvokesCallback_ApplePattern) {
    // Verify automatic scan also triggers callback (regression test)
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    bool callbackInvoked = false;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        callbackInvoked = true;
    };

    ROMScannerParams params{
        .startSpeed = FwSpeed::S100,
        .maxInflight = 2,
        .perStepRetries = 0,
        .maxRootDirQuadlets = 16
    };

    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.generation = 1;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});
    topology.nodes.push_back({.nodeId = 2, .linkActive = true});

    // Start automatic scan (localNodeId=0, scans nodeId 1 and 2)
    scanner.Begin(1, topology, 0);

    EXPECT_EQ(mockAsync.GetPendingReadCount(), 2);  // Both BIB reads started

    // Complete both reads
    mockAsync.SimulateReadSuccess(0, CreateMinimalBIB());
    mockAsync.SimulateReadSuccess(1, CreateMinimalBIB());

    // Callback should fire after last ROM completes
    EXPECT_TRUE(callbackInvoked);

    auto roms = scanner.DrainReady(1);
    EXPECT_EQ(roms.size(), 2);
}

TEST(ROMScannerCompletion, MultipleManualReads_EachInvokesCallback) {
    // Test that multiple manual reads each trigger completion
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::vector<Generation> completedGens;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        completedGens.push_back(gen);
    };

    ROMScannerParams params{
        .startSpeed = FwSpeed::S100,
        .maxInflight = 1,
        .perStepRetries = 0,
        .maxRootDirQuadlets = 16
    };

    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    // First manual read (gen=1)
    topology.generation = 1;
    scanner.TriggerManualRead(1, 1, topology);
    mockAsync.SimulateReadSuccess(0, CreateMinimalBIB());

    // Second manual read (gen=2, scanner restarts)
    topology.generation = 2;
    scanner.TriggerManualRead(1, 2, topology);
    mockAsync.SimulateReadSuccess(1, CreateMinimalBIB());

    // Both should have completed
    EXPECT_EQ(completedGens.size(), 2);
    EXPECT_EQ(completedGens[0], 1);
    EXPECT_EQ(completedGens[1], 2);
}
