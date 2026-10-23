#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
#include <memory>

#include "../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "../ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "../ASFWDriver/Controller/ControllerTypes.hpp"
#include "../ASFWDriver/Discovery/SpeedPolicy.hpp"
#include "../ASFWDriver/Common/FWCommon.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;

namespace {

class MockAsyncSubsystem : public ASFW::Async::IFireWireBus {
public:
    struct PendingRead {
        ASFW::Async::InterfaceCompletionCallback callback;
        ASFW::FW::NodeId nodeId{0};
        ASFW::FW::FwSpeed speed{ASFW::FW::FwSpeed::S100};
        ASFW::Async::FWAddress address{};
        uint32_t length{0};
    };

    std::vector<PendingRead> pendingReads_;
    mutable std::mutex readsMtx_;
    mutable std::condition_variable readsCv_;

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation,
                                       ASFW::FW::NodeId nodeId,
                                       ASFW::Async::FWAddress address,
                                       uint32_t length,
                                       ASFW::FW::FwSpeed speed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        std::lock_guard lock(readsMtx_);
        pendingReads_.push_back(PendingRead{.callback = std::move(callback),
                                            .nodeId = nodeId, .speed = speed,
                                            .address = address, .length = length});
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
    void FailRead(size_t index, ASFW::Async::AsyncStatus status = ASFW::Async::AsyncStatus::kTimeout) {
        ASFW::Async::InterfaceCompletionCallback callback;
        {
            std::lock_guard lock(readsMtx_);
            callback = std::move(pendingReads_.at(index).callback);
        }
        if (callback) callback(status, {});
    }
};

std::vector<uint32_t> CreateStandardBIBWithCrcLength4() {
    return {0x04040000, 0, 0, 0, 0};
}

std::vector<uint32_t> CreateBusyBIB() {
    return {0x00000000, 0, 0, 0, 0};
}

} // namespace

class ROMSpeedProbeTests : public ::testing::Test {
protected:
    void TearDown() override {
        // Abort callbacks capture the fixture; retire them before its members.
        if (scanner) scanner->Abort();
        scanner.reset();
    }
    void Start(uint8_t pathSpeed, uint8_t linkSpeed, bool betaRepeaters = false,
               bool expectProbe = true, uint8_t perStepRetries = 0,
               bool failInitialBIB = false) {
        ROMScannerParams params{};
        params.perStepRetries = perStepRetries;
        params.configROMReadyRetries = 0;
        scanner = std::make_unique<ROMScanner>(bus, policy, params);
        TopologySnapshot topology{};
        topology.generation = 1;
        topology.localNodeId = 0;
        topology.rootNodeId = 1;
        topology.betaRepeatersPresent = betaRepeaters;
        topology.graphStatus = TopologyGraphStatus::Valid;
        topology.physical.nodes.push_back({.physicalId = 0, .linkActive = true,
                                           .speedCode = 3, .portCount = 1});
        topology.physical.nodes.push_back({.physicalId = 1, .linkActive = true,
                                           .speedCode = pathSpeed, .portCount = 1});
        topology.physical.nodes[0].links[0] = {.connected = true, .remoteNodeId = 1};
        topology.physical.nodes[1].links[0] = {.connected = true, .remoteNodeId = 0};
        auto options = ASFW::FW::DecodeBusOptions(0);
        options.linkSpd = linkSpeed;
        bib = {0x04040000, 0x31333934, ASFW::FW::EncodeBusOptions(options), 0x000a9204, 0x050839ae};
        ROMScanRequest request{};
        request.gen = Generation{1};
        request.topology = topology;
        request.localNodeId = 0;
        request.rootCapabilityCallback = [this](auto value) {
            evidence.push_back(value);
            if (value.bibReadStatus == ASFW::Driver::Role::RootBibReadStatus::Success)
                speedAtRootEvidence = policy.ObservedSpeed(ASFW::FW::NodeId{1});
        };
        ASSERT_TRUE(scanner->Start(request, [this](Generation, std::vector<ConfigROM> roms, bool) {
            ++completions;
            completed = std::move(roms);
        }));
        const size_t bibStart = failInitialBIB ? 1 : 0;
        if (failInitialBIB) bus.FailRead(0);
        bus.SimulateFullBIBSuccess(bibStart, bib);
        bus.WaitForPendingReads(bibStart + 5);
        ASSERT_GE(bus.pendingReads_.size(), bibStart + 5);
        for (size_t i = 0; i < bibStart + 4; ++i)
            EXPECT_EQ(bus.pendingReads_[i].speed, FwSpeed::S100);
        if (expectProbe) EXPECT_FALSE(policy.ObservedSpeed(ASFW::FW::NodeId{1}));
    }
    void ExpectRead(size_t index, FwSpeed speed, uint32_t address) {
        bus.WaitForPendingReads(index + 1);
        ASSERT_GT(bus.pendingReads_.size(), index);
        EXPECT_EQ(bus.pendingReads_[index].speed, speed);
        EXPECT_EQ(bus.pendingReads_[index].address.addressLo, address);
        EXPECT_EQ(bus.pendingReads_[index].length, 4U);
    }
    MockAsyncSubsystem bus;
    SpeedPolicy policy;
    std::unique_ptr<ROMScanner> scanner;
    std::vector<uint32_t> bib;
    uint32_t completions{0};
    std::vector<ConfigROM> completed;
    std::vector<ASFW::Driver::Role::RootCapabilityEvidence> evidence;
    std::optional<FwSpeed> speedAtRootEvidence;
};

