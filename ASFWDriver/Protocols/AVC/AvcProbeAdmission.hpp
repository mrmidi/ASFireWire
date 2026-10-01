// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcProbeAdmission.hpp - Whether a Config ROM unit gets AV/C traffic, and which.
//
// Two inputs decide it: the unit directory's specifier and the catalog's plan.
// The specifier says the unit speaks AV/C; the plan says what we may send it.
// No other code decides whether generic AV/C discovery starts.

#pragma once

#include "../../Audio/Protocols/SelectProbeBootstrap.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

#include <cstdint>

namespace ASFW::Protocols::AVC {

/// 1394 Trade Association specifier in a unit directory. Linux BeBoB and
/// OXFW match on it (bebob.c:355-364, oxfw.c:315-326); OXFW also requires
/// AV/C version 0x010001, BeBoB does not (bebob.c:352-354).
inline constexpr uint32_t kTa1394SpecifierId = 0x00A02D;

[[nodiscard]] constexpr bool IsTa1394Unit(uint32_t specifierId) noexcept {
    return (specifierId & 0xFFFFFFU) == kTa1394SpecifierId;
}

enum class AvcProbeDecision : uint8_t {
    /// The unit directory is not 1394 TA: not ours.
    NotAvcUnit,
    /// No current catalog plan (unknown non-standard unit, hazardous identity,
    /// stale route). Unrecognised is the unsafe state for AV/C: no traffic.
    NoPolicy,
    /// UNIT INFO, SUBUNIT INFO, then music/audio subunit descriptors.
    GenericDiscovery,
    /// BridgeCo plug probes only; no generic UNIT/SUBUNIT INFO.
    BeBoBPlug0,
    /// No probe at all; the catalog's fixed geometry is published (M-Audio
    /// special firmware freezes on generic probes).
    ProfileOwned,
    /// AV/C directory present but driven by EFC.
    FireworksEfc,
    /// DICE, MOTU or RME: the AV/C directory is incidental.
    RegisterDriven,
    /// The plan's family/policy pair has no bring-up.
    Refused,
};

[[nodiscard]] inline AvcProbeDecision DecideAvcProbe(
    uint32_t specifierId,
    const DeviceProfiles::Audio::StaticAudioEndpointPlan* plan) noexcept {
    using ASFW::Audio::ProbeBootstrap;
    if (!IsTa1394Unit(specifierId)) {
        return AvcProbeDecision::NotAvcUnit;
    }
    if (plan == nullptr) {
        return AvcProbeDecision::NoPolicy;
    }
    switch (ASFW::Audio::SelectProbeBootstrap(*plan)) {
        case ProbeBootstrap::AvcInitializeThenPlug0:
            return AvcProbeDecision::GenericDiscovery;
        case ProbeBootstrap::BeBoBPlug0Only:
            return AvcProbeDecision::BeBoBPlug0;
        case ProbeBootstrap::BeBoBUnprobed:
            return AvcProbeDecision::ProfileOwned;
        case ProbeBootstrap::FireworksEfc:
            return AvcProbeDecision::FireworksEfc;
        case ProbeBootstrap::DiceProtocol:
        case ProbeBootstrap::MotuRegister:
        case ProbeBootstrap::RmeRegister:
            return AvcProbeDecision::RegisterDriven;
        case ProbeBootstrap::Unsupported:
            return AvcProbeDecision::Refused;
    }
    return AvcProbeDecision::Refused;
}

} // namespace ASFW::Protocols::AVC
