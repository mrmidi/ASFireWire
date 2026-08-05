#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ASFWDriver/Async/Interfaces/IAsyncControllerPort.hpp"
#include "ASFWDriver/Bus/BusManager.hpp"
#include "ASFWDriver/Bus/BusResetCoordinator.hpp"
#include "ASFWDriver/Bus/SelfIDCapture.hpp"
#include "ASFWDriver/Bus/TopologyManager.hpp"
#include "ASFWDriver/ConfigROM/ConfigROMStager.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/InterruptManager.hpp"
#include "ASFWDriver/Hardware/OHCIConstants.hpp"
#include "ASFWDriver/Hardware/RegisterMap.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"

namespace ASFW::Driver {

class SelfIDCaptureTestPeer {
  public:
    static uint32_t* MutableQuadlets(SelfIDCapture& capture) {
        return reinterpret_cast<uint32_t*>(capture.map_->GetAddress());
    }
};

class BusResetCoordinatorTestPeer {
  public:
    static void Attach(BusResetCoordinator& coordinator, HardwareInterface& hardware,
                       InterruptManager* interrupts = nullptr) {
        coordinator.hardware_ = &hardware;
        coordinator.interruptManager_ = interrupts;
    }

    static void SetSelfIDLatch(BusResetCoordinator& coordinator, bool complete, bool sticky) {
        coordinator.selfIdLatch_.complete = complete;
        coordinator.selfIdLatch_.stickyComplete = sticky;
    }

    static void ClearConsumedSelfIDInterrupts(BusResetCoordinator& coordinator) {
        coordinator.ClearConsumedSelfIDInterrupts();
    }

    static void RequestRecoveryReset(BusResetCoordinator& coordinator, bool longReset = false) {
        coordinator.RequestSoftwareReset(
            {BusResetCoordinator::ResetRequestKind::Recovery,
             longReset ? BusResetCoordinator::ResetFlavor::Long
                       : BusResetCoordinator::ResetFlavor::Short,
             std::nullopt, "Recovery"});
    }

    static void RequestGapCorrectionReset(BusResetCoordinator& coordinator, bool longReset,
                                          std::optional<uint8_t> gapCount) {
        BusManager::PhyConfigCommand command{};
        command.gapCount = gapCount;
        coordinator.RequestSoftwareReset(
            {BusResetCoordinator::ResetRequestKind::GapCorrection,
             longReset ? BusResetCoordinator::ResetFlavor::Long
                       : BusResetCoordinator::ResetFlavor::Short,
             command, "GapCorrection"});
    }

    static void RequestMismatchForce63Reset(BusResetCoordinator& coordinator, bool longReset) {
        BusManager::PhyConfigCommand command{};
        command.gapCount = 63U;
        coordinator.RequestSoftwareReset(
            {BusResetCoordinator::ResetRequestKind::GapCorrection,
             longReset ? BusResetCoordinator::ResetFlavor::Long
                       : BusResetCoordinator::ResetFlavor::Short,
             command, "MismatchForce63", BusManager::GapDecisionReason::MismatchForce63});
    }

    static void RequestDelegationReset(BusResetCoordinator& coordinator, bool longReset,
                                       std::optional<uint8_t> forceRootNodeId) {
        BusManager::PhyConfigCommand command{};
        command.forceRootNodeID = forceRootNodeId;
        command.setContender = false;
        coordinator.RequestSoftwareReset(
            {BusResetCoordinator::ResetRequestKind::Delegation,
             longReset ? BusResetCoordinator::ResetFlavor::Long
                       : BusResetCoordinator::ResetFlavor::Short,
             command, "Delegation"});
    }

    static bool HasPendingSoftwareReset(const BusResetCoordinator& coordinator) {
        return coordinator.cycle_.pendingReset.has_value();
    }

    static bool PendingResetIsLong(const BusResetCoordinator& coordinator) {
        return coordinator.cycle_.pendingReset.has_value() &&
               coordinator.cycle_.pendingReset->flavor ==
                   BusResetCoordinator::ResetFlavor::Long;
    }

    static bool PendingResetIsGapCorrection(const BusResetCoordinator& coordinator) {
        return coordinator.cycle_.pendingReset.has_value() &&
               coordinator.cycle_.pendingReset->kind ==
                   BusResetCoordinator::ResetRequestKind::GapCorrection;
    }

    static bool PendingResetIsDelegation(const BusResetCoordinator& coordinator) {
        return coordinator.cycle_.pendingReset.has_value() &&
               coordinator.cycle_.pendingReset->kind ==
                   BusResetCoordinator::ResetRequestKind::Delegation;
    }

    static bool PendingResetIsRolePolicyRestage(const BusResetCoordinator& coordinator) {
        return coordinator.cycle_.pendingReset.has_value() &&
               coordinator.cycle_.pendingReset->kind ==
                   BusResetCoordinator::ResetRequestKind::RolePolicyRestage;
    }

    static std::optional<uint8_t> PendingGapCount(const BusResetCoordinator& coordinator) {
        if (!coordinator.cycle_.pendingReset.has_value() ||
            !coordinator.cycle_.pendingReset->phyConfig.has_value()) {
            return std::nullopt;
        }
        return coordinator.cycle_.pendingReset->phyConfig->gapCount;
    }

