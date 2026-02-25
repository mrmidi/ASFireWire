#include <gtest/gtest.h>
#include <vector>
#include <functional>
#include <span>
#include <mutex>
#include <condition_variable>
#include "../ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "../ASFWDriver/Discovery/SpeedPolicy.hpp"
#include "../ASFWDriver/Discovery/DiscoveryTypes.hpp"
#include "../ASFWDriver/Controller/ControllerTypes.hpp"
#include "../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;

namespace {

// Mock AsyncSubsystem for testing ROM reads without hardware
class MockAsyncSubsystem : public ASFW::Async::IFireWireBus {
public:
    struct PendingRead {
        ASFW::FW::Generation gen{0};
        ASFW::FW::NodeId nodeId{0};
        ASFW::Async::FWAddress address;
        uint32_t length{0};
        ASFW::Async::InterfaceCompletionCallback callback;
        uint32_t handleValue{0};

        PendingRead() = default;
        PendingRead(const PendingRead&) = default;
        PendingRead& operator=(const PendingRead&) = default;
        PendingRead(PendingRead&&) noexcept = default;
        PendingRead& operator=(PendingRead&&) noexcept = default;
    };

    std::vector<PendingRead> pendingReads_;
    uint32_t nextHandle_ = 1;
    mutable std::mutex readsMtx_;
    mutable std::condition_variable readsCv_;

    ASFW::Async::AsyncHandle ReadBlock(
        ASFW::FW::Generation generation,
        ASFW::FW::NodeId nodeId,
        ASFW::Async::FWAddress address,
        uint32_t length,
        ASFW::FW::FwSpeed speed,
        ASFW::Async::InterfaceCompletionCallback callback) override
    {
        std::lock_guard lock(readsMtx_);
        PendingRead read;
        read.gen = generation;
        read.nodeId = nodeId;
        read.address = address;
        read.length = length;
        read.callback = std::move(callback);
        read.handleValue = nextHandle_++;
        pendingReads_.push_back(std::move(read));
        readsCv_.notify_all();
        return ASFW::Async::AsyncHandle{read.handleValue};
    }

    ASFW::Async::AsyncHandle WriteBlock(
        ASFW::FW::Generation generation,
        ASFW::FW::NodeId nodeId,
        ASFW::Async::FWAddress address,
        std::span<const uint8_t> data,
        ASFW::FW::FwSpeed speed,
        ASFW::Async::InterfaceCompletionCallback callback) override { return ASFW::Async::AsyncHandle{0}; }

    ASFW::Async::AsyncHandle Lock(
        ASFW::FW::Generation generation,
        ASFW::FW::NodeId nodeId,
        ASFW::Async::FWAddress address,
        ASFW::FW::LockOp lockOp,
        std::span<const uint8_t> operand,
        uint32_t responseLength,
        ASFW::FW::FwSpeed speed,
        ASFW::Async::InterfaceCompletionCallback callback) override { return ASFW::Async::AsyncHandle{0}; }

    bool Cancel(ASFW::Async::AsyncHandle handle) override { return false; }

    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId nodeId) const override { return ASFW::FW::FwSpeed::S100; }
    uint32_t HopCount(ASFW::FW::NodeId nodeA, ASFW::FW::NodeId nodeB) const override { return 0; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{0}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }

