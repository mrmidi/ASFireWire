#include <gtest/gtest.h>
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/OHCIConstants.hpp"

using namespace ASFW::Driver;

TEST(PhysicalRequestFilterTests, PerNodeLeasesSurviveAnotherLunLogout) {
    HardwareInterface hardware;
    hardware.SetTestRegister(Register32::kSelfIDCount, 7u << 16);
    for (const uint16_t node : {5, 31, 32, 40, 62}) {
        const auto reg = node < 32 ? Register32::kPhyReqFilterLoSet : Register32::kPhyReqFilterHiSet;
        const uint32_t bit = 1u << (node % 32);
        ASSERT_TRUE(hardware.ControlPhysicalRequests(node, 7, PhysicalRequestAccess::Acquire));
        ASSERT_TRUE(hardware.ControlPhysicalRequests(node, 7, PhysicalRequestAccess::Acquire));
        EXPECT_NE(0u, hardware.GetTestRegister(reg) & bit);
        EXPECT_EQ(kPhysicalUpperBound, hardware.GetTestRegister(Register32::kPhyUpperBound));
        ASSERT_TRUE(hardware.ControlPhysicalRequests(node, 7, PhysicalRequestAccess::Release));
        EXPECT_TRUE(hardware.ControlPhysicalRequests(node, 7, PhysicalRequestAccess::Check));
        ASSERT_TRUE(hardware.ControlPhysicalRequests(node, 7, PhysicalRequestAccess::Release));
        EXPECT_EQ(0u, hardware.GetTestRegister(reg) & bit);
    }
}

TEST(PhysicalRequestFilterTests, RejectsStaleResetBroadcastAndRemoteBus) {
    HardwareInterface hardware;
    hardware.SetTestRegister(Register32::kSelfIDCount, 7u << 16);
    EXPECT_FALSE(hardware.ControlPhysicalRequests(5, 6, PhysicalRequestAccess::Acquire));
    for (const uint16_t node : {63, 0xFFFF, 0x0045}) {
        EXPECT_FALSE(hardware.ControlPhysicalRequests(node, 7, PhysicalRequestAccess::Acquire));
    }
    hardware.SetTestRegister(Register32::kIntEvent, IntEventBits::kBusReset);
    EXPECT_FALSE(hardware.ControlPhysicalRequests(5, 7, PhysicalRequestAccess::Acquire));
    EXPECT_EQ(0u, hardware.GetTestRegister(Register32::kPhyReqFilterHiSet));
    EXPECT_EQ(0u, hardware.GetTestRegister(Register32::kPhyReqFilterLoSet));
}

TEST(PhysicalRequestFilterTests, RevokeFencesAllLunsUntilNextGeneration) {
    HardwareInterface hardware;
    hardware.SetTestRegister(Register32::kSelfIDCount, 7u << 16);
    ASSERT_TRUE(hardware.ControlPhysicalRequests(5, 7, PhysicalRequestAccess::Acquire));
    ASSERT_TRUE(hardware.ControlPhysicalRequests(5, 7, PhysicalRequestAccess::Acquire));
    ASSERT_TRUE(hardware.ControlPhysicalRequests(5, 7, PhysicalRequestAccess::Revoke));
    EXPECT_FALSE(hardware.ControlPhysicalRequests(5, 7, PhysicalRequestAccess::Check));
    EXPECT_FALSE(hardware.ControlPhysicalRequests(5, 7, PhysicalRequestAccess::Acquire));
    hardware.ClearPhysicalRequestFilters();
    hardware.SetTestRegister(Register32::kSelfIDCount, 8u << 16);
    ASSERT_TRUE(hardware.ControlPhysicalRequests(0xFFC5, 8, PhysicalRequestAccess::Acquire));
    EXPECT_TRUE(hardware.ControlPhysicalRequests(5, 7, PhysicalRequestAccess::Release));
    EXPECT_TRUE(hardware.ControlPhysicalRequests(5, 8, PhysicalRequestAccess::Check));
    hardware.Detach();
    EXPECT_FALSE(hardware.ControlPhysicalRequests(5, 8, PhysicalRequestAccess::Acquire));
}