    static std::optional<uint8_t> PendingForceRootNode(const BusResetCoordinator& coordinator) {
        if (!coordinator.cycle_.pendingReset.has_value() ||
            !coordinator.cycle_.pendingReset->phyConfig.has_value()) {
            return std::nullopt;
        }
        return coordinator.cycle_.pendingReset->phyConfig->forceRootNodeID;
    }
};

} // namespace ASFW::Driver

using namespace ASFW::Driver;

namespace {

constexpr uint32_t kNodeIdValidBit = 0x80000000U;
constexpr uint64_t kStartTimeNs = 1'000'000'000ULL;

uint32_t MakeBaseSelfID(uint8_t phyId, uint8_t gapCount, bool linkActive = true,
                        bool contender = false) {
    uint32_t quadlet = 0x80000000U;
    quadlet |= (static_cast<uint32_t>(phyId) & 0x3FU) << 24U;
    if (linkActive) {
        quadlet |= 1U << 22U;
    }
    quadlet |= (static_cast<uint32_t>(gapCount) & 0x3FU) << 16U;
    quadlet |= 0x2U << 14U;
    if (contender) {
        quadlet |= 1U << 11U;
    }
    // Auto-generate valid linear daisy-chain port states
    if (phyId == 0) {
        quadlet |= (2U << 6U); // port0 = Parent
    } else {
        quadlet |= (3U << 6U); // port0 = Child
        quadlet |= (2U << 4U); // port1 = Parent
    }
    return quadlet;
}

uint32_t MakeSelfIDHeader(uint8_t generation) {
    return static_cast<uint32_t>(generation) << 16U;
}

uint32_t MakeSelfIDCountRegister(uint8_t generation, uint32_t quadletCount) {
    return (static_cast<uint32_t>(generation) << SelfIDCountBits::kGenerationShift) |
           (quadletCount << SelfIDCountBits::kSizeShift);
}

std::vector<uint32_t> MakeRawSelfIDCapture(uint8_t generation,
                                           std::initializer_list<uint32_t> selfIdQuadlets) {
    std::vector<uint32_t> raw;
    raw.reserve(1U + selfIdQuadlets.size() * 2U);
    raw.push_back(MakeSelfIDHeader(generation));
    for (const uint32_t quadlet : selfIdQuadlets) {
        raw.push_back(quadlet);
        raw.push_back(~quadlet);
    }
    return raw;
}

class AsyncControllerStub final : public ASFW::Async::IAsyncControllerPort {
  public:
    kern_return_t ArmARContextsOnly() override { return kIOReturnSuccess; }
    void PostToWorkloop(void (^block)()) override {
        if (block != nullptr) {
            block();
        }
    }

    void OnTxInterrupt() override {}
    void OnRxRequestInterrupt() override {}
    void OnRxResponseInterrupt() override {}

    void OnBusResetBegin(uint8_t nextGen) override { beginGenerations.push_back(nextGen); }
    void OnBusResetComplete(uint8_t stableGen) override {
        completeGenerations.push_back(stableGen);
    }
    void ConfirmBusGeneration(uint8_t confirmedGeneration) override {
        confirmedGenerations.push_back(confirmedGeneration);
    }
    void StopATContextsOnly() override { ++stopAtContextsCount; }
    void FlushATContexts() override { ++flushAtContextsCount; }
    void RearmATContexts() override { ++rearmAtContextsCount; }

    [[nodiscard]] ASFW::Async::AsyncBusStateSnapshot GetBusStateSnapshot() const override {
        return {};
    }

    [[nodiscard]] ASFW::Shared::DMAMemoryManager* GetDMAManager() override { return nullptr; }

