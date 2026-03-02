#include <gtest/gtest.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "../ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "../ASFWDriver/Controller/ControllerTypes.hpp"
#include "../ASFWDriver/Discovery/SpeedPolicy.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;

namespace {

class MockAsyncSubsystem : public ASFW::Async::IFireWireBus {
public:
    struct PendingRead {
        ASFW::Async::InterfaceCompletionCallback callback;
    };

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation,
                                       ASFW::FW::NodeId,
                                       ASFW::Async::FWAddress,
                                       uint32_t,
                                       ASFW::FW::FwSpeed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        std::lock_guard lock(mtx_);
        pendingReads_.push_back(PendingRead{.callback = std::move(callback)});
        cv_.notify_all();
        return ASFW::Async::AsyncHandle{static_cast<uint32_t>(pendingReads_.size())};
    }

    ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation,
                                        ASFW::FW::NodeId,
                                        ASFW::Async::FWAddress,
                                        std::span<const uint8_t>,
                                        ASFW::FW::FwSpeed,
                                        ASFW::Async::InterfaceCompletionCallback) override {
        return ASFW::Async::AsyncHandle{0};
    }

    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation,
                                  ASFW::FW::NodeId,
                                  ASFW::Async::FWAddress,
                                  ASFW::FW::LockOp,
                                  std::span<const uint8_t>,
                                  uint32_t,
                                  ASFW::FW::FwSpeed,
                                  ASFW::Async::InterfaceCompletionCallback) override {
        return ASFW::Async::AsyncHandle{0};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override { return ASFW::FW::FwSpeed::S100; }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 0; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{0}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }

    void WaitForPendingReads(size_t count) const {
        std::unique_lock lock(mtx_);
        cv_.wait_for(lock, std::chrono::seconds(1),
                     [this, count] { return pendingReads_.size() >= count; });
    }

    void SimulateReadSuccess(size_t readIndex, std::span<const uint32_t> quadletsBE) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(mtx_);
            ASSERT_LT(readIndex, pendingReads_.size());
            callback = std::move(pendingReads_[readIndex].callback);
        }

        ASSERT_TRUE(static_cast<bool>(callback));

        std::vector<uint8_t> bytes;
        bytes.reserve(quadletsBE.size() * 4);
        for (const uint32_t q : quadletsBE) {
            bytes.push_back(static_cast<uint8_t>((q >> 24) & 0xFFU));
            bytes.push_back(static_cast<uint8_t>((q >> 16) & 0xFFU));
            bytes.push_back(static_cast<uint8_t>((q >> 8) & 0xFFU));
            bytes.push_back(static_cast<uint8_t>(q & 0xFFU));
        }
        callback(ASFW::Async::AsyncStatus::kSuccess, std::span(bytes.data(), bytes.size()));
    }

    void SimulateFullBIBSuccess(size_t startReadIndex, std::span<const uint32_t> bibBE) {
        ASSERT_GE(bibBE.size(), 5u);

        // ROMReader issues 4 reads: Q0, then Q2, Q3, Q4 (Q1 is prefilled).
        WaitForPendingReads(startReadIndex + 1);
        SimulateReadSuccess(startReadIndex + 0, std::span(&bibBE[0], 1));

        WaitForPendingReads(startReadIndex + 2);
        SimulateReadSuccess(startReadIndex + 1, std::span(&bibBE[2], 1));

        WaitForPendingReads(startReadIndex + 3);
        SimulateReadSuccess(startReadIndex + 2, std::span(&bibBE[3], 1));

        WaitForPendingReads(startReadIndex + 4);
        SimulateReadSuccess(startReadIndex + 3, std::span(&bibBE[4], 1));
    }

    std::vector<PendingRead> pendingReads_;

private:
    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
};

std::vector<uint32_t> CreateMinimalBIB() {
    return {0x04040000, 0, 0, 0, 0};
}

