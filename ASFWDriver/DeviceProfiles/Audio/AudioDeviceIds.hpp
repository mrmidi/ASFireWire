// SPDX-License-Identifier: LGPL-3.0-or-later
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

// ---- Alesis (DICE / TCAT family) ----
inline constexpr uint32_t kAlesisVendorId        = 0x000595;
inline constexpr uint32_t kAlesisMultiMixModelId = 0x000000;

// ---- Midas (DICE / TCAT family) ----
// Observed on real Midas Venice hardware via FFADO + Config ROM probing: FFADO lists
// vendorid=0x0010C73F, modelid=0x00000001 (its "Venice F32" entry), driver="DICE",
// mixer="Generic_Dice_EAP". The Venice F-series (F16/F24/F32) shares this IEEE OUI and
// 0x000001 is the only model id seen so far, so recognition matches the vendor (see
// Vendors/MidasAudioProfiles.hpp) and presents one honest "Venice" name rather than
// guessing the variant. Integration is deferred/fail-closed (mode kNone) until the DICE
// EAP / current-config path lands; see the Midas EAP probing design note.
inline constexpr uint32_t kMidasVendorId = 0x10c73f;
inline constexpr uint32_t kVeniceModelId = 0x000001;

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
inline constexpr const char* kAlesisVendorName        = "Alesis";
inline constexpr const char* kAlesisMultiMixModelName = "MultiMix FireWire";
inline constexpr const char* kMidasVendorName         = "Midas";
inline constexpr const char* kVeniceModelName         = "Venice";

} // namespace ASFW::DeviceProfiles::Audio