    // Simulate successful read completion with provided data
    void SimulateReadSuccess(size_t readIndex, const std::vector<uint32_t>& quadlets) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(readsMtx_);
            if (readIndex >= pendingReads_.size()) {
                return;
            }
            callback = std::move(pendingReads_[readIndex].callback);
        }

        if (!callback) return;

        // Convert quadlets to bytes (big-endian)
        std::vector<uint8_t> bytes;
        bytes.reserve(quadlets.size() * 4);
        for (uint32_t q : quadlets) {
            bytes.push_back((q >> 24) & 0xFF);
            bytes.push_back((q >> 16) & 0xFF);
            bytes.push_back((q >> 8) & 0xFF);
            bytes.push_back(q & 0xFF);
        }

        callback(ASFW::Async::AsyncStatus::kSuccess,
                 std::span(bytes.data(), bytes.size()));
    }

    // Simulate timeout
    void SimulateReadTimeout(size_t readIndex) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(readsMtx_);
            if (readIndex >= pendingReads_.size()) {
                return;
            }
            callback = std::move(pendingReads_[readIndex].callback);
        }

        if (!callback) return;

        std::vector<uint8_t> empty;
        callback(ASFW::Async::AsyncStatus::kTimeout,
                 std::span(empty.data(), 0));
    }

    size_t GetPendingReadCount() const {
        std::lock_guard lock(readsMtx_);
        return pendingReads_.size();
    }

    void WaitForPendingReads(size_t count) const {
        std::unique_lock lock(readsMtx_);
        readsCv_.wait_for(lock, std::chrono::seconds(1), [this, &count] { return pendingReads_.size() >= count; });
    }

    // Helper to simulate all quadlet reads for a standard 5-quadlet BIB
    void SimulateFullBIBSuccess(const std::vector<uint32_t>& bib) {
        // ROMReader issues 4 reads: Q0, then Q2, Q3, Q4 (Q1 is prefilled)
        // We must simulate them one by one because ROMReader is sequential.
        size_t startIdx = 0;
        {
            std::lock_guard lock(readsMtx_);
            if (pendingReads_.empty()) {
                return;
            }
            startIdx = pendingReads_.size() - 1;
        }
        
        // Q0
        WaitForPendingReads(startIdx + 1);
        SimulateReadSuccess(startIdx, {bib[0]});
        // Q2
        WaitForPendingReads(startIdx + 2);
        SimulateReadSuccess(startIdx + 1, {bib[2]});
        // Q3
        WaitForPendingReads(startIdx + 3);
        SimulateReadSuccess(startIdx + 2, {bib[3]});
        // Q4
        WaitForPendingReads(startIdx + 4);
        SimulateReadSuccess(startIdx + 3, {bib[4]});
    }

    void SimulateSequentialReads(size_t startIdx, const std::vector<uint32_t>& quadlets) {
        for (size_t i = 0; i < quadlets.size(); ++i) {
            WaitForPendingReads(startIdx + i + 1);
            SimulateReadSuccess(startIdx + i, {quadlets[i]});
        }
    }
};

// Helper to create minimal BIB (Bus Info Block) for testing
// Q0: info_length=1 (minimal ROM), crc_length=1, crc=valid
// Format: [31:24]=info_length, [23:16]=crc_length, [15:0]=crc
std::vector<uint32_t> CreateMinimalBIB() {
    // Minimal BIB: header quadlet + 4 quadlets of zeros
    // info_length=4 (standard BIB), crc_length=4 (minimal total ROM), crc=0x0000
    return {0x04040000, 0, 0, 0, 0};
}

