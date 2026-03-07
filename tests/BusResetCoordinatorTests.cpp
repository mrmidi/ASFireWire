#include <gtest/gtest.h>

#include "ASFWDriver/Bus/BusResetCoordinator.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/InterruptManager.hpp"
#include "ASFWDriver/Hardware/RegisterMap.hpp"

namespace ASFW::Driver {

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

    static bool StickyCompleteLatched(const BusResetCoordinator& coordinator) {
        return coordinator.selfIdLatch_.stickyComplete;
    }

    static bool CanAttemptSelfIDDecode(const BusResetCoordinator& coordinator) {
        return coordinator.CanAttemptSelfIDDecode();
    }

    static void ClearConsumedSelfIDInterrupts(BusResetCoordinator& coordinator) {
        coordinator.ClearConsumedSelfIDInterrupts();
    }

    static void BeginNewResetCycle(BusResetCoordinator& coordinator) {
        coordinator.BeginNewResetCycle();
    }

    static void SetSoftwareResetBlockedUntil(BusResetCoordinator& coordinator, uint64_t whenNs) {
        coordinator.resetTiming_.softwareResetBlockedUntilNs = whenNs;
    }

    static uint64_t NowNs() { return BusResetCoordinator::MonotonicNow(); }

    static void RequestRecoveryReset(BusResetCoordinator& coordinator,
                                     bool longReset = false) {
        coordinator.RequestSoftwareReset(
            {BusResetCoordinator::ResetRequestKind::Recovery,
             longReset ? BusResetCoordinator::ResetFlavor::Long
                       : BusResetCoordinator::ResetFlavor::Short,
             std::nullopt, "Recovery"});
    }

    static void RequestGapCorrectionReset(BusResetCoordinator& coordinator, bool longReset,
                                          std::optional<uint8_t> gapCount,
                                          std::optional<uint8_t> forceRootNode) {
        BusManager::PhyConfigCommand command{};
        command.gapCount = gapCount;
        command.forceRootNodeID = forceRootNode;
        coordinator.RequestSoftwareReset(
            {BusResetCoordinator::ResetRequestKind::GapCorrection,
             longReset ? BusResetCoordinator::ResetFlavor::Long
                       : BusResetCoordinator::ResetFlavor::Short,
             command, "GapCorrection"});
    }

    static bool MaybeDispatchPendingSoftwareReset(BusResetCoordinator& coordinator) {
        return coordinator.MaybeDispatchPendingSoftwareReset();
    }

    static bool HasPendingSoftwareReset(const BusResetCoordinator& coordinator) {
        return coordinator.pendingSoftwareReset_.has_value();
    }

    static bool PendingResetIsLong(const BusResetCoordinator& coordinator) {
        return coordinator.pendingSoftwareReset_.has_value() &&
               coordinator.pendingSoftwareReset_->flavor ==
                   BusResetCoordinator::ResetFlavor::Long;
    }

    static bool PendingResetIsGapCorrection(const BusResetCoordinator& coordinator) {
        return coordinator.pendingSoftwareReset_.has_value() &&
               coordinator.pendingSoftwareReset_->kind ==
                   BusResetCoordinator::ResetRequestKind::GapCorrection;
    }

    static std::optional<uint8_t> PendingGapCount(const BusResetCoordinator& coordinator) {
        if (!coordinator.pendingSoftwareReset_.has_value() ||
            !coordinator.pendingSoftwareReset_->phyConfig.has_value()) {
            return std::nullopt;
        }
        return coordinator.pendingSoftwareReset_->phyConfig->gapCount;
    }
};

} // namespace ASFW::Driver

using namespace ASFW::Driver;

namespace {

constexpr uint32_t kValidNodeId = 0x80000000U;

} // namespace

TEST(BusResetCoordinatorTests, SelfIDComplete2OnlyNodeValidStillAttemptsDecode) {
    BusResetCoordinator coordinator;
    HardwareInterface hardware;

    BusResetCoordinatorTestPeer::Attach(coordinator, hardware);
    hardware.SetTestRegister(Register32::kNodeID, kValidNodeId);
    BusResetCoordinatorTestPeer::SetSelfIDLatch(coordinator, false, true);

    EXPECT_TRUE(BusResetCoordinatorTestPeer::CanAttemptSelfIDDecode(coordinator));
}