TEST_F(ROMSpeedProbeTests, S800ProbeSucceedsBeforeRootDirectory) {
    Start(3, 3);
    ExpectRead(4, FwSpeed::S800, 0xf0000400);
    ASSERT_EQ(evidence.size(), 1U);
    EXPECT_EQ(evidence.front().bibReadStatus, ASFW::Driver::Role::RootBibReadStatus::Pending);
    bus.SimulateReadSuccess(4, {bib[0]});
    ExpectRead(5, FwSpeed::S800, 0xf0000414);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S800);
    ASSERT_EQ(evidence.size(), 2U);
    EXPECT_EQ(speedAtRootEvidence, FwSpeed::S800);
    bus.SimulateReadSuccess(5, {0});
    EXPECT_EQ(completions, 1U);
    ASSERT_EQ(completed.size(), 1U);
    EXPECT_EQ(completed.front().bib.guid, 0x000a9204050839aeULL);
}

TEST_F(ROMSpeedProbeTests, ProbeStepsDownUntilAHeaderReadWorks) {
    Start(3, 3);
    ExpectRead(4, FwSpeed::S800, 0xf0000400);
    bus.FailRead(4);
    ExpectRead(5, FwSpeed::S400, 0xf0000400);
    bus.FailRead(5);
    ExpectRead(6, FwSpeed::S200, 0xf0000400);
    bus.SimulateReadSuccess(6, {bib[0]});
    ExpectRead(7, FwSpeed::S200, 0xf0000414);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S200);
    bus.SimulateReadSuccess(7, {0});
    EXPECT_EQ(completions, 1U);
}

TEST_F(ROMSpeedProbeTests, UnderreportedRomLinkSpeedDoesNotLimitProbeStart) {
    Start(2, 0);
    ExpectRead(4, FwSpeed::S400, 0xf0000400);
    bus.SimulateReadSuccess(4, {bib[0]});
    ExpectRead(5, FwSpeed::S400, 0xf0000414);
}

TEST_F(ROMSpeedProbeTests, BetaRepeaterTriggersProbeOnAnS400Path) {
    Start(2, 2, true);
    ExpectRead(4, FwSpeed::S400, 0xf0000400);
}

