// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDeviceIds.hpp - Canonical IEEE OUI vendor IDs, model IDs and display names for
// the FireWire audio devices ASFW recognizes.
//
// Single source of truth: both the DeviceProfiles audio providers (this layer) and
// Protocols/Audio/DeviceProtocolFactory reference these constants, so the metadata
// matcher and the runtime instantiator can never drift on identity.

#pragma once

#include <cstdint>

namespace ASFW::DeviceProfiles::Audio {

// ---- Focusrite (DICE / TCAT family) ----
inline constexpr uint32_t kFocusriteVendorId    = 0x00130e;
inline constexpr uint32_t kSPro40ModelId        = 0x000005;
inline constexpr uint32_t kLiquidS56ModelId     = 0x000006;
inline constexpr uint32_t kSPro24ModelId        = 0x000007;
inline constexpr uint32_t kSPro24DspModelId     = 0x000008;
inline constexpr uint32_t kSPro14ModelId        = 0x000009;
inline constexpr uint32_t kSPro26ModelId        = 0x000012;
inline constexpr uint32_t kSPro40Tcd3070ModelId = 0x0000de;

// Focusrite DICE devices encode the board model in GUID bits [27:22]; the legacy
// macOS driver uses the same field during probe.
inline constexpr uint32_t kFocusriteGuidModelSPro40Tcd3070 = 0x13;

// ---- Weiss Engineering (DICE / TCAT family) ----
// Model identifiers are from the vendor/model match table in Linux
// sound/firewire/dice/dice.c. Only INT202/INT203 are audio-enabled below; the
// remainder are recognized for identity so future verified profiles do not
// need to rediscover their Config-ROM identity.
inline constexpr uint32_t kWeissVendorId          = 0x001c6a;
inline constexpr uint32_t kWeissAdc2ModelId       = 0x000001;
inline constexpr uint32_t kWeissVestaModelId      = 0x000002;
inline constexpr uint32_t kWeissDac2ModelId       = 0x000003;
inline constexpr uint32_t kWeissAfi1ModelId       = 0x000004;
inline constexpr uint32_t kWeissInt202ModelId     = 0x000006;
inline constexpr uint32_t kWeissDac202ModelId     = 0x000007;
inline constexpr uint32_t kWeissMayaModelId       = 0x000008;
inline constexpr uint32_t kWeissInt203ModelId     = 0x00000a;
inline constexpr uint32_t kWeissMan301ModelId     = 0x00000b;

// ---- Apogee (Oxford / AV/C family) ----
inline constexpr uint32_t kApogeeVendorId    = 0x0003db;
inline constexpr uint32_t kApogeeDuetModelId = 0x01dddd;

// ---- TerraTec (BridgeCo / BeBoB family) ----
inline constexpr uint32_t kTerraTecVendorId     = 0x000aac;
inline constexpr uint32_t kPhase88RackFwModelId = 0x000003;

// ---- Alesis (DICE / TCAT family) ----
inline constexpr uint32_t kAlesisVendorId        = 0x000595;
inline constexpr uint32_t kAlesisMultiMixModelId = 0x000000;

// ---- Midas (DICE / TCAT family) ----
inline constexpr uint32_t kMidasVendorId       = 0x10c73f;
inline constexpr uint32_t kMidasVeniceModelId  = 0x000001;

// ---- Mackie / LOUD Technologies (Onyx-i family — production-run variant split) ----
// OUI 0x000ff2 is registered to LOUD Technologies (Mackie's parent company); Linux names
// it VENDOR_LOUD (sound/firewire/oxfw/oxfw.c) and OUI_LOUD (sound/firewire/dice/dice.c).
// Mackie shipped the Onyx-i mixers with two different FireWire implementations:
//   - former production: Oxford OXFW971, AV/C driven (Linux snd-oxfw). Published anchors:
//     Onyx 1640i = model 0x001640, Onyx Satellite = 0x00200f; the vendor-wide oxfw entry
//     otherwise disambiguates by model-name string ("Onyxi" / "Onyx-i").
//   - latter production: TCAT DICE with the LOUD category quirk (0x10 in the GUID
//     category byte instead of the standard 0x04) — Linux snd-dice check_dice_category();
//     its Kconfig lists "Onyx 820i/1220i/1620i/1640i (latter models)".
// Neither ALSA nor libffado publishes a model id for the 820i (both match Loud
// vendor-wide), so recognition is gated on a Config-ROM capture from a real unit. Replace
// the placeholder below with the captured model id (ASFW app -> device details) to
// activate identity matching in MackieAudioProfiles.hpp.
inline constexpr uint32_t kMackieVendorId              = 0x000ff2;
inline constexpr uint32_t kMackieModelIdPendingCapture = 0xffffffff;  // sentinel; real model ids are 24-bit
inline constexpr uint32_t kOnyx820iModelId             = kMackieModelIdPendingCapture;  // TODO(capture): from real 820i

// ---- PreSonus (DICE / TCAT family) ----
// The OUI is shared with PreSonus BeBoB-era devices (FireBox/FP10/Inspire) and the
// DICE FireStudio (model 0x000008); only exact vendor+model pairs may match.
// Sibling StudioLive model IDs from libffado 2.5.0; only the 16.0.2 is
// hardware-verified — the siblings are recognized by name but not audio-enabled
// until their stream geometry is captured from real hardware.
inline constexpr uint32_t kPreSonusVendorId      = 0x000a92;
inline constexpr uint32_t kStudioLive1602ModelId = 0x000013;
inline constexpr uint32_t kStudioLive1642ModelId = 0x000010;
inline constexpr uint32_t kStudioLive2442ModelId = 0x000012;
inline constexpr uint32_t kStudioLive3242ModelId = 0x000014;

// ---- Display names ----
inline constexpr const char* kFocusriteVendorName     = "Focusrite";
inline constexpr const char* kSPro40ModelName         = "Saffire Pro 40";
inline constexpr const char* kLiquidS56ModelName      = "Liquid Saffire 56";
inline constexpr const char* kSPro24ModelName         = "Saffire Pro 24";
inline constexpr const char* kSPro24DspModelName      = "Saffire Pro 24 DSP";
inline constexpr const char* kSPro14ModelName         = "Saffire Pro 14";
inline constexpr const char* kSPro26ModelName         = "Saffire Pro 26";
inline constexpr const char* kSPro40Tcd3070ModelName  = "Saffire Pro 40 (TCD3070)";
inline constexpr const char* kWeissVendorName         = "Weiss Engineering";
inline constexpr const char* kWeissAdc2ModelName      = "ADC2";
inline constexpr const char* kWeissVestaModelName     = "Vesta";
inline constexpr const char* kWeissDac2ModelName      = "DAC2 / Minerva";
inline constexpr const char* kWeissAfi1ModelName      = "AFI1";
inline constexpr const char* kWeissInt202ModelName    = "INT202";
inline constexpr const char* kWeissDac202ModelName    = "DAC202";
inline constexpr const char* kWeissMayaModelName      = "MAYA";
inline constexpr const char* kWeissInt203ModelName    = "INT203";
inline constexpr const char* kWeissMan301ModelName    = "MAN301";
inline constexpr const char* kApogeeVendorName        = "Apogee";
inline constexpr const char* kApogeeDuetModelName     = "Duet";
inline constexpr const char* kTerraTecVendorName      = "TerraTec Electronic GmbH";
inline constexpr const char* kPhase88RackFwModelName  = "PHASE 88 Rack FW";
inline constexpr const char* kAlesisVendorName        = "Alesis";
inline constexpr const char* kAlesisMultiMixModelName = "MultiMix FireWire";
inline constexpr const char* kMidasVendorName         = "Midas";
inline constexpr const char* kMidasVeniceModelName    = "Venice F32";
inline constexpr const char* kMackieVendorName        = "Mackie";
inline constexpr const char* kOnyx820iModelName       = "Onyx 820i";
inline constexpr const char* kPreSonusVendorName      = "PreSonus";
inline constexpr const char* kStudioLive1602ModelName = "StudioLive 16.0.2";
inline constexpr const char* kStudioLive1642ModelName = "StudioLive 16.4.2";
inline constexpr const char* kStudioLive2442ModelName = "StudioLive 24.4.2";
inline constexpr const char* kStudioLive3242ModelName = "StudioLive 32.4.2";

} // namespace ASFW::DeviceProfiles::Audio