    ASFW::Async::AsyncHandle Read(const ASFW::Async::ReadParams&,
                                  ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle ReadWithRetry(const ASFW::Async::ReadParams&,
                                           const ASFW::Async::RetryPolicy&,
                                           ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle Write(const ASFW::Async::WriteParams&,
                                   ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle Lock(const ASFW::Async::LockParams&, uint16_t,
                                  ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle CompareSwap(const ASFW::Async::CompareSwapParams&,
                                         ASFW::Async::CompareSwapCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle PhyRequest(const ASFW::Async::PhyParams&,
                                        ASFW::Async::CompletionCallback) override {
        return {};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    void OnTimeoutTick() override {}

    [[nodiscard]] ASFW::Async::AsyncWatchdogStats GetWatchdogStats() const override {
        return {};
    }
    [[nodiscard]] ASFW::Debug::BusResetPacketCapture* GetBusResetCapture() const override {
        return nullptr;
    }
    [[nodiscard]] std::optional<ASFW::Async::AsyncStatusSnapshot> GetStatusSnapshot() const override {
        return std::nullopt;
    }
    [[nodiscard]] ASFW::Debug::AsyncTraceCapture* GetAsyncTraceCapture() const override {
        return nullptr;
    }
    [[nodiscard]] ASFWDiagInboundCSRStats* GetInboundCSRStats() const override {
        return nullptr;
    }

    std::vector<uint8_t> beginGenerations;
    std::vector<uint8_t> confirmedGenerations;
    std::vector<uint8_t> completeGenerations;
    uint32_t stopAtContextsCount{0};
    uint32_t flushAtContextsCount{0};
    uint32_t rearmAtContextsCount{0};
};

struct BusResetTestRig {
    OSSharedPtr<IODispatchQueue> queue{new IODispatchQueue(), OSNoRetain};
    HardwareInterface hardware;
    InterruptManager interrupts;
    AsyncControllerStub async;
    SelfIDCapture selfIdCapture;
    ConfigROMStager configRomStager;
    TopologyManager topologyManager;
    BusManager busManager;
    BusResetCoordinator coordinator;

    uint64_t nowNs{kStartTimeNs};
    std::vector<TopologySnapshot> publishedTopologies;

    BusResetTestRig() {
        queue->SetManualDispatchForTesting(true);
        ASFW::Testing::SetHostMonotonicClockForTesting([this]() { return nowNs; });
    }

    ~BusResetTestRig() {
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }

    void Initialize(bool withBusManager = false) {
        ASSERT_EQ(selfIdCapture.PrepareBuffers(64U, hardware), kIOReturnSuccess);

        SetLocalNode(0U);
        hardware.SetTestRegister(
            Register32FromOffsetUnchecked(DMAContextHelpers::AsReqTrContextControlSet), 0U);
        hardware.SetTestRegister(
            Register32FromOffsetUnchecked(DMAContextHelpers::AsRspTrContextControlSet), 0U);

        coordinator.Initialize(&hardware, queue, &async, &selfIdCapture, &configRomStager,
                               &interrupts, &topologyManager,
                               withBusManager ? &busManager : nullptr, nullptr);
        coordinator.BindCallbacks([this](const TopologySnapshot& topology) {
            publishedTopologies.push_back(topology);
        });
    }

    void SetLocalNode(uint8_t nodeId) {
        hardware.SetTestRegister(Register32::kNodeID,
                                 kNodeIdValidBit | static_cast<uint32_t>(nodeId & 0x3FU));
    }

    void StartResetCycle() {
        TriggerIrq(IntEventBits::kBusReset);
        DrainReady();
        ASSERT_EQ(coordinator.GetState(), BusResetCoordinator::State::WaitingSelfID);
    }

    void PrimeCapture(std::span<const uint32_t> rawCapture, uint8_t countGeneration) {
        auto* quadlets = SelfIDCaptureTestPeer::MutableQuadlets(selfIdCapture);
        std::fill_n(quadlets, rawCapture.size(), 0U);
        std::copy(rawCapture.begin(), rawCapture.end(), quadlets);

        hardware.SetTestRegister(Register32::kSelfIDCount,
                                 MakeSelfIDCountRegister(countGeneration,
                                                         static_cast<uint32_t>(rawCapture.size())));
    }

    void TriggerStickyCompletion() {
        TriggerIrq(IntEventBits::kSelfIDComplete2);
        DrainReady();
    }

    void AdvanceMs(uint32_t milliseconds) {
        nowNs += static_cast<uint64_t>(milliseconds) * 1'000'000ULL;
        DrainReady();
    }

    void DrainReady() {
        while (queue->DrainReadyForTesting() > 0U) {
        }
    }

    void ResetHardwareState() {
        hardware.ResetTestState();
        SetLocalNode(0U);
        hardware.SetTestRegister(
            Register32FromOffsetUnchecked(DMAContextHelpers::AsReqTrContextControlSet), 0U);
        hardware.SetTestRegister(
            Register32FromOffsetUnchecked(DMAContextHelpers::AsRspTrContextControlSet), 0U);
    }

    void SetSendPhyConfigResult(const bool success) {
        hardware.SetTestSendPhyConfigResult(success);
    }

    void SetInitiateBusResetResult(const bool success) {
        hardware.SetTestInitiateBusResetResult(success);
    }

  private:
    void TriggerIrq(uint32_t intEvent) {
        hardware.SetTestRegister(Register32::kIntEvent,
                                 hardware.GetTestRegister(Register32::kIntEvent) | intEvent);
        coordinator.OnIrq(intEvent, nowNs);
    }
};

} // namespace

TEST(BusResetCoordinatorTests, StableResetPublishesTopologyExactlyOnce) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.StartResetCycle();

    const auto rawCapture = MakeRawSelfIDCapture(
        7U, {MakeBaseSelfID(0U, 63U, true, true), MakeBaseSelfID(1U, 63U)});
    rig.PrimeCapture(rawCapture, 7U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(100U);

    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_EQ(rig.publishedTopologies.front().generation, 7U);
    EXPECT_EQ(rig.publishedTopologies.front().physical.nodes.size(), 2U);
    EXPECT_EQ(rig.coordinator.GetState(), BusResetCoordinator::State::Idle);
    EXPECT_EQ(rig.async.beginGenerations, std::vector<uint8_t>({8U}));
    EXPECT_EQ(rig.async.confirmedGenerations, std::vector<uint8_t>({7U}));
    EXPECT_EQ(rig.async.completeGenerations, std::vector<uint8_t>({7U}));
    EXPECT_EQ(rig.async.stopAtContextsCount, 1U);
    EXPECT_EQ(rig.async.flushAtContextsCount, 1U);
    EXPECT_EQ(rig.async.rearmAtContextsCount, 1U);
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    const auto operations = rig.hardware.CopyTestOperations();
    EXPECT_TRUE(std::find(operations.begin(), operations.end(),
                          HardwareInterface::TestOperation::SendPhyGlobalResume) ==
                operations.end());
}

TEST(BusResetCoordinatorTests, StableResetDelaysDiscoveryByAppleScanDelay) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.StartResetCycle();

    const auto rawCapture = MakeRawSelfIDCapture(
        7U, {MakeBaseSelfID(0U, 63U, true, true), MakeBaseSelfID(1U, 63U)});
    rig.PrimeCapture(rawCapture, 7U);
    rig.TriggerStickyCompletion();

    rig.AdvanceMs(99U);
    EXPECT_TRUE(rig.publishedTopologies.empty());

    rig.AdvanceMs(1U);
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_EQ(rig.publishedTopologies.front().generation, 7U);
}

TEST(BusResetCoordinatorTests, StickyCompletionOnlyStillCompletesDecodePath) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.StartResetCycle();

    const auto rawCapture = MakeRawSelfIDCapture(
        3U, {MakeBaseSelfID(0U, 63U, true, false), MakeBaseSelfID(1U, 63U)});
    rig.PrimeCapture(rawCapture, 3U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(100U);

    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_EQ(rig.publishedTopologies.front().generation, 3U);
    EXPECT_EQ(rig.async.confirmedGenerations, std::vector<uint8_t>({3U}));
}

TEST(BusResetCoordinatorTests, InvalidInversePairRequestsShortRecoveryReset) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.StartResetCycle();

    const uint32_t node0 = MakeBaseSelfID(0U, 63U);
    const std::vector<uint32_t> rawCapture{MakeSelfIDHeader(9U), node0, 0xDEADBEEFU};
    rig.PrimeCapture(rawCapture, 9U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(1U);

    // Should be deferred due to the 2s holdoff from Self-ID completion.
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    rig.AdvanceMs(1999U);

    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_TRUE(rig.hardware.TestLastBusResetWasShort());
    EXPECT_TRUE(rig.publishedTopologies.empty());
    ASSERT_TRUE(rig.coordinator.Metrics().lastFailureReason.has_value());
    EXPECT_NE(rig.coordinator.Metrics().lastFailureReason->find("InvalidInversePair"),
              std::string::npos);
}

TEST(BusResetCoordinatorTests, GenerationMismatchRequestsShortRecoveryReset) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.StartResetCycle();

    const auto rawCapture = MakeRawSelfIDCapture(10U, {MakeBaseSelfID(0U, 63U)});
    rig.PrimeCapture(rawCapture, 11U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(1U);

    // Should be deferred due to the 2s holdoff from Self-ID completion.
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    rig.AdvanceMs(1999U);

    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_TRUE(rig.hardware.TestLastBusResetWasShort());
    EXPECT_TRUE(rig.publishedTopologies.empty());
    ASSERT_TRUE(rig.coordinator.Metrics().lastFailureReason.has_value());
    EXPECT_NE(rig.coordinator.Metrics().lastFailureReason->find("GenerationMismatch"),
              std::string::npos);
}

TEST(BusResetCoordinatorTests, InvalidTopologyDoesNotReusePreviouslyPublishedSnapshot) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.StartResetCycle();
    const auto stableCapture =
        MakeRawSelfIDCapture(12U, {MakeBaseSelfID(0U, 63U, true, true), MakeBaseSelfID(1U, 63U)});
    rig.PrimeCapture(stableCapture, 12U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(100U);

    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    rig.ResetHardwareState();

    rig.StartResetCycle();
    const auto invalidCapture =
        MakeRawSelfIDCapture(13U, {MakeBaseSelfID(0U, 63U, false, false),
                                   MakeBaseSelfID(2U, 63U, false, false)});
    rig.PrimeCapture(invalidCapture, 13U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(1U);

    // Previous snapshot should not be re-published, and no recovery reset should be issued.
    EXPECT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    rig.AdvanceMs(1999U);
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    
    // Recovery reason should still be recorded for diagnostics.
    ASSERT_TRUE(rig.coordinator.Metrics().lastFailureReason.has_value());
    EXPECT_NE(rig.coordinator.Metrics().lastFailureReason->find("NonContiguousPhysicalIds"),
              std::string::npos);
}

TEST(BusResetCoordinatorTests, SelfIDTimeoutRequestsShortRecoveryResetAfterDeadline) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.StartResetCycle();
    rig.AdvanceMs(1000U);

    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_TRUE(rig.hardware.TestLastBusResetWasShort());
    EXPECT_TRUE(rig.publishedTopologies.empty());
    ASSERT_TRUE(rig.coordinator.Metrics().lastFailureReason.has_value());
    EXPECT_NE(rig.coordinator.Metrics().lastFailureReason->find("Self-ID timeout"),
              std::string::npos);
}

TEST(BusResetCoordinatorTests, ManualResetWithoutIrqTriggersOneBoundedRecoveryReset) {
    BusResetTestRig rig;
    rig.Initialize();

    rig.coordinator.RequestUserReset(true);
    rig.DrainReady();

    auto countBusResets = [&rig]() {
        const auto operations = rig.hardware.CopyTestOperations();
        return static_cast<size_t>(std::count(operations.begin(), operations.end(),
                                             HardwareInterface::TestOperation::InitiateBusReset));
    };

    EXPECT_EQ(countBusResets(), 1U);
    auto diag = rig.coordinator.Diagnostics();
    EXPECT_EQ(diag.manualResetEpoch, 1U);
    EXPECT_EQ(diag.resetEpoch, 0U);
    EXPECT_EQ(diag.softwareResetIssuedCount, 1U);

    rig.AdvanceMs(499U);
    EXPECT_EQ(countBusResets(), 1U);

    rig.AdvanceMs(1U);
    EXPECT_EQ(countBusResets(), 2U);
    diag = rig.coordinator.Diagnostics();
    EXPECT_EQ(diag.recoveryResetAttempts, 1U);
    EXPECT_EQ(diag.softwareResetIssuedCount, 2U);
    EXPECT_EQ(diag.lastRecoveryReasonCode,
              BusResetCoordinator::RecoveryReasonCode::ManualResetWatchdog);

    rig.AdvanceMs(1000U);
    EXPECT_EQ(countBusResets(), 2U);
}

TEST(BusResetCoordinatorTests, GapMismatchResetIsDeferredThenSentWithPhyConfig) {
    BusResetTestRig rig;
    rig.Initialize(true);
    rig.busManager.SetGapOptimizationEnabled(true);
    rig.SetLocalNode(1U);

    rig.StartResetCycle();
    const auto inconsistentCapture = MakeRawSelfIDCapture(
        14U, {MakeBaseSelfID(0U, 10U, true, false), MakeBaseSelfID(1U, 20U, true, true)});
    rig.PrimeCapture(inconsistentCapture, 14U);
    rig.TriggerStickyCompletion();

    EXPECT_FALSE(rig.hardware.TestBusResetIssued());

    rig.AdvanceMs(1U);
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    EXPECT_GT(rig.queue->PendingTaskCountForTesting(), 0U);

    rig.AdvanceMs(1999U);

    EXPECT_TRUE(rig.hardware.TestPhyConfigIssued());
    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_FALSE(rig.hardware.TestLastBusResetWasShort());
    EXPECT_EQ(rig.hardware.TestLastGapCount(), 63U);

    const auto operations = rig.hardware.CopyTestOperations();
    ASSERT_GE(operations.size(), 2U);
    EXPECT_EQ(operations[operations.size() - 2U], HardwareInterface::TestOperation::SendPhyConfig);
    EXPECT_EQ(operations.back(), HardwareInterface::TestOperation::InitiateBusReset);
}

// ---- §8.2.1: software resets must restate the current gap count -----------------
//
// "When gap_count has a value other than 63, bus resets initiated by software should
//  be immediately preceded by the transmission of a PHY configuration packet with a
//  nonzero T bit and gap_cnt equal to the current value of gap_count. Without this
//  precaution, the bus manager is almost certain to transmit a PHY configuration
//  packet to restore the optimal value of gap_count and then generate an additional
//  bus reset."
//
// The failure mode is therefore an *extra reset from a peer*, not a local error.
// Cross-validated with Linux core-card.c:252 (br_work sends
// FW_PHY_CONFIG_CURRENT_GAP_COUNT immediately before every scheduled reset).

namespace {

// Drive a full reset cycle that settles on a uniform gap count across two nodes,
// leaving the coordinator Idle with an accepted topology.
void SettleWithUniformGap(BusResetTestRig& rig, uint8_t generation, uint8_t gapCount) {
    rig.StartResetCycle();
    rig.PrimeCapture(MakeRawSelfIDCapture(generation,
                                          {MakeBaseSelfID(0U, gapCount, true, true),
                                           MakeBaseSelfID(1U, gapCount, true, false)}),
                     generation);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(2000U);
}

} // namespace

TEST(BusResetCoordinatorTests, BareSoftwareResetRestatesCurrentGapCount) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    SettleWithUniformGap(rig, 30U, 21U);
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    ASSERT_TRUE(rig.publishedTopologies.back().gapCountConsistent);
    ASSERT_EQ(rig.publishedTopologies.back().gapCount, 21U);

    rig.ResetHardwareState();
    BusResetCoordinatorTestPeer::RequestRecoveryReset(rig.coordinator, false);
    rig.AdvanceMs(2000U);

    ASSERT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_TRUE(rig.hardware.TestPhyConfigIssued())
        << "a bare software reset must restate gap_count (IEEE 1394-2008 §8.2.1)";
    EXPECT_EQ(rig.hardware.TestLastGapCount(), 21U);

    // Ordering matters: the PHY config is only meaningful immediately before the reset.
    const auto operations = rig.hardware.CopyTestOperations();
    ASSERT_GE(operations.size(), 2U);
    EXPECT_EQ(operations[operations.size() - 2U], HardwareInterface::TestOperation::SendPhyConfig);
    EXPECT_EQ(operations.back(), HardwareInterface::TestOperation::InitiateBusReset);
}

TEST(BusResetCoordinatorTests, BareSoftwareResetAtGap63SendsNoPhyConfig) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    SettleWithUniformGap(rig, 31U, 63U);
    ASSERT_EQ(rig.publishedTopologies.back().gapCount, 63U);

    rig.ResetHardwareState();
    BusResetCoordinatorTestPeer::RequestRecoveryReset(rig.coordinator, false);
    rig.AdvanceMs(2000U);

    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued())
        << "63 is the post-reset default; restating it would be pure bus noise";
}

TEST(BusResetCoordinatorTests, BareSoftwareResetWithInconsistentGapsSendsNoPhyConfig) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    rig.StartResetCycle();
    rig.PrimeCapture(MakeRawSelfIDCapture(32U, {MakeBaseSelfID(0U, 10U, true, true),
                                                MakeBaseSelfID(1U, 20U, true, false)}),
                     32U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(2000U);
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    ASSERT_FALSE(rig.publishedTopologies.back().gapCountConsistent);

    rig.ResetHardwareState();
    BusResetCoordinatorTestPeer::RequestRecoveryReset(rig.coordinator, false);
    rig.AdvanceMs(2000U);

    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued())
        << "nodes disagree, so there is no single current gap_count to restate";
}

TEST(BusResetCoordinatorTests, BareSoftwareResetWithoutAcceptedTopologySendsNoPhyConfig) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    // No reset cycle has completed, so nothing is known about the bus yet.
    BusResetCoordinatorTestPeer::RequestRecoveryReset(rig.coordinator, false);
    rig.AdvanceMs(2000U);

    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued());
}

