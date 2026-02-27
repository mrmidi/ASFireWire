#include <gtest/gtest.h>

#include <condition_variable>
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

    std::vector<PendingRead> pendingReads_;
    mutable std::mutex readsMtx_;
    mutable std::condition_variable readsCv_;

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation,
                                       ASFW::FW::NodeId,
                                       ASFW::Async::FWAddress,
                                       uint32_t,
                                       ASFW::FW::FwSpeed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        std::lock_guard lock(readsMtx_);
        pendingReads_.push_back(PendingRead{.callback = std::move(callback)});
        readsCv_.notify_all();
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
        std::unique_lock lock(readsMtx_);
        readsCv_.wait_for(lock, std::chrono::seconds(1), [this, count] {
            return pendingReads_.size() >= count;
        });
    }

    void SimulateReadSuccess(size_t readIndex, const std::vector<uint32_t>& quadlets) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(readsMtx_);
            if (readIndex >= pendingReads_.size()) {
                return;
            }
            callback = std::move(pendingReads_[readIndex].callback);
        }

        if (!callback) {
            return;
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(quadlets.size() * 4);
        for (uint32_t q : quadlets) {
            bytes.push_back((q >> 24) & 0xFF);
            bytes.push_back((q >> 16) & 0xFF);
            bytes.push_back((q >> 8) & 0xFF);
            bytes.push_back(q & 0xFF);
        }

        callback(ASFW::Async::AsyncStatus::kSuccess, std::span(bytes.data(), bytes.size()));
    }

    void SimulateFullBIBSuccess(size_t startIdx, const std::vector<uint32_t>& bib) {
        WaitForPendingReads(startIdx + 1);
        SimulateReadSuccess(startIdx + 0, {bib[0]});
        WaitForPendingReads(startIdx + 2);
        SimulateReadSuccess(startIdx + 1, {bib[2]});
        WaitForPendingReads(startIdx + 3);
        SimulateReadSuccess(startIdx + 2, {bib[3]});
        WaitForPendingReads(startIdx + 4);
        SimulateReadSuccess(startIdx + 3, {bib[4]});
    }
};

std::vector<uint32_t> CreateMinimalBIB() {
    return {0x04040000, 0, 0, 0, 0};
}

std::vector<uint32_t> CreateBusyBIB() {
    return {0x00000000, 0, 0, 0, 0};
}

} // namespace

TEST(ROMScannerMultiNodeFSM, AutomaticTwoNodesCompletesOnce) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    int callbackCount = 0;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params, [&](Generation) {
        std::lock_guard lock(mtx);
        callbackCount++;
        cv.notify_all();
    });

    TopologySnapshot topology;
    topology.generation = 11;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});
    topology.nodes.push_back({.nodeId = 2, .linkActive = true});

    scanner.Begin(11, topology, 0);

    // Interleaved BIB completion for two nodes.
    mockAsync.WaitForPendingReads(2);
    mockAsync.SimulateReadSuccess(0, {CreateMinimalBIB()[0]});
    mockAsync.SimulateReadSuccess(1, {CreateMinimalBIB()[0]});

    mockAsync.WaitForPendingReads(4);
    mockAsync.SimulateReadSuccess(2, {0});
    mockAsync.SimulateReadSuccess(3, {0});

    mockAsync.WaitForPendingReads(6);
    mockAsync.SimulateReadSuccess(4, {0});
    mockAsync.SimulateReadSuccess(5, {0});

    mockAsync.WaitForPendingReads(8);
    mockAsync.SimulateReadSuccess(6, {0});
    mockAsync.SimulateReadSuccess(7, {0});

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return callbackCount > 0; });
    }

    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(scanner.IsIdleFor(11));
    EXPECT_EQ(scanner.DrainReady(11).size(), 2u);
}

TEST(ROMScannerMultiNodeFSM, BusyBIBSetsBusyFlagAndRecovers) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params, [&](Generation) {
        std::lock_guard lock(mtx);
        done = true;
        cv.notify_all();
    });

    TopologySnapshot topology;
    topology.generation = 9;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 3, .linkActive = true});

    scanner.Begin(9, topology, 0);

    // First BIB returns not-ready payload (q0 == 0), then retry succeeds.
    mockAsync.SimulateFullBIBSuccess(0, CreateBusyBIB());
    mockAsync.SimulateFullBIBSuccess(4, CreateMinimalBIB());

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return done; });
    }

    EXPECT_TRUE(done);
    EXPECT_TRUE(scanner.HadBusyNodes());
    EXPECT_TRUE(scanner.IsIdleFor(9));
    EXPECT_EQ(scanner.DrainReady(9).size(), 1u);
}
