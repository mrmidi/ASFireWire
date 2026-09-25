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

// Focusrite DICE devices encode the board model in the GUID's product field.
// Saffire.kext 4.3.0 probe() reads it as GUID bits [31:22] (after requiring
// category byte 0x04) and names 0x13 "Saffire Pro40"; 4.1.4 had no such entry.
// The catalog's GuidEncoded clause masks only bits [27:22].
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
// The iO14/iO26. Named in no vendor id table -- the Alesis kext's three entries
// are MultiMix (id 0), iO (id != 0) and MasterControl -- but it shares the
// MultiMix's habit of advertising two capture streams when it has one
// (libffado-2.5.0/src/dice/dice_avdevice.cpp:1682-1695), which is why it has a
// row in the catalog. Recognition only; its geometry has never been captured.
inline constexpr uint32_t kAlesisIoModelId = 0x000001;

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
// Documented family identities, recognition-only (same policy as the PreSonus StudioLive
// siblings: identity from the libffado 2.5.0 device database; audio stays off until the
// stream geometry is captured from hardware). Provenance per id, from
// references/libffado-2.5.0/configuration device_definitions:
//   0x081216 "Onyx-i"          OXFORD driver (H. Dehnhardt entry) — the shared model id of
//                              the OXFW971 production run; units differ by name string
//   0x001640 "Onyx 1640i"      OXFORD driver (S. Tonge entry; matches Linux snd-oxfw
//                              MODEL_ONYX_1640I)
//   0x000006 "Onyx 1640i"      DICE driver (T. Kepley entry)
//   0x000007 "Onyx Blackbird"  DICE driver (M. Bernkopf entry; cross-checked with
//                              snd-firewire-ctl-services runtime/dice/src/model.rs:122)
// Live capture (2026-08-17, Onyx 820i on an Apple TB->FW643 chain, ASFW enumeration):
//   GUID 0x000FF20400003AFC, vendor 0x000FF2, model 0x081216, unit specifier 0x00A02D
//   version 0x010001 (1394TA AV/C), ROM strings "Loud Technologies Inc." / "Onyx-i".
//   Confirms an Oxford-run 820i reports the shared 0x081216 id; the GUID category byte
//   is 0x04, not the 0x10 Linux snd-dice requires for Loud DICE units, so the DICE gate
//   rejects it and snd-oxfw's name match ("Onyx-i") claims it — consistent on all axes.
// The DICE-run 820i/1220i/1620i model ids remain unpublished (ALSA and libffado match
// them vendor-wide/generically), so the DICE-run 820i placeholder below stays
// sentinel-gated until captured from a real DICE-run unit.
inline constexpr uint32_t kMackieVendorId              = 0x000ff2;
inline constexpr uint32_t kMackieModelIdPendingCapture = 0xffffffff;  // sentinel; real model ids are 24-bit
inline constexpr uint32_t kOnyxIOxfwModelId            = 0x081216;
inline constexpr uint32_t kOnyx1640iOxfwModelId        = 0x001640;
inline constexpr uint32_t kOnyx1640iDiceModelId        = 0x000006;
inline constexpr uint32_t kOnyxBlackbirdModelId        = 0x000007;
inline constexpr uint32_t kOnyx820iModelId             = kMackieModelIdPendingCapture;  // TODO(capture): from real 820i
// Echo Fireworks production run (not Oxford, not DICE): Linux snd-fireworks
// fireworks.c VENDOR_LOUD 0x000ff2, MODEL_MACKIE_400F 0x00400f, MODEL_MACKIE_1200F
// 0x01200f; matched there on unit specifier 0x00a02d / version 0x010000 (AV/C).
// Control is EFC (Audio/Protocols/Fireworks), streaming is CMP + AM824 blocking.
// 400F: static 10x10 geometry pending HWINFO capture; 1200F: recognition only.
inline constexpr uint32_t kOnyx400FModelId             = 0x00400f;
inline constexpr uint32_t kOnyx1200FModelId            = 0x01200f;

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

