// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "../../ASFWDriver/Audio/Protocols/AVCStartReadiness.hpp"

namespace {

TEST(AVCStartReadiness, RequiresPublishedTransportForOperationalNode) {
    EXPECT_FALSE(ASFW::Audio::HasReadyAVCStartRoute(2, false));
    EXPECT_TRUE(ASFW::Audio::HasReadyAVCStartRoute(2, true));
}

TEST(AVCStartReadiness, RejectsResetAndOutOfRangeNodeValues) {
    EXPECT_FALSE(ASFW::Audio::HasReadyAVCStartRoute(0xFFFFU, true));
    EXPECT_FALSE(ASFW::Audio::HasReadyAVCStartRoute(64U, true));
}

} // namespace