TEST_F(ROMSpeedProbeTests, AbortDuringProbeDoesNotPublishOrTryASlowerSpeed) {
    Start(3, 3);
    scanner->Abort();
    bus.SimulateReadSuccess(4, {bib[0]});
    EXPECT_EQ(bus.pendingReads_.size(), 5U);
    EXPECT_EQ(completions, 0U);
    EXPECT_FALSE(policy.ObservedSpeed(ASFW::FW::NodeId{1}));
    ASSERT_EQ(evidence.size(), 2U);
    EXPECT_EQ(evidence.back().bibReadStatus, ASFW::Driver::Role::RootBibReadStatus::AbortedByReset);
}

TEST_F(ROMSpeedProbeTests, StaleGenerationFailureDoesNotLearnASlowerCeiling) {
    Start(3, 3);
    bus.FailRead(4, ASFW::Async::AsyncStatus::kStaleGeneration);
    EXPECT_EQ(bus.pendingReads_.size(), 5U);
    EXPECT_EQ(completions, 1U);
    EXPECT_TRUE(completed.empty());
    EXPECT_FALSE(policy.ObservedSpeed(ASFW::FW::NodeId{1}));
}

TEST_F(ROMSpeedProbeTests, MatchingLegacyLinkSkipsProbeAndReadsRootAtPathSpeed) {
    Start(2, 2, false, false);
    ExpectRead(4, FwSpeed::S400, 0xf0000414);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S400);
    bus.SimulateReadSuccess(4, {0});
    EXPECT_EQ(completions, 1U);
}

TEST_F(ROMSpeedProbeTests, RootDirectoryRetriesExhaustBeforeFallingBackToS200) {
    Start(2, 2, false, false, 2);
    size_t rootRead = 4;
    for (size_t attempt = 0; attempt < 3; ++attempt) {
        ExpectRead(rootRead, FwSpeed::S400, 0xf0000414);
        bus.FailRead(rootRead);
        ExpectRead(rootRead + 1, FwSpeed::S100, 0xf0000400);
        bus.SimulateFullBIBSuccess(rootRead + 1, bib);
        rootRead += 5;
    }
    ExpectRead(rootRead, FwSpeed::S200, 0xf0000414);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S200);
    bus.SimulateReadSuccess(rootRead, {0});
    EXPECT_EQ(completions, 1U);
    ASSERT_EQ(completed.size(), 1U);
    EXPECT_EQ(completed.front().bib.guid, 0x000a9204050839aeULL);
    EXPECT_EQ(bus.pendingReads_.size(), 20U);
}

TEST_F(ROMSpeedProbeTests, RootDirectoryFailureAtEverySpeedTerminatesAtS100) {
    Start(2, 2, false, false, 2);
    size_t rootRead = 4;
    for (const auto speed : {FwSpeed::S400, FwSpeed::S200, FwSpeed::S100}) {
        for (size_t attempt = 0; attempt < 3; ++attempt) {
            ExpectRead(rootRead, speed, 0xf0000414);
            bus.FailRead(rootRead);
            if (speed == FwSpeed::S100 && attempt == 2) break;
            ExpectRead(rootRead + 1, FwSpeed::S100, 0xf0000400);
            bus.SimulateFullBIBSuccess(rootRead + 1, bib);
            rootRead += 5;
        }
    }
    EXPECT_EQ(completions, 1U);
    EXPECT_TRUE(completed.empty());
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S100);
    EXPECT_EQ(bus.pendingReads_.size(), 45U);
}

TEST_F(ROMSpeedProbeTests, TransientRootDirectoryFailureKeepsOperationalSpeed) {
    Start(2, 2, false, false, 2);
    bus.FailRead(4);
    bus.SimulateFullBIBSuccess(5, bib);
    ExpectRead(9, FwSpeed::S400, 0xf0000414);
    bus.SimulateReadSuccess(9, {0});
    EXPECT_EQ(completions, 1U);
    EXPECT_EQ(completed.size(), 1U);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S400);
}

