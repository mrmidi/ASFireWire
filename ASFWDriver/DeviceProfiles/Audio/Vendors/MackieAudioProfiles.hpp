// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieAudioProfiles.hpp - Mackie (LOUD Technologies) Onyx-i FireWire mixer knowledge.
// Knows ONLY Mackie/LOUD devices; performs no runtime protocol construction.
//
// Status: scaffold pending a Config-ROM capture from real hardware. The Onyx-i mixers
// were shipped with two different FireWire implementations over the production run (see
// AudioDeviceIds.hpp for the split and the Linux reference anchors), so the first capture
// must answer two questions before anything can be enabled:
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
// References consulted (behavioral only, no code copied):
//   - Linux sound/firewire/Kconfig — "Onyx 820i/1220i/1620i/1640i (latter models)" under
//     SND_DICE; "Onyx-i series (former models)" under SND_OXFW.
//   - Linux sound/firewire/oxfw/oxfw.c — VENDOR_LOUD 0x000ff2, detect_loud_models(),
//     LOUD stream quirks (WRONG_DBS, blocking transmission, 1640i NO-INFO packets).
//   - Linux sound/firewire/dice/dice.c — OUI_LOUD, LOUD_CATEGORY_ID 0x10.
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
    if constexpr (kOnyx820iModelId == kMackieModelIdPendingCapture) {
        // Placeholder still unset: no Config-ROM capture from a real 820i exists yet, so
        // this provider intentionally matches nothing. Replace kOnyx820iModelId in
        // AudioDeviceIds.hpp with the captured value to activate recognition.
        (void)query;
        return std::nullopt;
    } else {
        if (query.vendorId == kMackieVendorId && query.modelId == kOnyx820iModelId) {
            return DeviceIdentityHint{.vendorId = query.vendorId,
                                      .modelId = query.modelId,
                                      .vendorName = kMackieVendorName,
                                      .modelName = kOnyx820iModelName,
                                      .source = MatchSource::VendorModel};
        }
        return std::nullopt;
    }
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
