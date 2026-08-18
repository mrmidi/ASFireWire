// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MackieAudioProfiles.hpp - Mackie (LOUD Technologies) Onyx-i FireWire mixer knowledge.
// Knows ONLY Mackie/LOUD devices; performs no runtime protocol construction.
//
// Status: the Onyx-i Oxford run (shared id 0x081216) is audio-enabled as
// {Oxford, kAVCDriven}, geometry-verified against a real Onyx 820i (identity capture +
// AV/C stream-format capture, 2026-08-17 — see the provenance block in
// AudioDeviceIds.hpp and the wire-format block below). Onyx 1640i (both runs) and
// Onyx Blackbird remain recognition-only from libffado 2.5.0 database ids — the same
// recognition-without-hardware policy as the PreSonus StudioLive siblings. The Onyx-i mixers were shipped with two
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
// Live AV/C stream-format capture (2026-08-17, same Oxford-run 820i, via the MCP
// control plane, STATUS-class FCP only — EXTENDED STREAM FORMAT INFORMATION 0xBF):
//   PLUG INFO: 1 isoch input plug, 1 isoch output plug, 1 external in, 1 external out.
//   Output plug 0 (capture, device->host):  8ch MBLA (0x06), compound AM824 (0x90 0x40),
//     supported rates 44.1/48/88.2/96 kHz (codes 0x03/0x04/0x0A/0x05); current 44.1 kHz.
//   Input plug 0 (playback, host->device):  2ch MBLA, same four rates; current 44.1 kHz.
//   No IEC60958 or MIDI (0x0D) fields in any entry. Raw frames recorded in the local
//   runbook. Enablement implications: asymmetric duplex (8 in / 2 out), and the Loud
//   quirks from Linux snd-oxfw apply (unreliable CIP DBS -> derive stride from the
//   format, and blocking-mode transmission expected by the device).
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
    if (query.vendorId != kMackieVendorId) {
        return std::nullopt;
    }
    // Onyx-i (Oxford run, shared id 0x081216): AV/C-driven, geometry verified against a
    // real 820i (stream-format capture in the header of this file). Wire geometry lives
    // in the ADK profile (Audio/DriverKit/Config/AVC/MackieOnyx820iProfile). Streaming
    // additionally requires the runtime duplex control, which intentionally does not
    // exist yet — DeviceProtocolFactory has no Mackie clause, so StartStreaming fails
    // cleanly while CoreAudio can already publish the device.
    if (query.modelId == kOnyxIOxfwModelId) {
        return AudioProfileHint{.family = AudioProtocolFamily::Oxford,
                                .mode = AudioIntegrationMode::kAVCDriven,
                                .source = MatchSource::VendorModel};
    }
    // Onyx 1640i (both runs) and Onyx Blackbird stay recognition-only: no stream
    // geometry has been verified against their hardware (same policy as the PreSonus
    // StudioLive siblings). A DICE-run device would take
    // {AudioProtocolFamily::DICE, kHardcodedNub} once captured — note the LOUD DICE
    // category quirk (0x10) documented in Linux snd-dice.
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Mackie
