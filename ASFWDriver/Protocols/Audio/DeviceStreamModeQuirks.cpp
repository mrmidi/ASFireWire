// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DeviceStreamModeQuirks.cpp - Vendor/model stream mode overrides

#include "DeviceStreamModeQuirks.hpp"

namespace ASFW::Audio::Quirks {

namespace {
constexpr uint32_t kApogeeVendorId = 0x0003DB;
constexpr uint32_t kApogeeDuetModelId = 0x01DDDD;
} // namespace

std::optional<Model::StreamMode> LookupForcedStreamMode(
    uint32_t vendorId,
    uint32_t modelId) noexcept {
    // Apogee Duet quirk:
    // - Discovery reports/supports non-blocking, and host playback can work in that mode.
    // - Observed device output stream cadence is blocking.
    // Force blocking so host/device cadence stays aligned and stream sync remains stable.
    if (vendorId == kApogeeVendorId && modelId == kApogeeDuetModelId) {
        return Model::StreamMode::kBlocking;
    }

    return std::nullopt;
}

} // namespace ASFW::Audio::Quirks