TEST(BusResetCoordinatorTests, GapPreservingPhyConfigFailureAbortsTheReset) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    SettleWithUniformGap(rig, 33U, 21U);
    ASSERT_EQ(rig.publishedTopologies.back().gapCount, 21U);

    rig.ResetHardwareState();
    rig.SetSendPhyConfigResult(false);
    BusResetCoordinatorTestPeer::RequestRecoveryReset(rig.coordinator, false);
    rig.AdvanceMs(2000U);

    // Resetting after a failed PHY config would strand the bus at the wrong gap and
    // invite the very peer-initiated extra reset §8.2.1 warns about.
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
}

TEST(BusResetCoordinatorTests, CallerSuppliedPhyConfigOverridesPreservedGapCount) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    SettleWithUniformGap(rig, 34U, 21U);
    ASSERT_EQ(rig.publishedTopologies.back().gapCount, 21U);

    rig.ResetHardwareState();
    // An explicit gap-correction request must win over the preserved current value.
    BusResetCoordinatorTestPeer::RequestGapCorrectionReset(rig.coordinator, true, 30U);
    rig.AdvanceMs(2000U);

    ASSERT_TRUE(rig.hardware.TestPhyConfigIssued());
    EXPECT_EQ(rig.hardware.TestLastGapCount(), 30U);
    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
}

