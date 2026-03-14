#include <gtest/gtest.h>

#include <array>
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
#include "../ASFWDriver/IRM/IRMTypes.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;

namespace {

class MockAsyncSubsystem : public ASFW::Async::IFireWireBus {
public:
    struct PendingRead {
        ASFW::FW::Generation generation{0};
        ASFW::FW::NodeId nodeId{0};
        ASFW::Async::FWAddress address{};
        uint32_t length{0};
        ASFW::FW::FwSpeed speed{ASFW::FW::FwSpeed::S100};
        ASFW::Async::InterfaceCompletionCallback callback;
    };

    struct PendingLock {
        ASFW::FW::Generation generation{0};
        ASFW::FW::NodeId nodeId{0};
        ASFW::Async::FWAddress address{};
        ASFW::FW::LockOp op{ASFW::FW::LockOp::kCompareSwap};
        std::vector<uint8_t> operand;
        uint32_t responseLength{0};
        ASFW::FW::FwSpeed speed{ASFW::FW::FwSpeed::S100};
        ASFW::Async::InterfaceCompletionCallback callback;
    };

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation generation,
                                       ASFW::FW::NodeId nodeId,
                                       ASFW::Async::FWAddress address,
                                       uint32_t length,
                                       ASFW::FW::FwSpeed speed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        std::lock_guard lock(mtx_);
        pendingReads_.push_back(PendingRead{
            .generation = generation,
            .nodeId = nodeId,
            .address = address,
            .length = length,
            .speed = speed,
            .callback = std::move(callback),
        });
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

    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation generation,
                                  ASFW::FW::NodeId nodeId,
                                  ASFW::Async::FWAddress address,
                                  ASFW::FW::LockOp lockOp,
                                  std::span<const uint8_t> operand,
                                  uint32_t responseLength,
                                  ASFW::FW::FwSpeed speed,
                                  ASFW::Async::InterfaceCompletionCallback callback) override {
        std::lock_guard lock(mtx_);
        PendingLock pending{};
        pending.generation = generation;
        pending.nodeId = nodeId;
        pending.address = address;
        pending.op = lockOp;
        pending.operand.assign(operand.begin(), operand.end());
        pending.responseLength = responseLength;
        pending.speed = speed;
        pending.callback = std::move(callback);
        pendingLocks_.push_back(std::move(pending));
        cv_.notify_all();
        return ASFW::Async::AsyncHandle{static_cast<uint32_t>(pendingLocks_.size())};
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

    void WaitForPendingLocks(size_t count) const {
        std::unique_lock lock(mtx_);
        cv_.wait_for(lock, std::chrono::seconds(1),
                     [this, count] { return pendingLocks_.size() >= count; });
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

    void SimulateLockSuccess(size_t lockIndex, std::span<const uint8_t> payload) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(mtx_);
            ASSERT_LT(lockIndex, pendingLocks_.size());
            callback = std::move(pendingLocks_[lockIndex].callback);
        }

        ASSERT_TRUE(static_cast<bool>(callback));
        callback(ASFW::Async::AsyncStatus::kSuccess, payload);
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
    std::vector<PendingLock> pendingLocks_;

private:
    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
};

std::vector<uint32_t> CreateMinimalIRMCapableBIB() {
    // Q0: info_length=4, crc_length=4, crc=0x0000 (CRC mismatch is allowed; warning only).
    // Q2: set IRMC (bit 31) so IRM verification path runs when enabled.
    return {0x04040000, 0, 0x80000000, 0, 0};
}

} // namespace

TEST(ROMScannerIRMVerify, IRMVerify_CrcLengthFour_StillReadsRootDirectory) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    int callbackCount = 0;
    bool hadBusyNodes = false;
    std::vector<ConfigROM> completedROMs;

    ROMScannerParams params{};
    params.doIRMCheck = true;

    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 7;
    topology.busBase16 = 0xFFC0;
    topology.irmNodeId = 1;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    ROMScanRequest request{};
    request.gen = Generation{topology.generation};
    request.topology = topology;
    request.localNodeId = 0;
    request.targetNodes = {1};

    ASSERT_TRUE(scanner.Start(
        request,
        [&](Generation /*gen*/, std::vector<ConfigROM> roms, bool busy) {
            std::lock_guard lock(mtx);
            ++callbackCount;
            hadBusyNodes = busy;
            completedROMs = std::move(roms);
            cv.notify_all();
        }));

    const auto bib = CreateMinimalIRMCapableBIB();
    mockAsync.SimulateFullBIBSuccess(/*startReadIndex=*/0, bib);

    // IRM verify should run before the root directory read.
    mockAsync.WaitForPendingReads(5);
    ASSERT_GE(mockAsync.pendingReads_.size(), 5u);

    const auto& irmRead = mockAsync.pendingReads_[4];
    EXPECT_EQ(irmRead.length, 4u);
    EXPECT_EQ(irmRead.address.addressHi, ASFW::IRM::IRMRegisters::kAddressHi);
    EXPECT_EQ(irmRead.address.addressLo, ASFW::IRM::IRMRegisters::kChannelsAvailable63_32);
    EXPECT_EQ(irmRead.speed, ASFW::FW::FwSpeed::S100);

    const std::array<uint32_t, 1> irmValue{0};
    mockAsync.SimulateReadSuccess(4, irmValue);

    mockAsync.WaitForPendingLocks(1);
    ASSERT_GE(mockAsync.pendingLocks_.size(), 1u);

    const auto& irmLock = mockAsync.pendingLocks_[0];
    EXPECT_EQ(irmLock.address.addressHi, ASFW::IRM::IRMRegisters::kAddressHi);
    EXPECT_EQ(irmLock.address.addressLo, ASFW::IRM::IRMRegisters::kChannelsAvailable63_32);
    EXPECT_EQ(irmLock.op, ASFW::FW::LockOp::kCompareSwap);
    EXPECT_EQ(irmLock.operand.size(), 8u);
    EXPECT_EQ(irmLock.responseLength, 4u);
    EXPECT_EQ(irmLock.speed, ASFW::FW::FwSpeed::S100);

    const std::array<uint8_t, 4> lockResponse{};
    mockAsync.SimulateLockSuccess(0, lockResponse);

    mockAsync.WaitForPendingReads(6);
    ASSERT_GE(mockAsync.pendingReads_.size(), 6u);

    const auto& rootDirRead = mockAsync.pendingReads_[5];
    EXPECT_EQ(rootDirRead.length, 4u);
    EXPECT_EQ(rootDirRead.address.addressHi, ASFW::FW::ConfigROMAddr::kAddressHi);
    EXPECT_EQ(rootDirRead.address.addressLo, ASFW::FW::ConfigROMAddr::kAddressLo + 20u);

    const std::array<uint32_t, 1> emptyRootDir{0x00000000};
    mockAsync.SimulateReadSuccess(5, emptyRootDir);

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return callbackCount > 0; });
    }

    EXPECT_EQ(callbackCount, 1);
    EXPECT_FALSE(hadBusyNodes);
    EXPECT_EQ(completedROMs.size(), 1u);
}
