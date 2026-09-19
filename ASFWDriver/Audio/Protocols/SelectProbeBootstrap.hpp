// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include <cstdint>

namespace ASFW::Audio {

enum class ProbeBootstrap : uint8_t {
    Unsupported = 0,
    DiceProtocol,
    AvcInitializeThenPlug0,
    BeBoBPlug0Only,
    BeBoBUnprobed,
    FireworksEfc,
    MotuRegister,
};

[[nodiscard]] constexpr ProbeBootstrap SelectProbeBootstrap(
    DeviceProfiles::Audio::AudioFamilyProviderId family,
    DeviceProfiles::Audio::ProbePolicyId policy) noexcept {
    using FamilyId = DeviceProfiles::Audio::AudioFamilyProviderId;
    using ProbePolicyId = DeviceProfiles::Audio::ProbePolicyId;

    switch (family) {
        case FamilyId::GenericAvc:
            return policy == ProbePolicyId::GenericAvc
                       ? ProbeBootstrap::AvcInitializeThenPlug0
                       : ProbeBootstrap::Unsupported;
        case FamilyId::BeBoB:
            switch (policy) {
                case ProbePolicyId::BeBoBPlug0:
                    return ProbeBootstrap::BeBoBPlug0Only;
                case ProbePolicyId::BeBoBFilteredCommandSet:
                    return ProbeBootstrap::BeBoBUnprobed;
                case ProbePolicyId::None:
                case ProbePolicyId::NoAutomaticTraffic:
                case ProbePolicyId::GenericAvc:
                case ProbePolicyId::DiceTcat:
                case ProbePolicyId::OxfwAvc:
                case ProbePolicyId::FireworksEfc:
                case ProbePolicyId::MotuRegister:
                    return ProbeBootstrap::Unsupported;
            }
        case FamilyId::DICE:
            return policy == ProbePolicyId::DiceTcat
                       ? ProbeBootstrap::DiceProtocol
                       : ProbeBootstrap::Unsupported;
        case FamilyId::OXFW:
            return policy == ProbePolicyId::OxfwAvc
                       ? ProbeBootstrap::AvcInitializeThenPlug0
                       : ProbeBootstrap::Unsupported;
        case FamilyId::Fireworks:
            return policy == ProbePolicyId::FireworksEfc
                       ? ProbeBootstrap::FireworksEfc
                       : ProbeBootstrap::Unsupported;
        case FamilyId::MotuRegister:
            return policy == ProbePolicyId::MotuRegister
                       ? ProbeBootstrap::MotuRegister
                       : ProbeBootstrap::Unsupported;
        case FamilyId::None:
            return ProbeBootstrap::Unsupported;
    }
}

static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::BeBoB,
                                   DeviceProfiles::Audio::ProbePolicyId::BeBoBPlug0) ==
              ProbeBootstrap::BeBoBPlug0Only);
static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::BeBoB,
                                   DeviceProfiles::Audio::ProbePolicyId::BeBoBFilteredCommandSet) ==
              ProbeBootstrap::BeBoBUnprobed);
static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::GenericAvc,
                                   DeviceProfiles::Audio::ProbePolicyId::GenericAvc) ==
              ProbeBootstrap::AvcInitializeThenPlug0);
static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::OXFW,
                                   DeviceProfiles::Audio::ProbePolicyId::OxfwAvc) ==
              ProbeBootstrap::AvcInitializeThenPlug0);
static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::DICE,
                                   DeviceProfiles::Audio::ProbePolicyId::DiceTcat) ==
              ProbeBootstrap::DiceProtocol);
static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::Fireworks,
                                   DeviceProfiles::Audio::ProbePolicyId::FireworksEfc) ==
              ProbeBootstrap::FireworksEfc);
static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::MotuRegister,
                                   DeviceProfiles::Audio::ProbePolicyId::MotuRegister) ==
              ProbeBootstrap::MotuRegister);
static_assert(SelectProbeBootstrap(DeviceProfiles::Audio::AudioFamilyProviderId::None,
                                   DeviceProfiles::Audio::ProbePolicyId::None) ==
              ProbeBootstrap::Unsupported);

} // namespace ASFW::Audio