// ---- Config ROM re-stage resets are coordinator-owned ---------------------------
//
// ControllerCore::ApplyRolePolicy used to call HardwareInterface::InitiateBusReset
// directly, bypassing the §8.2.1 holdoff, the gap-preserving PHY config and the
// reset-origin attribution. Apple draws the same line we now do: the raw link path
// (IOFireWireUserClient::busReset -> getLink()->resetBus()) is reachable only behind
// the debug-only "unsafe bus resets" property.

TEST(BusResetCoordinatorTests, ConfigRomRestageResetIsLongAndDistinctlyLabelled) {
    BusResetCoordinator coordinator;

    coordinator.RequestConfigRomRestageReset("Role policy BIB re-stage");

    ASSERT_TRUE(BusResetCoordinatorTestPeer::HasPendingSoftwareReset(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsLong(coordinator))
        << "peers must re-read the ROM; a short reset may not force re-enumeration";
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsRolePolicyRestage(coordinator))
        << "the kind is the only reset-origin attribution a trace carries";
}

TEST(BusResetCoordinatorTests, ConfigRomRestageResetObservesRepeatedResetHoldoff) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    // Stay inside the holdoff: it is armed at Self-ID completion, so the request has
    // to be made before the window elapses for the deferral to be exercised at all.
    rig.StartResetCycle();
    rig.PrimeCapture(MakeRawSelfIDCapture(40U, {MakeBaseSelfID(0U, 63U, true, true),
                                                MakeBaseSelfID(1U, 63U, true, false)}),
                     40U);
    rig.TriggerStickyCompletion();
    ASSERT_FALSE(rig.hardware.TestBusResetIssued());

    rig.coordinator.RequestConfigRomRestageReset("Role policy BIB re-stage");
    rig.AdvanceMs(1U);
    EXPECT_FALSE(rig.hardware.TestBusResetIssued())
        << "must not fire inside the 2 s window (IEEE 1394-2008 §8.2.1)";

    rig.AdvanceMs(1999U);
    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
    EXPECT_FALSE(rig.hardware.TestLastBusResetWasShort());
}

