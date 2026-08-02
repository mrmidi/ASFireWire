// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileTypes.hpp - Metadata hints describing which audio family/profile a
// recognized device belongs to. These are facts ABOUT the device, not instructions for
// how to instantiate a runtime protocol (that lives in the Audio layer).

#pragma once

#include "../Common/DeviceProfileTypes.hpp"

#include <cstdint>

namespace ASFW::DeviceProfiles::Audio {

/// How a recognized device integrates with the audio stack.
///
/// Value-compatible with the legacy ASFW::Audio::DeviceIntegrationMode, which is kept as
/// a using-alias of this type so existing audio-internal call sites are unaffected.
enum class AudioIntegrationMode : uint8_t {
    kNone = 0,      // Recognized but not driven (deferred multistream models).
    kHardcodedNub,  // Vendor-specific audio backend (DICE/TCAT), no AV/C.
    kAVCDriven,     // AV/C discovery drives topology; vendor protocol adds extra controls.
};

/// Audio protocol family a device belongs to. Forward-looking: consumed today only for
/// diagnostics, but it lets the registry grow generic (capability-derived) providers
/// later without changing the hint shape.
enum class AudioProtocolFamily : uint8_t {
    Unknown = 0,
    AVC,
    TA61883,
    DICE,
    Oxford,
    VendorSpecific,
};

/// Identity enrichment for a recognized device (display names + canonical/inferred IDs).
struct DeviceIdentityHint {
    uint32_t vendorId{0};
    uint32_t modelId{0};
    const char* vendorName{nullptr};
    const char* modelName{nullptr};
    MatchSource source{MatchSource::VendorModel};
};

/// The audio profile that best applies to a recognized device.
struct AudioProfileHint {
    AudioProtocolFamily family{AudioProtocolFamily::Unknown};
    AudioIntegrationMode mode{AudioIntegrationMode::kNone};
    MatchSource source{MatchSource::VendorModel};
};

} // namespace ASFW::DeviceProfiles::Audio
