// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DeviceStreamModeQuirks.cpp - Vendor/model stream mode overrides

#include "DeviceStreamModeQuirks.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../DeviceProfiles/Audio/Vendors/BeBoBDeviceProfiles.hpp"

namespace ASFW::Audio::Quirks {

namespace {
// Identities come from the shared DeviceProfiles table. They were previously
// respelled here, which meant every id in this file was a second copy free to
// drift from the one the rest of the driver matches on. The local names are kept
// so the rules below still read as rules.
namespace ids = DeviceProfiles::Audio;

constexpr uint32_t kApogeeVendorId    = ids::kApogeeVendorId;
constexpr uint32_t kApogeeDuetModelId = ids::kApogeeDuetModelId;

// Focusrite DICE devices — Linux kernel dice-stream.c unconditionally uses CIP_BLOCKING.
constexpr uint32_t kFocusriteVendorId = ids::kFocusriteVendorId;
constexpr uint32_t kSPro14ModelId     = ids::kSPro14ModelId;
constexpr uint32_t kSPro24ModelId     = ids::kSPro24ModelId;
constexpr uint32_t kSPro24DspModelId  = ids::kSPro24DspModelId;
constexpr uint32_t kSPro40ModelId     = ids::kSPro40ModelId;

// Midas DICE devices — same rationale as Focusrite above.
constexpr uint32_t kMidasVendorId      = ids::kMidasVendorId;
constexpr uint32_t kMidasVeniceModelId = ids::kMidasVeniceModelId;

// LOUD Technologies (Mackie Onyx family).
constexpr uint32_t kLoudMackieVendorId = ids::kMackieVendorId;

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

    // Focusrite Saffire DICE devices:
    // Linux kernel DICE driver unconditionally uses CIP_BLOCKING (dice-stream.c:508).
    // DICE devices expect blocking cadence (8 samples/packet + NO-DATA packets).
    if (vendorId == kFocusriteVendorId &&
        (modelId == kSPro14ModelId ||
         modelId == kSPro24ModelId ||
         modelId == kSPro24DspModelId ||
         modelId == kSPro40ModelId)) {
        return Model::StreamMode::kBlocking;
    }

    if (vendorId == kMidasVendorId && modelId == kMidasVeniceModelId) {
        return Model::StreamMode::kBlocking;
    }

    // LOUD/Mackie, vendor-wide: Linux snd-oxfw forces blocking transmission for
    // every Loud OXFW unit (oxfw.c:189-196, SND_OXFW_QUIRK_BLOCKING_TRANSMISSION),
    // and Linux snd-dice is unconditionally CIP_BLOCKING — so blocking is correct
    // for both production runs of the Onyx family.
    if (vendorId == kLoudMackieVendorId) {
        return Model::StreamMode::kBlocking;
    }

    // Linux's BeBoB path initializes AMDTP with CIP_BLOCKING
    // (sound/firewire/bebob/bebob_stream.c:400-465). Applies to all BeBoB devices.
    if (DeviceProfiles::Audio::BeBoB::IsBeBoBDevice(vendorId, modelId)) {
        return Model::StreamMode::kBlocking;
    }

    return std::nullopt;
}

} // namespace ASFW::Audio::Quirks
