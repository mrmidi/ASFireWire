#include <gtest/gtest.h>

#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/RegisterMap.hpp"

using namespace ASFW::Driver;

// OHCI PhysicalRequestFilter: one bit per node, Lo covers nodes 0-31, Hi 32-62
// (Linux ohci.c:2624-2628).
TEST(PhysicalRequestFilterTests, EnableSetsNodeBitInLoOrHiRegister) {
    HardwareInterface hardware;
    hardware.SetPhysicalRequestFilter(5, true);
    EXPECT_EQ(1u << 5, hardware.GetTestRegister(Register32::kPhyReqFilterLoSet));
    hardware.SetPhysicalRequestFilter(40, true);
    EXPECT_EQ(1u << 8, hardware.GetTestRegister(Register32::kPhyReqFilterHiSet));
}

TEST(PhysicalRequestFilterTests, DisableWritesClearRegister) {
    HardwareInterface hardware;
    hardware.SetPhysicalRequestFilter(5, false);
    EXPECT_EQ(1u << 5, hardware.GetTestRegister(Register32::kPhyReqFilterLoClear));
    EXPECT_EQ(0u, hardware.GetTestRegister(Register32::kPhyReqFilterLoSet));
    hardware.SetPhysicalRequestFilter(62, false);
    EXPECT_EQ(1u << 30, hardware.GetTestRegister(Register32::kPhyReqFilterHiClear));
}