std::vector<uint32_t> CreateBIBWithRootDir() {
    // info_length=4, crc_length=8 => root directory present.
    return {0x04080000, 0, 0, 0, 0};
}

std::vector<uint32_t> CreateRootDir_WithFarTextLeaf() {
    // Root directory (2 entries):
    //   1) immediate VendorId
    //   2) textual descriptor leaf with large offset so EnsurePrefix exceeds cap.
    //
    // Header: len=2, crc=0
    const uint32_t header = 0x00020000;

    // Entry 1: keyType=0 (immediate), keyId=0x03 (ModuleVendorId), value=0x00ABCDEF.
    const uint32_t vendorId = 0x03ABCDEF;

    // Entry 2: keyType=2 (leaf), keyId=0x01 (TextualDescriptor), value = +298 quadlets.
    // targetRel = entryIndex(2) + 298 = 300 quadlets from directory header.
    const uint32_t textLeafFar = 0x8100012A;

    return {header, vendorId, textLeafFar};
}

} // namespace

TEST(ROMScannerAbort, AbortGeneration_IgnoresLateCallbacks) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    int callbackCount = 0;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 42;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    ROMScanRequest request{};
    request.gen = topology.generation;
    request.topology = topology;
    request.localNodeId = 0;
    request.targetNodes = {1};

    ASSERT_TRUE(scanner.Start(
        request,
        [&](Generation /*gen*/, std::vector<ConfigROM> /*roms*/, bool /*busy*/) {
            std::lock_guard lock(mtx);
            ++callbackCount;
            cv.notify_all();
        }));

    mockAsync.WaitForPendingReads(1);
    scanner.Abort(request.gen);

    // Late BIB completions should be ignored (no user completion callback).
    const auto bib = CreateMinimalBIB();
    mockAsync.SimulateReadSuccess(0, std::span(&bib[0], 1));
    mockAsync.WaitForPendingReads(4);
    mockAsync.SimulateReadSuccess(1, std::span(&bib[2], 1));
    mockAsync.SimulateReadSuccess(2, std::span(&bib[3], 1));
    mockAsync.SimulateReadSuccess(3, std::span(&bib[4], 1));

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return callbackCount > 0; });
    }

    EXPECT_EQ(callbackCount, 0);
}

TEST(ROMScannerEnsurePrefix, EnsurePrefixCapExceeded_CompletesDeterministically) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    int callbackCount = 0;
    std::vector<ConfigROM> completedROMs;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 5;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    ROMScanRequest request{};
    request.gen = topology.generation;
    request.topology = topology;
    request.localNodeId = 0;
    request.targetNodes = {1};

    ASSERT_TRUE(scanner.Start(
        request,
        [&](Generation /*gen*/, std::vector<ConfigROM> roms, bool /*busy*/) {
            std::lock_guard lock(mtx);
            ++callbackCount;
            completedROMs = std::move(roms);
            cv.notify_all();
        }));

    // Complete BIB reads (non-minimal => RootDir read).
    const auto bib = CreateBIBWithRootDir();
    mockAsync.SimulateFullBIBSuccess(/*startReadIndex=*/0, bib);

    // RootDir header-first read: header then 2 entries.
    const auto rootDir = CreateRootDir_WithFarTextLeaf();
    mockAsync.WaitForPendingReads(5);
    mockAsync.SimulateReadSuccess(4, std::span(&rootDir[0], 1));
    mockAsync.WaitForPendingReads(6);
    mockAsync.SimulateReadSuccess(5, std::span(&rootDir[1], 1));
    mockAsync.WaitForPendingReads(7);
    mockAsync.SimulateReadSuccess(6, std::span(&rootDir[2], 1));

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return callbackCount > 0; });
    }

    ASSERT_EQ(callbackCount, 1);
    ASSERT_EQ(completedROMs.size(), 1u);
    EXPECT_TRUE(completedROMs[0].vendorName.empty());
    EXPECT_TRUE(completedROMs[0].modelName.empty());
}

