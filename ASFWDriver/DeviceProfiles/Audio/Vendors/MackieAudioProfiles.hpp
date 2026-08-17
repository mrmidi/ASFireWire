// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieAudioProfiles.hpp - Mackie (LOUD Technologies) Onyx-i FireWire mixer knowledge.
// Knows ONLY Mackie/LOUD devices; performs no runtime protocol construction.
//
// Status: recognition-only. The documented Onyx family models (Onyx-i Oxford run,
// Onyx 1640i in both runs, Onyx Blackbird) are recognized from libffado 2.5.0 device
// database ids — the same recognition-without-hardware policy as the PreSonus StudioLive
// siblings. No Onyx model is audio-enabled. A live capture (2026-08-17) confirmed an
// Oxford-run Onyx 820i in the field reporting the shared 0x081216 id with a 1394TA AV/C
// unit — see the provenance block in AudioDeviceIds.hpp. The Onyx-i mixers were shipped with two
// different FireWire implementations over the production run (see AudioDeviceIds.hpp for
// the split), and the DICE-run model ids (820i/1220i/1620i) are published nowhere, so for
// a DICE-run unit the first capture must answer two questions:
//
//   1. Which variant is the unit?
//      - DICE variant: unit directory carries the TCAT/DICE interface version and the
//        GUID category byte is LOUD's 0x10 (not the standard 0x04) — cross-check
//        Linux sound/firewire/dice/dice.c check_dice_category().
//      - OXFW971 variant: AV/C unit (specifier 0x00a02d, version 0x010001) and a
//        model-name string like "Onyx-i"; Linux snd-oxfw matches these vendor-wide and
//        disambiguates by name because their model ids were never published.
//   2. What are its vendor/model ids and (DICE) clock/stream capabilities, or (OXFW)
//      AV/C stream formats?
//
// Capture procedure (no code required): connect the mixer, open the ASFW app, run the
// read-only smoke, and record from device details: GUID, vendor id, model id, unit
// directory specifier + version, and the vendor/model name strings. Then replace
// kOnyx820iModelId in AudioDeviceIds.hpp with the captured model id to activate
// identity recognition below.
//
// Audio enablement is a separate, later step (see LookupAudioProfile).
//
// References consulted (behavioral only, no code copied; all in references/ locally):
//   - references/linux-sound-firewire-stack/Kconfig — "Onyx 820i/1220i/1620i/1640i
//     (latter models)" under SND_DICE; "Onyx-i series (former models)" under SND_OXFW.
//   - references/linux-sound-firewire-stack/oxfw/oxfw.c — VENDOR_LOUD 0x000ff2,
//     detect_loud_models(), LOUD stream quirks (WRONG_DBS, blocking transmission,
//     1640i NO-INFO packets).
//   - references/linux-sound-firewire-stack/dice/dice.c — OUI_LOUD, LOUD_CATEGORY_ID 0x10.
//   - references/libffado-2.5.0/configuration — Loud device_definitions (the ids above).
//   - references/alsa-userspace-control-protocols-impl — runtime/dice model.rs Blackbird
//     mapping; runtime/oxfw loud_model.rs + protocols::loud (future OXFW controls).
//   - FFADO-user thread "Mackie Onyx 820i and module snd_dice" (2016) — confirms
//     DICE-variant 820i units exist in the wild.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Mackie {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kMackieVendorId) {
        return std::nullopt;
    }
    // Documented family models: recognition only, ids from the libffado 2.5.0 device
    // database (see AudioDeviceIds.hpp for per-id provenance). Audio stays off for all
    // of them until stream geometry is captured — see LookupAudioProfile.
    const char* modelName = nullptr;
    switch (query.modelId) {
    case kOnyxIOxfwModelId:     modelName = kOnyxIOxfwModelName; break;
    case kOnyx1640iOxfwModelId:
    case kOnyx1640iDiceModelId: modelName = kOnyx1640iModelName; break;
    case kOnyxBlackbirdModelId: modelName = kOnyxBlackbirdModelName; break;
    default: break;
    }
    if (modelName != nullptr) {
        return DeviceIdentityHint{.vendorId = query.vendorId,
                                  .modelId = query.modelId,
                                  .vendorName = kMackieVendorName,
                                  .modelName = modelName,
                                  .source = MatchSource::VendorModel};
    }
    if constexpr (kOnyx820iModelId != kMackieModelIdPendingCapture) {
        // Activated once a Config-ROM capture from a real 820i replaces the sentinel in
        // AudioDeviceIds.hpp. (A DICE-run 820i reports a model id nobody has published;
        // an OXFW-run unit should already match the shared 0x081216 entry above.)
        if (query.modelId == kOnyx820iModelId) {
            return DeviceIdentityHint{.vendorId = query.vendorId,
                                      .modelId = query.modelId,
                                      .vendorName = kMackieVendorName,
                                      .modelName = kOnyx820iModelName,
                                      .source = MatchSource::VendorModel};
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    // Intentionally never matches yet. Even after the model id is captured and
    // LookupIdentity starts returning a hint, audio must stay disabled until the stream
    // geometry is verified against the real unit — a wrong data-block size means the
    // device never locks to the host stream (same policy as the PreSonus StudioLive
    // siblings). Expected shape once verified:
    //   - DICE variant    -> AudioProfileHint{AudioProtocolFamily::DICE,
    //                                         AudioIntegrationMode::kHardcodedNub, ...}
    //     (geometry from the DICE GLOBAL/TX/RX sections at capture time; note the LOUD
    //     category quirk 0x10 if DICE detection checks the GUID category byte)
    //   - OXFW971 variant -> AudioProfileHint{AudioProtocolFamily::Oxford,
    //                                         AudioIntegrationMode::kAVCDriven, ...}
    //     (formats via AV/C stream-format reads; expect the LOUD quirks documented in
    //     Linux snd-oxfw)
    (void)query;
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Mackie
