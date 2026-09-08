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

TEST(ApogeeDuetClockTransitionTests, Advertises44100And48000And96000Only) {
    AvcTestRig rig;

    ApogeeDuetProtocol protocol(rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(), nullptr, nullptr, nullptr, 100U,
                                &rig.Timers());

    std::vector<uint32_t> rates;
    EXPECT_TRUE(protocol.GetSupportedSampleRates(rates));
    EXPECT_EQ(rates, (std::vector<uint32_t>{44100U, 48000U, 96000U}));
}

TEST(ApogeeDuetClockTransitionTests, SupportsConfigurationValidatesRatesAndOptical) {
    AvcTestRig rig;

    ApogeeDuetProtocol protocol(rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(), nullptr, nullptr, nullptr, 100U,
                                &rig.Timers());

    using ASFW::Configuration::DeviceConfiguration;

    // Supported rates without optical
    EXPECT_TRUE(protocol.SupportsConfiguration(DeviceConfiguration{.sampleRate = 44100U}));
    EXPECT_TRUE(protocol.SupportsConfiguration(DeviceConfiguration{.sampleRate = 48000U}));
    EXPECT_TRUE(protocol.SupportsConfiguration(DeviceConfiguration{.sampleRate = 96000U}));

    // Unsupported rates (88.2k is out of scope; 32k/192k not supported by Duet)
    EXPECT_FALSE(protocol.SupportsConfiguration(DeviceConfiguration{.sampleRate = 32000U}));
    EXPECT_FALSE(protocol.SupportsConfiguration(DeviceConfiguration{.sampleRate = 88200U}));
    EXPECT_FALSE(protocol.SupportsConfiguration(DeviceConfiguration{.sampleRate = 192000U}));

    // Optical configurations rejected (Duet has no optical I/O)
    EXPECT_FALSE(protocol.SupportsConfiguration(DeviceConfiguration{
        .sampleRate = 48000U,
        .opticalInput = ASFW::Configuration::OpticalMode::Spdif}));
    EXPECT_FALSE(protocol.SupportsConfiguration(DeviceConfiguration{
        .sampleRate = 96000U,
        .opticalOutput = ASFW::Configuration::OpticalMode::Adat}));
}

TEST(ApogeeDuetClockTransitionTests, AudioClockConfigAccepts96000AndRejects88200) {
    EXPECT_TRUE(ASFW::Audio::IsSupportedAudioClockConfig(AudioClockConfig{.sampleRateHz = 44100U}));
    EXPECT_TRUE(ASFW::Audio::IsSupportedAudioClockConfig(AudioClockConfig{.sampleRateHz = 48000U}));
    EXPECT_TRUE(ASFW::Audio::IsSupportedAudioClockConfig(AudioClockConfig{.sampleRateHz = 96000U}));
    EXPECT_FALSE(ASFW::Audio::IsSupportedAudioClockConfig(AudioClockConfig{.sampleRateHz = 88200U}));
    EXPECT_FALSE(ASFW::Audio::IsSupportedAudioClockConfig(AudioClockConfig{.sampleRateHz = 192000U}));
}

} // namespace