TEST(BusResetCoordinatorTests, ConsumedSelfIDComplete2IsExplicitlyClearedByCoordinator) {
    BusResetCoordinator coordinator;
    HardwareInterface hardware;

    BusResetCoordinatorTestPeer::Attach(coordinator, hardware);
    BusResetCoordinatorTestPeer::SetSelfIDLatch(coordinator, true, true);

    BusResetCoordinatorTestPeer::ClearConsumedSelfIDInterrupts(coordinator);

    EXPECT_EQ(hardware.GetTestRegister(Register32::kIntEventClear),
              IntEventBits::kSelfIDComplete | IntEventBits::kSelfIDComplete2);
    EXPECT_FALSE(BusResetCoordinatorTestPeer::StickyCompleteLatched(coordinator));
}

TEST(BusResetCoordinatorTests, StaleSelfIDComplete2OnNewResetIsClearedBeforeWaitCycle) {
    BusResetCoordinator coordinator;
    HardwareInterface hardware;
    InterruptManager interrupts;

    BusResetCoordinatorTestPeer::Attach(coordinator, hardware, &interrupts);
    BusResetCoordinatorTestPeer::SetSelfIDLatch(coordinator, false, true);

    BusResetCoordinatorTestPeer::BeginNewResetCycle(coordinator);

    EXPECT_EQ(coordinator.GetState(), BusResetCoordinator::State::Detecting);
    EXPECT_FALSE(BusResetCoordinatorTestPeer::StickyCompleteLatched(coordinator));
    EXPECT_EQ(hardware.GetTestRegister(Register32::kIntEventClear),
              IntEventBits::kSelfIDComplete2);
    EXPECT_EQ(hardware.GetTestRegister(Register32::kIntMaskClear), IntEventBits::kBusReset);
}

TEST(BusResetCoordinatorTests, RepeatedSoftwareResetWithinTwoSecondsIsDeferred) {
    BusResetCoordinator coordinator;
    HardwareInterface hardware;

    BusResetCoordinatorTestPeer::Attach(coordinator, hardware);
    BusResetCoordinatorTestPeer::RequestRecoveryReset(coordinator);
    BusResetCoordinatorTestPeer::SetSoftwareResetBlockedUntil(
        coordinator, BusResetCoordinatorTestPeer::NowNs() + 50'000'000ULL);

    EXPECT_TRUE(BusResetCoordinatorTestPeer::MaybeDispatchPendingSoftwareReset(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::HasPendingSoftwareReset(coordinator));
    EXPECT_FALSE(hardware.TestBusResetIssued());
}

TEST(BusResetCoordinatorTests, MultipleDeferredResetRequestsAreCoalescedLongWins) {
    BusResetCoordinator coordinator;

    BusResetCoordinatorTestPeer::RequestRecoveryReset(coordinator, false);
    BusResetCoordinatorTestPeer::RequestGapCorrectionReset(coordinator, true, 21U, std::nullopt);

    ASSERT_TRUE(BusResetCoordinatorTestPeer::HasPendingSoftwareReset(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsLong(coordinator));
    EXPECT_TRUE(BusResetCoordinatorTestPeer::PendingResetIsGapCorrection(coordinator));
    EXPECT_EQ(BusResetCoordinatorTestPeer::PendingGapCount(coordinator), 21U);
}

TEST(BusResetCoordinatorTests, DeferredGapCorrectionResetSendsPhyConfigBeforeReset) {
    BusResetCoordinator coordinator;
    HardwareInterface hardware;

    BusResetCoordinatorTestPeer::Attach(coordinator, hardware);
    BusResetCoordinatorTestPeer::RequestGapCorrectionReset(coordinator, true, 12U, 3U);

    ASSERT_TRUE(BusResetCoordinatorTestPeer::MaybeDispatchPendingSoftwareReset(coordinator));
    EXPECT_TRUE(hardware.TestPhyConfigIssued());
    EXPECT_TRUE(hardware.TestBusResetIssued());
    EXPECT_FALSE(hardware.TestLastBusResetWasShort());
    EXPECT_EQ(hardware.TestLastGapCount(), 12U);
    EXPECT_EQ(hardware.TestLastForceRootNode(), 3U);

    const auto operations = hardware.CopyTestOperations();
    ASSERT_GE(operations.size(), 2U);
    EXPECT_EQ(operations[operations.size() - 2U], HardwareInterface::TestOperation::SendPhyConfig);
    EXPECT_EQ(operations.back(), HardwareInterface::TestOperation::InitiateBusReset);
}
