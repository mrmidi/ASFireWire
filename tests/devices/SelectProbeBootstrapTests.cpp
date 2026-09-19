// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>
#include "../../ASFWDriver/Audio/Protocols/SelectProbeBootstrap.hpp"

namespace ASFW::Audio::Tests {

using DeviceProfiles::Audio::AudioFamilyProviderId;
using DeviceProfiles::Audio::ProbePolicyId;

TEST(SelectProbeBootstrapTests, ReturnsExpectedBootstrapForFamilyPolicies) {
    // BeBoB
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::BeBoB, ProbePolicyId::BeBoBPlug0),
              ProbeBootstrap::BeBoBPlug0Only);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::BeBoB, ProbePolicyId::BeBoBFilteredCommandSet),
              ProbeBootstrap::BeBoBUnprobed);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::BeBoB, ProbePolicyId::GenericAvc),
              ProbeBootstrap::Unsupported);

    // GenericAvc
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::GenericAvc, ProbePolicyId::GenericAvc),
              ProbeBootstrap::AvcInitializeThenPlug0);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::GenericAvc, ProbePolicyId::BeBoBPlug0),
              ProbeBootstrap::Unsupported);

    // OXFW
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::OXFW, ProbePolicyId::OxfwAvc),
              ProbeBootstrap::AvcInitializeThenPlug0);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::OXFW, ProbePolicyId::DiceTcat),
              ProbeBootstrap::Unsupported);

    // DICE
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat),
              ProbeBootstrap::DiceProtocol);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::DICE, ProbePolicyId::GenericAvc),
              ProbeBootstrap::Unsupported);

    // Fireworks
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::Fireworks, ProbePolicyId::FireworksEfc),
              ProbeBootstrap::FireworksEfc);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::Fireworks, ProbePolicyId::OxfwAvc),
              ProbeBootstrap::Unsupported);

    // MotuRegister
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::MotuRegister, ProbePolicyId::MotuRegister),
              ProbeBootstrap::MotuRegister);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::MotuRegister, ProbePolicyId::GenericAvc),
              ProbeBootstrap::Unsupported);

    // None
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::None, ProbePolicyId::None),
              ProbeBootstrap::Unsupported);
    EXPECT_EQ(SelectProbeBootstrap(AudioFamilyProviderId::None, ProbePolicyId::GenericAvc),
              ProbeBootstrap::Unsupported);
}

} // namespace ASFW::Audio::Tests
