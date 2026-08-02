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
inline constexpr const char* kApogeeVendorName        = "Apogee";
inline constexpr const char* kApogeeDuetModelName     = "Duet";
inline constexpr const char* kTerraTecVendorName      = "TerraTec Electronic GmbH";
inline constexpr const char* kPhase88RackFwModelName  = "PHASE 88 Rack FW";
inline constexpr const char* kAlesisVendorName        = "Alesis";
inline constexpr const char* kAlesisMultiMixModelName = "MultiMix FireWire";
inline constexpr const char* kMidasVendorName         = "Midas";
inline constexpr const char* kMidasVeniceModelName    = "Venice F32";
inline constexpr const char* kPreSonusVendorName      = "PreSonus";
inline constexpr const char* kStudioLive1602ModelName = "StudioLive 16.0.2";
inline constexpr const char* kStudioLive1642ModelName = "StudioLive 16.4.2";
inline constexpr const char* kStudioLive2442ModelName = "StudioLive 24.4.2";
inline constexpr const char* kStudioLive3242ModelName = "StudioLive 32.4.2";

} // namespace ASFW::DeviceProfiles::Audio
