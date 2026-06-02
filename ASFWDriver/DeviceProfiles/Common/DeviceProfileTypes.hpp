// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DeviceProfileTypes.hpp - Neutral, metadata-only inputs for device profile matching.
//
// DeviceProfiles is a metadata layer: it answers "what is this device, and which audio
// family/profile applies" from Config-ROM identity, WITHOUT constructing or owning any
// runtime protocol object. It must not depend on Protocols/Audio runtime classes.

#pragma once

#include <cstdint>

namespace ASFW::DeviceProfiles {

/// Where a matched hint came from (diagnostics / future precedence).
enum class MatchSource : uint8_t {
    ConfigROM,    // Self-described capability (Unit_Spec_Id / Unit_Sw_Version).
    VendorModel,  // Direct vendor_id + model_id table match.
    GUID,         // Inferred from GUID-encoded fields (e.g. Focusrite DICE board id).
};

/// Immutable identity inputs for a profile query. Scalars only — intentionally free of
/// Discovery types so DeviceProfiles carries no dependency on Discovery. (A
/// std::span<const Discovery::UnitDirectory> field can be added later, when generic
/// capability-derived providers land.)
struct DeviceProfileQuery {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
};

} // namespace ASFW::DeviceProfiles