// Helper to create full BIB with GUID
std::vector<uint32_t> CreateFullBIB(uint64_t guid = 0x0123456789ABCDEF) {
    return {
        0x0408B95A,  // Q0: header with valid CRC, info_length=4, crc_length=8
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
    std::mutex mtx;
    std::condition_variable cv;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        std::lock_guard lock(mtx);
        callbackInvoked = true;
        completedGen = gen;
        cv.notify_one();
    };

    ROMScannerParams params{};
    params.doIRMCheck = false;
    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    // Create topology with one remote node
    TopologySnapshot topology;
    topology.generation = 42;
    topology.busBase16 = 0xFFC0;  // Standard bus address
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    // Trigger manual ROM read
    bool initiated = scanner.TriggerManualRead(1, 42, topology);
    ASSERT_TRUE(initiated);
    mockAsync.WaitForPendingReads(1);
    EXPECT_EQ(mockAsync.GetPendingReadCount(), 1);  // BIB read started (Q0)

    // Simulate BIB read completion with minimal ROM
    mockAsync.SimulateFullBIBSuccess(CreateMinimalBIB());

    // Wait for async completion
    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&callbackInvoked] { return callbackInvoked; });
    }

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
    std::mutex mtx;
    std::condition_variable cv;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        std::lock_guard lock(mtx);
        callbackCount++;
        lastCompletedGen = gen;
        cv.notify_one();
    };

    ROMScannerParams params{};
    params.doIRMCheck = false;
    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.generation = 10;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 2, .linkActive = true});

    bool initiated = scanner.TriggerManualRead(2, 10, topology);
    ASSERT_TRUE(initiated);
    mockAsync.WaitForPendingReads(1);

    // Simulate BIB read (full ROM)
    mockAsync.SimulateFullBIBSuccess(CreateFullBIB());
    EXPECT_EQ(callbackCount, 0) << "Callback should not fire after BIB, waiting for root dir";
    
    // Give a moment for async transitions
    mockAsync.WaitForPendingReads(5);
    EXPECT_EQ(mockAsync.GetPendingReadCount(), 5);  // 4 BIB reads + 1 RootDir header read

    // Simulate root directory read (header + 3 entries)
    std::vector<uint32_t> rootDir = {
        0x00020000,  // Length=2, CRC=0
        0x03000001,  // Vendor ID entry
        0x17000002   // Model ID entry
    };
    mockAsync.SimulateSequentialReads(4, rootDir);

    // Wait for async completion
    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&callbackCount] { return callbackCount > 0; });
    }

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

    ROMScannerParams params{};
    params.doIRMCheck = false;
    // Create scanner WITHOUT callback
    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 5;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 3, .linkActive = true});

    bool initiated = scanner.TriggerManualRead(3, 5, topology);
    ASSERT_TRUE(initiated);
    mockAsync.WaitForPendingReads(1);

    // Simulate completion - should not crash
    mockAsync.SimulateFullBIBSuccess(CreateMinimalBIB());

    // Give moment for async transitions
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

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
    std::mutex mtx;
    std::condition_variable cv;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        std::lock_guard lock(mtx);
        callbackInvoked = true;
        cv.notify_one();
    };

    ROMScannerParams params{};
    params.doIRMCheck = false;
    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.generation = 7;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 4, .linkActive = true});

    bool initiated = scanner.TriggerManualRead(4, 7, topology);
    ASSERT_TRUE(initiated);
    mockAsync.WaitForPendingReads(1);

    // Simulate timeout on Q0 read
    mockAsync.SimulateReadTimeout(0);
    mockAsync.WaitForPendingReads(2);
    mockAsync.SimulateReadTimeout(1);
    mockAsync.WaitForPendingReads(3);
    mockAsync.SimulateReadTimeout(2);

    // Wait for async completion
    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&callbackInvoked] { return callbackInvoked; });
    }

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
    std::mutex mtx;
    std::condition_variable cv;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        std::lock_guard lock(mtx);
        callbackInvoked = true;
        cv.notify_one();
    };

    ROMScannerParams params{};
    params.doIRMCheck = false;
    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.generation = 1;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});
    topology.nodes.push_back({.nodeId = 2, .linkActive = true});

    // Start automatic scan (localNodeId=0, scans nodeId 1 and 2)
    scanner.Begin(1, topology, 0);

    mockAsync.WaitForPendingReads(2);
    EXPECT_EQ(mockAsync.GetPendingReadCount(), 2);  // Both BIB reads started (Q0 for both nodes)

    // Complete BIB reads for both nodes
    // Node 1: index 0 (Q0), index 2 (Q2), index 4 (Q3), index 6 (Q4)
    // Node 2: index 1 (Q0), index 3 (Q2), index 5 (Q3), index 7 (Q4)
    // This interleaved pattern happens because ROMReader is sequential per-node but concurrent across nodes.
    
    mockAsync.SimulateReadSuccess(0, {CreateMinimalBIB()[0]}); // Node 1 Q0
    mockAsync.SimulateReadSuccess(1, {CreateMinimalBIB()[0]}); // Node 2 Q0
    
    mockAsync.WaitForPendingReads(4);
    mockAsync.SimulateReadSuccess(2, {0}); // Node 1 Q2
    mockAsync.SimulateReadSuccess(3, {0}); // Node 2 Q2
    
    mockAsync.WaitForPendingReads(6);
    mockAsync.SimulateReadSuccess(4, {0}); // Node 1 Q3
    mockAsync.SimulateReadSuccess(5, {0}); // Node 2 Q3
    
    mockAsync.WaitForPendingReads(8);
    mockAsync.SimulateReadSuccess(6, {0}); // Node 1 Q4
    mockAsync.SimulateReadSuccess(7, {0}); // Node 2 Q4

    // Wait for async completion
    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&callbackInvoked] { return callbackInvoked; });
    }

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
    std::mutex mtx;
    std::condition_variable cv;

    ScanCompletionCallback onComplete = [&](Generation gen) {
        std::lock_guard lock(mtx);
        completedGens.push_back(gen);
        cv.notify_all();
    };

    ROMScannerParams params{};
    params.doIRMCheck = false;
    ROMScanner scanner(mockAsync, speedPolicy, params, onComplete);

    TopologySnapshot topology;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    // First manual read (gen=1)
    topology.generation = 1;
    scanner.TriggerManualRead(1, 1, topology);
    mockAsync.WaitForPendingReads(1);
    mockAsync.SimulateFullBIBSuccess(CreateMinimalBIB());

    // Wait for first completion
    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&completedGens] { return completedGens.size() == 1; });
    }

    // Second manual read (gen=2, scanner restarts)
    topology.generation = 2;
    scanner.TriggerManualRead(1, 2, topology);
    mockAsync.WaitForPendingReads(5); // 4 from first + 1 for second
    mockAsync.SimulateFullBIBSuccess(CreateMinimalBIB());

    // Wait for second completion
    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&completedGens] { return completedGens.size() == 2; });
    }

    // Both should have completed
    EXPECT_EQ(completedGens.size(), 2);
    EXPECT_EQ(completedGens[0], 1);
    EXPECT_EQ(completedGens[1], 2);
}
