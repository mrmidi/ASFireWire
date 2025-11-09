#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "InterruptManager.hpp"
#include "RegisterMap.hpp"

using namespace ASFW::Driver;

// ───────────────────────────────────────────────
// Test fixture
// ───────────────────────────────────────────────
class InterruptMaskShadowTests : public ::testing::Test {
protected:
    InterruptManager mgr;
};

// ───────────────────────────────────────────────
// 1. Verify EnableInterrupts sets bits in shadow
// ───────────────────────────────────────────────
TEST_F(InterruptMaskShadowTests, EnableInterruptsSetsBitsInShadow) {
    constexpr uint32_t bits = IntEventBits::kSelfIDComplete | IntEventBits::kSelfIDComplete2;

    mgr.EnableInterrupts(bits);

    // Expect shadow updated
    EXPECT_TRUE((mgr.EnabledMask() & bits) == bits);
}

// ───────────────────────────────────────────────
// 2. Verify DisableInterrupts clears bits in shadow
// ───────────────────────────────────────────────
TEST_F(InterruptMaskShadowTests, DisableInterruptsClearsBitsInShadow) {
    constexpr uint32_t bits = IntEventBits::kSelfIDComplete | IntEventBits::kSelfIDComplete2;

    // Enable first
    mgr.EnableInterrupts(bits);
    ASSERT_TRUE((mgr.EnabledMask() & bits) == bits);

    // Then disable them
    mgr.DisableInterrupts(bits);

    EXPECT_EQ(mgr.EnabledMask() & bits, 0u);
}

// ───────────────────────────────────────────────
// 3. Verify ControllerCore-style init sequence
//    keeps shadow in sync
// ───────────────────────────────────────────────
TEST_F(InterruptMaskShadowTests, InitSequenceSyncsShadowProperly) {
    constexpr uint32_t initMask = IntEventBits::kBusReset |
                                  IntEventBits::kSelfIDComplete |
                                  IntEventBits::kSelfIDComplete2;

    // mimic ControllerCore::InitialiseHardware path
    mgr.EnableInterrupts(initMask);

    EXPECT_TRUE((mgr.EnabledMask() & initMask) == initMask);

    // Now simulate BusResetCoordinator masking busReset
    mgr.DisableInterrupts(IntEventBits::kBusReset);
    uint32_t current = mgr.EnabledMask();
    EXPECT_FALSE(current & IntEventBits::kBusReset);
    EXPECT_TRUE(current & IntEventBits::kSelfIDComplete);
    EXPECT_TRUE(current & IntEventBits::kSelfIDComplete2);
}

// ───────────────────────────────────────────────
// 4. Verify shadow survives multiple enable/disable cycles
// ───────────────────────────────────────────────
TEST_F(InterruptMaskShadowTests, ShadowSurvivesMultipleCycles) {
    // Enable some bits
    mgr.EnableInterrupts(IntEventBits::kBusReset | IntEventBits::kSelfIDComplete);
    EXPECT_TRUE((mgr.EnabledMask() & IntEventBits::kBusReset) != 0);
    EXPECT_TRUE((mgr.EnabledMask() & IntEventBits::kSelfIDComplete) != 0);
    
    // Enable more bits (should not affect previous)
    mgr.EnableInterrupts(IntEventBits::kSelfIDComplete2);
    EXPECT_TRUE((mgr.EnabledMask() & IntEventBits::kBusReset) != 0);
    EXPECT_TRUE((mgr.EnabledMask() & IntEventBits::kSelfIDComplete) != 0);
    EXPECT_TRUE((mgr.EnabledMask() & IntEventBits::kSelfIDComplete2) != 0);
    
    // Disable one (should not affect others)
    mgr.DisableInterrupts(IntEventBits::kBusReset);
    EXPECT_FALSE((mgr.EnabledMask() & IntEventBits::kBusReset) != 0);
    EXPECT_TRUE((mgr.EnabledMask() & IntEventBits::kSelfIDComplete) != 0);
    EXPECT_TRUE((mgr.EnabledMask() & IntEventBits::kSelfIDComplete2) != 0);
}
