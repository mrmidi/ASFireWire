#include <gtest/gtest.h>

#include "ASFWDriver/Async/Contexts/ContextBase.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/OHCIConstants.hpp"

using namespace ASFW::Async;
using namespace ASFW::Driver;

namespace {

struct TestContext : ContextBase<TestContext, ATRequestTag> {};

class ContextBaseTest : public ::testing::Test {
  protected:
    void SetUp() override { ASSERT_EQ(ctx_.Initialize(hardware_), kIOReturnSuccess); }

    void SetControl(uint32_t value) { hardware_.SetTestRegister(ATRequestTag::kControlSetReg, value); }

    HardwareInterface hardware_;
    TestContext ctx_;
};

} // namespace

// Regression (2026-09-24): IsActive() masked a local `1u << 13` (reserved) and
// always answered false, so every stop/quiesce wait returned instantly while
// hardware was still ACTIVE. ACTIVE is bit 10 (OHCI §3.1.1).
TEST_F(ContextBaseTest, IsActiveReadsBit10) {
    SetControl(kContextControlActiveBit);
    EXPECT_TRUE(ctx_.IsActive());

    SetControl(1u << 13);
    EXPECT_FALSE(ctx_.IsActive());

    SetControl(kContextControlRunBit);
    EXPECT_FALSE(ctx_.IsActive());
}

TEST_F(ContextBaseTest, IsRunningReadsBit15) {
    SetControl(kContextControlRunBit);
    EXPECT_TRUE(ctx_.IsRunning());

    SetControl(kContextControlActiveBit);
    EXPECT_FALSE(ctx_.IsRunning());
}

TEST_F(ContextBaseTest, AllOnesReadIsNeitherActiveNorRunning) {
    SetControl(0xFFFFFFFFu);
    EXPECT_FALSE(ctx_.IsActive());
    EXPECT_FALSE(ctx_.IsRunning());
}