TEST(BusResetCoordinatorTests, ConfigRomRestageResetRestatesCurrentGapCount) {
    BusResetTestRig rig;
    rig.Initialize();
    rig.SetLocalNode(0U);

    SettleWithUniformGap(rig, 41U, 21U);
    ASSERT_EQ(rig.publishedTopologies.back().gapCount, 21U);
    rig.ResetHardwareState();

    rig.coordinator.RequestConfigRomRestageReset("Role policy BIB re-stage");
    rig.AdvanceMs(2000U);

    // Inherits the §8.2.1 gap-preservation the direct-to-PHY path never had.
    ASSERT_TRUE(rig.hardware.TestPhyConfigIssued());
    EXPECT_EQ(rig.hardware.TestLastGapCount(), 21U);
    EXPECT_TRUE(rig.hardware.TestBusResetIssued());
}

TEST(BusResetCoordinatorTests, ConfigRomRestageDoesNotOutrankBusWidePolicyResets) {
    // A restage is local housekeeping. If it coalesces with a request carrying
    // bus-wide policy, the policy kind must survive the merge — otherwise the trace
    // attributes a gap correction or delegation to a ROM re-stage.
    {
        BusResetCoordinator coordinator;
        coordinator.RequestConfigRomRestageReset("restage");
        BusResetCoordinatorTestPeer::RequestGapCorrectionReset(coordinator, true, 21U);
        EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsGapCorrection(coordinator));
    }
    {
        BusResetCoordinator coordinator;
        coordinator.RequestConfigRomRestageReset("restage");
        BusResetCoordinatorTestPeer::RequestDelegationReset(coordinator, true, 2U);
        EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsDelegation(coordinator));
    }
}

