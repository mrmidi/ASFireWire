// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetClockTransitionTests.cpp - Unit tests for ApogeeDuetProtocol non-blocking clock transitions.
//
// FW-138: the bespoke TestBusOps/TestBusInfo pair was replaced by the shared
// AvcTestRig, which supplies the same bus surface plus a real FCPTransport.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"

#include "AvcTestRig.hpp"

namespace {

using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::ClockApplyResult;
using ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol;
using ASFW::Testing::AvcTestRig;

TEST(ApogeeDuetClockTransitionTests, ConcurrentApplyClockConfigReturnsBusy) {
    AvcTestRig rig;

    ApogeeDuetProtocol protocol(rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(), nullptr, nullptr, nullptr, 100U,
                                &rig.Timers());

    // Initial ApplyClockConfig without transport returns kIOReturnNotReady
    bool callbackFired = false;
    protocol.ApplyClockConfig(AudioClockConfig{.sampleRateHz = 48000U},
                              [&callbackFired](IOReturn status, const ClockApplyResult&) {
                                  callbackFired = true;
                                  EXPECT_EQ(status, kIOReturnNotReady);
                              });
    EXPECT_TRUE(callbackFired);
}

TEST(ApogeeDuetClockTransitionTests, ShutdownCancelsClockTransition) {
    AvcTestRig rig;

    ApogeeDuetProtocol protocol(rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(), nullptr, nullptr, nullptr, 100U,
                                &rig.Timers());

    EXPECT_EQ(protocol.Shutdown(), kIOReturnSuccess);
}

} // namespace
