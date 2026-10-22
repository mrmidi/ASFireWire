#include <gtest/gtest.h>

#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/RegisterMap.hpp"

using namespace ASFW::Driver;

TEST(PhysicalRequestFilterTests, EnableSetsTheNodeBitForTheCurrentGeneration) {
    HardwareInterface hardware;
    hardware.SetTestRegister(Register32::kSelfIDCount, 7u << 16);
    for (const uint16_t node : {uint16_t{5}, uint16_t{31}, uint16_t{32}, uint16_t{0xFFC0 | 40}, uint16_t{62}}) {
        ASSERT_TRUE(hardware.EnablePhysicalRequests(node, 7).has_value()) << node;
        const uint16_t phy = node & 0x3Fu;
        const auto reg = phy < 32 ? Register32::kPhyReqFilterLoSet : Register32::kPhyReqFilterHiSet;
        EXPECT_NE(0u, hardware.GetTestRegister(reg) & (1u << (phy % 32))) << node;
    }
    hardware.ClearPhysicalRequestFilters();
    EXPECT_EQ(0u, hardware.GetTestRegister(Register32::kPhyReqFilterLoSet));
    EXPECT_EQ(0u, hardware.GetTestRegister(Register32::kPhyReqFilterHiSet));
}

TEST(PhysicalRequestFilterTests, RejectsStaleGenerationPendingResetBroadcastAndRemoteBus) {
    HardwareInterface hardware;
    hardware.SetTestRegister(Register32::kSelfIDCount, 7u << 16);
    EXPECT_EQ(PhysicalDmaError::StaleGeneration, hardware.EnablePhysicalRequests(5, 6).error());
    EXPECT_EQ(PhysicalDmaError::InvalidNode, hardware.EnablePhysicalRequests(63, 7).error());
    EXPECT_EQ(PhysicalDmaError::InvalidNode, hardware.EnablePhysicalRequests(0xFFFF, 7).error());
    EXPECT_EQ(PhysicalDmaError::InvalidNode, hardware.EnablePhysicalRequests(0x0045, 7).error());
    hardware.SetTestRegister(Register32::kIntEvent, IntEventBits::kBusReset);
    EXPECT_EQ(PhysicalDmaError::StaleGeneration, hardware.EnablePhysicalRequests(5, 7).error());
    EXPECT_EQ(0u, hardware.GetTestRegister(Register32::kPhyReqFilterLoSet));
    EXPECT_EQ(0u, hardware.GetTestRegister(Register32::kPhyReqFilterHiSet));
    hardware.Detach();
    EXPECT_EQ(PhysicalDmaError::HardwareGone, hardware.EnablePhysicalRequests(5, 7).error());
}