TEST(BusResetCoordinatorTests, ConfigRomRestageOutranksPlainRecovery) {
    // Against an unattributed Recovery, the more specific cause wins so the trace
    // still says why the reset happened.
    BusResetCoordinator coordinator;

    BusResetCoordinatorTestPeer::RequestRecoveryReset(coordinator, false);
    coordinator.RequestConfigRomRestageReset("restage");

    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsRolePolicyRestage(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsLong(coordinator));
}

TEST(BusResetCoordinatorTests, EarlyTopologyPolicyDoesNotDelegateBeforeEvidence) {
    BusResetTestRig rig;
    rig.Initialize(true);
    rig.busManager.SetGapOptimizationEnabled(true);
    rig.SetLocalNode(1U);

    rig.StartResetCycle();
    const auto capture = MakeRawSelfIDCapture(
        15U, {MakeBaseSelfID(0U, 10U, true, true), MakeBaseSelfID(1U, 20U, true, false)});
    rig.PrimeCapture(capture, 15U);
    rig.TriggerStickyCompletion();

    EXPECT_FALSE(rig.hardware.TestBusResetIssued());

    rig.AdvanceMs(2000U);

    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued());
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
}

TEST(BusResetCoordinatorTests, LocalIRMRemoteRootWithoutPeerContenderForcesLocalRoot) {
    BusResetTestRig rig;
    rig.Initialize(true);
    rig.SetLocalNode(0U);

    rig.StartResetCycle();
    const auto capture = MakeRawSelfIDCapture(
        16U, {MakeBaseSelfID(0U, 63U, true, true),
              MakeBaseSelfID(1U, 63U, false, false),
              MakeBaseSelfID(2U, 63U, true, false)});
    rig.PrimeCapture(capture, 16U);
    rig.TriggerStickyCompletion();

    EXPECT_FALSE(rig.hardware.TestBusResetIssued());

    rig.AdvanceMs(2000U);

    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued());
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_EQ(rig.publishedTopologies.back().rootNodeId, 2U);
}

TEST(BusResetCoordinatorTests, ConsumedSelfIDComplete2IsClearedExplicitly) {
    BusResetCoordinator coordinator;
    HardwareInterface hardware;

    BusResetCoordinatorTestPeer::Attach(coordinator, hardware);
    BusResetCoordinatorTestPeer::SetSelfIDLatch(coordinator, true, true);

    BusResetCoordinatorTestPeer::ClearConsumedSelfIDInterrupts(coordinator);

    EXPECT_EQ(hardware.GetTestRegister(Register32::kIntEventClear),
              IntEventBits::kSelfIDComplete | IntEventBits::kSelfIDComplete2);
}

