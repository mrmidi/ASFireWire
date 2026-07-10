// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.hpp - Aggregates the per-vendor audio profile providers into one
// query surface for Discovery.
//
// Metadata only: it never constructs or owns a runtime protocol object (that is the job
// of ASFW::Audio::AudioRuntimeRegistry). Stateless, header-only constexpr static helpers
// so callers incur no link dependency. Add generic, capability-derived providers here
// (ahead of the vendor providers) when a self-describing AV/C or TA 61883 device needs to
// be matched without a vendor table entry.

#pragma once

#include "../Common/DeviceProfileTypes.hpp"
#include "AudioProfileTypes.hpp"
#include "Vendors/AlesisAudioProfiles.hpp"
#include "Vendors/ApogeeAudioProfiles.hpp"
#include "Vendors/FocusriteAudioProfiles.hpp"
#include "Vendors/MidasAudioProfiles.hpp"
#include "Vendors/PreSonusAudioProfiles.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio {

class AudioProfileRegistry final {
public:
    /// Resolve display identity (names) and, where applicable, canonical/inferred IDs.
    /// Tries direct vendor/model matches first, then GUID-based inference (Focusrite).
    [[nodiscard]] static constexpr std::optional<DeviceIdentityHint>
    LookupIdentity(const DeviceProfileQuery& query) noexcept {
        if (auto hint = Focusrite::LookupIdentity(query)) { return hint; }
        if (auto hint = Apogee::LookupIdentity(query)) { return hint; }
        if (auto hint = Alesis::LookupIdentity(query)) { return hint; }
        if (auto hint = Midas::LookupIdentity(query)) { return hint; }
        if (auto hint = PreSonus::LookupIdentity(query)) { return hint; }
        if (auto hint = Focusrite::LookupIdentityByGuid(query)) { return hint; }
        return std::nullopt;
    }

    /// Resolve the best audio profile (family + integration mode) for a recognized
    /// device. Named "best" because a later phase may collect and rank multiple hints;
    /// today a device matches at most one vendor provider.
    [[nodiscard]] static constexpr std::optional<AudioProfileHint>
    LookupBestAudioProfile(const DeviceProfileQuery& query) noexcept {
        if (auto hint = Focusrite::LookupAudioProfile(query)) { return hint; }
        if (auto hint = Apogee::LookupAudioProfile(query)) { return hint; }
        if (auto hint = Alesis::LookupAudioProfile(query)) { return hint; }
        if (auto hint = Midas::LookupAudioProfile(query)) { return hint; }
        if (auto hint = PreSonus::LookupAudioProfile(query)) { return hint; }
        return std::nullopt;
    }
};

} // namespace ASFW::DeviceProfiles::Audio
