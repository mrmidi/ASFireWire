// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// BroadcastChannelCSRTests.cpp — Unit tests for BroadcastChannelCSR.

#include "Bus/CSR/BroadcastChannelCSR.hpp"
#include <gtest/gtest.h>

using namespace ASFW::Bus;

TEST(BroadcastChannelCSRTests, InitialValue_IsImplementedInvalidChannel31) {
    BroadcastChannelCSR bc;
    EXPECT_EQ(bc.Read(), 0x8000001F);
}

TEST(BroadcastChannelCSRTests, MarkValid_SetsCorrectBits) {
    BroadcastChannelCSR bc;
    bc.MarkValidChannel31();
    EXPECT_EQ(bc.Read(), 0xC000001F);
}

TEST(BroadcastChannelCSRTests, Reset_ReturnsToInitialState) {
    BroadcastChannelCSR bc;
    bc.MarkValidChannel31();
    bc.ResetImplementedInvalid();
    EXPECT_EQ(bc.Read(), 0x8000001F);
}

TEST(BroadcastChannelCSRTests, Write_SanitizesReservedAndForceImplemented) {
    BroadcastChannelCSR bc;
    
    // Attempt to write 0: should become 0x8000001F (implemented + channel 31)
    bc.Write(0);
    EXPECT_EQ(bc.Read(), 0x8000001F);
    
    // Attempt to write valid bit + garbage
    bc.Write(0x40000000 | 0x3FFFFFE0);
    EXPECT_EQ(bc.Read(), 0xC000001F);
}