TEST(BusResetCoordinatorTests, MultipleDeferredResetRequestsAreCoalescedLongWins) {
    BusResetCoordinator coordinator;

    BusResetCoordinatorTestPeer::RequestRecoveryReset(coordinator, false);
    BusResetCoordinatorTestPeer::RequestGapCorrectionReset(coordinator, true, 21U);

    ASSERT_TRUE(BusResetCoordinatorTestPeer::HasPendingSoftwareReset(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsLong(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsGapCorrection(coordinator));
    EXPECT_EQ(BusResetCoordinatorTestPeer::PendingGapCount(coordinator), 21U);
}

TEST(BusResetCoordinatorTests, DeferredResetMergePreservesRootTargetAndGapConfig) {
    BusResetCoordinator coordinator;

    BusResetCoordinatorTestPeer::RequestDelegationReset(coordinator, true, 2U);
    BusResetCoordinatorTestPeer::RequestGapCorrectionReset(coordinator, true, 21U);

    ASSERT_TRUE(BusResetCoordinatorTestPeer::HasPendingSoftwareReset(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsLong(coordinator));
    EXPECT_EQ(BusResetCoordinatorTestPeer::PendingForceRootNode(coordinator), 2U);
    EXPECT_EQ(BusResetCoordinatorTestPeer::PendingGapCount(coordinator), 21U);
}

TEST(BusResetCoordinatorTests, ConservativeMismatchGapOverridesDeferredTargetGap) {
    BusResetCoordinator coordinator;

    BusResetCoordinatorTestPeer::RequestGapCorrectionReset(coordinator, true, 21U);
    BusResetCoordinatorTestPeer::RequestMismatchForce63Reset(coordinator, true);

    ASSERT_TRUE(BusResetCoordinatorTestPeer::HasPendingSoftwareReset(coordinator));
    EXPECT_EQ(BusResetCoordinatorTestPeer::PendingGapCount(coordinator), 63U);
}

TEST(BusResetCoordinatorTests, TargetGapOptimizationDoesNotRunBeforeRoleEvidence) {
    BusResetTestRig rig;
    rig.Initialize(true);
    rig.busManager.SetGapOptimizationEnabled(true);
    rig.busManager.SetForcedGapCount(21U);
    rig.SetLocalNode(1U);
    rig.SetSendPhyConfigResult(false);

    rig.StartResetCycle();
    const auto capture =
        MakeRawSelfIDCapture(17U, {MakeBaseSelfID(0U, 63U, true, false),
                                   MakeBaseSelfID(1U, 63U, true, true)});
    rig.PrimeCapture(capture, 17U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(2000U);

    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued());
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_TRUE(rig.publishedTopologies.back().gapCountConsistent);
    EXPECT_EQ(rig.publishedTopologies.back().gapCount, 63U);
}

TEST(BusResetCoordinatorTests, FailedResetInitiationDoesNotApplyToSkippedTargetGap) {
    BusResetTestRig rig;
    rig.Initialize(true);
    rig.busManager.SetGapOptimizationEnabled(true);
    rig.busManager.SetForcedGapCount(21U);
    rig.SetLocalNode(1U);
    rig.SetInitiateBusResetResult(false);

    rig.StartResetCycle();
    rig.PrimeCapture(MakeRawSelfIDCapture(19U, {MakeBaseSelfID(0U, 63U, true, false),
                                                MakeBaseSelfID(1U, 63U, true, true)}),
                     19U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(2000U);

    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued());
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_EQ(rig.publishedTopologies.back().gapCount, 63U);
}

TEST(BusResetCoordinatorTests, StableAcceptedGenerationDoesNotCommitSkippedTargetGap) {
    BusResetTestRig rig;
    rig.Initialize(true);
    rig.busManager.SetGapOptimizationEnabled(true);
    rig.busManager.SetForcedGapCount(21U);
    rig.SetLocalNode(1U);

    rig.StartResetCycle();
    rig.PrimeCapture(MakeRawSelfIDCapture(21U, {MakeBaseSelfID(0U, 63U, true, false),
                                                MakeBaseSelfID(1U, 63U, true, true)}),
                     21U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(2000U);

    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    ASSERT_EQ(rig.publishedTopologies.size(), 1U);
    EXPECT_EQ(rig.publishedTopologies.back().gapCount, 63U);

    rig.ResetHardwareState();
    rig.SetLocalNode(1U);

    rig.StartResetCycle();
    rig.PrimeCapture(MakeRawSelfIDCapture(22U, {MakeBaseSelfID(0U, 21U, true, false),
                                                MakeBaseSelfID(1U, 21U, true, true)}),
                     22U);
    rig.TriggerStickyCompletion();
    rig.AdvanceMs(100U);

    EXPECT_FALSE(rig.hardware.TestPhyConfigIssued());
    EXPECT_FALSE(rig.hardware.TestBusResetIssued());
    ASSERT_EQ(rig.publishedTopologies.size(), 2U);
    ASSERT_TRUE(rig.publishedTopologies.back().gapCountConsistent);
    EXPECT_EQ(rig.publishedTopologies.back().gapCount, 21U);
}

// FW-9/FW-10: local cycleMaster is role-policy controlled, never reset-rearm controlled.
TEST(BusResetCoordinatorCycleMasterGuard, NeverReassertsFromResetRearm) {
    using BRC = ASFW::Driver::BusResetCoordinator;
    EXPECT_FALSE(BRC::ShouldReassertCycleMasterOnRearm(/*wasRoot=*/false, /*isRootNow=*/false));
    EXPECT_FALSE(BRC::ShouldReassertCycleMasterOnRearm(/*wasRoot=*/false, /*isRootNow=*/true));
    EXPECT_FALSE(BRC::ShouldReassertCycleMasterOnRearm(/*wasRoot=*/true, /*isRootNow=*/false));
    EXPECT_FALSE(BRC::ShouldReassertCycleMasterOnRearm(/*wasRoot=*/true, /*isRootNow=*/true));
}