// ---- M-Audio / Avid (BridgeCo BeBoB family, "special" firmware) ----
// Recognised for their probe bound, not for audio: this branch has no
// MAudioSpecialProtocol. Their firmware hangs on AV/C it does not implement
// (Protocols/AVC/AVC_DEVICE_HAZARDS.md H1), so being unrecognised is the unsafe
// state -- an unmatched AV/C unit is opened with generic UNIT_INFO/SUBUNIT_INFO.
// Model ids from Linux sound/firewire/bebob/bebob.c (MODEL_MAUDIO_FW1814,
// MODEL_MAUDIO_PROJECTMIX and the 0x00010070 bootloader persona).
inline constexpr uint32_t kMAudioVendorId                      = 0x000d6c;
inline constexpr uint32_t kMAudioFireWire1814BootloaderModelId = 0x00010070;
inline constexpr uint32_t kMAudioFireWire1814ModelId           = 0x00010071;
inline constexpr uint32_t kMAudioProjectMixModelId             = 0x00010091;

// ---- MOTU (vendor-specific register protocol) ----
// MOTU does not use model_id: the root directory publishes model_id 0 and the model is
// identified by Unit_Sw_Version, with Unit_Spec_Id equal to the OUI. Version values from
// Linux sound/firewire/motu/motu.c:162-181. Only the 828mk2 is hardware-verified here
// (config ROM captured 2026-07-26: vendor 0x0001f2, spec 0x0001f2, version 0x000003);
// the protocol-v2 siblings are recognized by name but not audio-enabled until their
// chunk layouts are confirmed against real hardware.
inline constexpr uint32_t kMotuVendorId          = 0x0001f2;
inline constexpr uint32_t kMotu828mk2SwVersion   = 0x000003;
inline constexpr uint32_t kMotu896hdSwVersion    = 0x000005;
inline constexpr uint32_t kMotuTravelerSwVersion = 0x000009;
inline constexpr uint32_t kMotuUltraliteSwVersion = 0x00000d;
inline constexpr uint32_t kMotu8preSwVersion     = 0x00000f;

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
inline constexpr const char* kAlesisIoModelName       = "iO14 / iO26";
inline constexpr const char* kMidasVendorName         = "Midas";
inline constexpr const char* kMidasVeniceModelName    = "Venice F32";
inline constexpr const char* kMackieVendorName        = "Mackie";
inline constexpr const char* kOnyxIOxfwModelName      = "Onyx-i (Oxford)";
inline constexpr const char* kOnyx1640iModelName      = "Onyx 1640i";
inline constexpr const char* kOnyxBlackbirdModelName  = "Onyx Blackbird";
inline constexpr const char* kOnyx820iModelName       = "Onyx 820i";
inline constexpr const char* kOnyx400FModelName       = "Onyx 400F";
inline constexpr const char* kOnyx1200FModelName      = "Onyx 1200F";
inline constexpr const char* kPreSonusVendorName      = "PreSonus";
inline constexpr const char* kStudioLive1602ModelName = "StudioLive 16.0.2";
inline constexpr const char* kStudioLive1642ModelName = "StudioLive 16.4.2";
inline constexpr const char* kStudioLive2442ModelName = "StudioLive 24.4.2";
inline constexpr const char* kStudioLive3242ModelName = "StudioLive 32.4.2";
inline constexpr const char* kMAudioVendorName        = "M-Audio";
inline constexpr const char* kMAudioFireWire1814BootloaderModelName = "FireWire 1814 (bootloader)";
inline constexpr const char* kMAudioFireWire1814ModelName = "FireWire 1814";
inline constexpr const char* kMAudioProjectMixModelName   = "ProjectMix I/O";
inline constexpr const char* kMotuVendorName          = "MOTU";
inline constexpr const char* kMotu828mk2ModelName     = "828mkII";
inline constexpr const char* kMotu896hdModelName      = "896HD";
inline constexpr const char* kMotuTravelerModelName   = "Traveler";
inline constexpr const char* kMotuUltraliteModelName  = "UltraLite";
inline constexpr const char* kMotu8preModelName       = "8pre";

} // namespace ASFW::DeviceProfiles::Audio