TEST_F(ROMSpeedProbeTests, InitialBIBFailureDoesNotConsumeRootDirectoryRetryBudget) {
    Start(2, 2, false, false, 2, true);
    size_t rootRead = 5;
    for (size_t attempt = 0; attempt < 2; ++attempt) {
        ExpectRead(rootRead, FwSpeed::S400, 0xf0000414);
        bus.FailRead(rootRead);
        bus.SimulateFullBIBSuccess(rootRead + 1, bib);
        rootRead += 5;
    }
    ExpectRead(rootRead, FwSpeed::S400, 0xf0000414);
    bus.SimulateReadSuccess(rootRead, {0});
    EXPECT_EQ(completions, 1U);
    EXPECT_EQ(completed.size(), 1U);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S400);
}

TEST_F(ROMSpeedProbeTests, FailedFastProbesUseTheAlreadyVerifiedS100Floor) {
    Start(3, 3);
    for (size_t i = 0; i < 3; ++i) {
        ExpectRead(4 + i, static_cast<FwSpeed>(3 - i), 0xf0000400);
        bus.FailRead(4 + i);
    }
    ExpectRead(7, FwSpeed::S100, 0xf0000414);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S100);
    bus.SimulateReadSuccess(7, {0});
    EXPECT_EQ(completions, 1U);
    EXPECT_EQ(completed.size(), 1U);
    EXPECT_EQ(bus.pendingReads_.size(), 8U);
}

TEST_F(ROMSpeedProbeTests, LaterDirectoryFailureKeepsFallbackBelowTheProbedSpeed) {
    Start(3, 3);
    bus.SimulateReadSuccess(4, {bib[0]});
    ExpectRead(5, FwSpeed::S800, 0xf0000414);
    bus.FailRead(5);
    ExpectRead(6, FwSpeed::S100, 0xf0000400);
    bus.SimulateFullBIBSuccess(6, bib);
    ExpectRead(10, FwSpeed::S400, 0xf0000400);
    bus.SimulateReadSuccess(10, {bib[0]});
    ExpectRead(11, FwSpeed::S400, 0xf0000414);
    EXPECT_EQ(policy.ObservedSpeed(ASFW::FW::NodeId{1}), FwSpeed::S400);
    bus.SimulateReadSuccess(11, {0});
    EXPECT_EQ(completions, 1U);
}

TEST(ROMScannerMultiNodeFSM, InitialBIBReadUsesS100EvenOnFasterPhysicalPath) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;
    ROMScannerParams params{};
    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology{};
    topology.generation = 1;
    topology.localNodeId = 0;
    topology.graphStatus = TopologyGraphStatus::Valid;
    topology.physical.nodes.push_back({.physicalId = 0, .linkActive = true,
                                       .speedCode = 3, .portCount = 1});
    topology.physical.nodes.push_back({.physicalId = 1, .linkActive = true,
                                       .speedCode = 1, .portCount = 1});
    topology.physical.nodes[0].links[0].connected = true;
    topology.physical.nodes[0].links[0].remoteNodeId = 1;
    topology.physical.nodes[1].links[0].connected = true;
    topology.physical.nodes[1].links[0].remoteNodeId = 0;

    ROMScanRequest request{};
    request.gen = Generation{1};
    request.topology = topology;
    request.localNodeId = 0;
    ASSERT_TRUE(scanner.Start(request, [](Generation, std::vector<ConfigROM>, bool) {}));

    mockAsync.WaitForPendingReads(1);
    {
        std::lock_guard lock(mockAsync.readsMtx_);
        ASSERT_EQ(mockAsync.pendingReads_.size(), 1u);
        EXPECT_EQ(mockAsync.pendingReads_[0].nodeId.value, 1);
        EXPECT_EQ(mockAsync.pendingReads_[0].speed, ASFW::FW::FwSpeed::S100);
    }
    scanner.Abort();
}

