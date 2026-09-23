#include <gtest/gtest.h>
#include "ASFWDriver/Audio/Protocols/BeBoB/MAudioSpecialStartPolicy.hpp"

namespace {
using namespace ASFW::Audio::BeBoB;

TEST(MAudioSpecialStartPolicy, StartsHostTransmitBeforeReceive) {
    ASSERT_EQ(kMAudioHostStartOrder.size(), 2U);
    EXPECT_EQ(kMAudioHostStartOrder[0], MAudioHostStartDirection::Transmit);
    EXPECT_EQ(kMAudioHostStartOrder[1], MAudioHostStartDirection::Receive);
}

TEST(MAudioSpecialStartPolicy, ReassertsOutputBeforeInputWithTheRequiredGap) {
    ASSERT_EQ(kMAudioPostStartPlan.size(), 2U);
    EXPECT_EQ(kMAudioPostStartPlan[0].action,
              MAudioPostStartAction::SetOutputSignalFormat);
    EXPECT_EQ(kMAudioPostStartPlan[0].delayBeforeMs, 0U);
    EXPECT_EQ(kMAudioPostStartPlan[1].action,
              MAudioPostStartAction::SetInputSignalFormat);
    EXPECT_EQ(kMAudioPostStartPlan[1].delayBeforeMs,
              kMAudioPostStartInputDelayMs);
    EXPECT_EQ(kMAudioPostStartInputDelayMs, 100U);
}

TEST(MAudioSpecialStartPolicy, RetainsLinuxStreamReadinessTimeout) {
    EXPECT_EQ(kMAudioStreamReadyTimeoutMs, 4000U);
}
} // namespace