TEST(ROMScannerMultiNodeFSM, AutomaticTwoNodesCompletesOnce) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    int callbackCount = 0;
    bool hadBusyNodes = false;
    std::vector<ConfigROM> completedROMs;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 11;
    topology.busBase16 = 0xFFC0;
    topology.physical.nodes.push_back({.physicalId = 1, .linkActive = true});
    topology.physical.nodes.push_back({.physicalId = 2, .linkActive = true});

    ROMScanRequest request{};
    request.gen = Generation{topology.generation};
    request.topology = topology;
    request.localNodeId = 0;

    ASSERT_TRUE(scanner.Start(
        request,
        [&](Generation /*gen*/, std::vector<ConfigROM> roms, bool busy) {
            std::lock_guard lock(mtx);
            callbackCount++;
            hadBusyNodes = busy;
            completedROMs = std::move(roms);
            cv.notify_all();
        }));

    // Interleaved BIB completion for two nodes.
    mockAsync.WaitForPendingReads(2);
    mockAsync.SimulateReadSuccess(0, {CreateStandardBIBWithCrcLength4()[0]});
    mockAsync.SimulateReadSuccess(1, {CreateStandardBIBWithCrcLength4()[0]});

    mockAsync.WaitForPendingReads(4);
    mockAsync.SimulateReadSuccess(2, {0});
    mockAsync.SimulateReadSuccess(3, {0});

    mockAsync.WaitForPendingReads(6);
    mockAsync.SimulateReadSuccess(4, {0});
    mockAsync.SimulateReadSuccess(5, {0});

    mockAsync.WaitForPendingReads(8);
    mockAsync.SimulateReadSuccess(6, {0});
    mockAsync.SimulateReadSuccess(7, {0});

    // Full root-directory parse (general ROM, crc_length == bus_info_length): each
    // node now reads its root-directory header. An empty header (0 entries) completes
    // the node. See ROMScanSession::ContinueAfterBIBSuccess (always reads the root dir;
    // cross-validated with Linux: firewire/core-device.c:650-652).
    mockAsync.WaitForPendingReads(10);
    mockAsync.SimulateReadSuccess(8, {0});
    mockAsync.SimulateReadSuccess(9, {0});

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return callbackCount > 0; });
    }

    EXPECT_EQ(callbackCount, 1);
    EXPECT_FALSE(hadBusyNodes);
    EXPECT_EQ(completedROMs.size(), 2u);
}

TEST(ROMScannerMultiNodeFSM, BusyBIBSetsBusyFlagAndRecovers) {
    MockAsyncSubsystem mockAsync;
    SpeedPolicy speedPolicy;

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool hadBusyNodes = false;
    std::vector<ConfigROM> completedROMs;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(mockAsync, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 9;
    topology.busBase16 = 0xFFC0;
    topology.physical.nodes.push_back({.physicalId = 3, .linkActive = true});

    ROMScanRequest request{};
    request.gen = Generation{topology.generation};
    request.topology = topology;
    request.localNodeId = 0;

    ASSERT_TRUE(scanner.Start(
        request,
        [&](Generation /*gen*/, std::vector<ConfigROM> roms, bool busy) {
            std::lock_guard lock(mtx);
            done = true;
            hadBusyNodes = busy;
            completedROMs = std::move(roms);
            cv.notify_all();
        }));

    // First BIB returns not-ready payload (q0 == 0), then retry succeeds.
    mockAsync.SimulateFullBIBSuccess(0, CreateBusyBIB());
    mockAsync.SimulateFullBIBSuccess(4, CreateStandardBIBWithCrcLength4());

    // Full root-directory parse: the recovered node reads its (empty) root-directory
    // header to complete (general ROM with crc_length == bus_info_length).
    mockAsync.WaitForPendingReads(9);
    mockAsync.SimulateReadSuccess(8, {0});

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return done; });
    }

    EXPECT_TRUE(done);
    EXPECT_TRUE(hadBusyNodes);
    EXPECT_EQ(completedROMs.size(), 1u);
}
