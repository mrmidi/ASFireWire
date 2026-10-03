// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DescriptorTypeCodes.hpp - Named type codes and flag bits of AV/C descriptors, lists, entries
// and information blocks. Every value cites the table that defines it; values that no local
// spec defines say where they come from instead.
//
// Specs (1papers): TA 2002013 AV/C Descriptor Mechanism 1.2, TA 1999045 Information Blocks,
// TA 2001007 Music Subunit 1.0, TA 1999008 Audio Subunit 1.0.

#pragma once

#include <cstdint>

namespace ASFW::Protocols::AVC::Descriptors {

// ---------------------------------------------------------------------------
// Information block types (TA 1999045)
// ---------------------------------------------------------------------------

inline constexpr uint16_t kInfoBlockRawText = 0x000A;  ///< Raw Text Info Block, §4.8
inline constexpr uint16_t kInfoBlockName = 0x000B;     ///< Name Info Block, §4.9

// ---------------------------------------------------------------------------
// Music Subunit status descriptor info blocks (TA 2001007 §6.2). The same numbers mean other
// things inside the identifier descriptor, where capabilities are plain fields (§5.2), not
// typed blocks.
// ---------------------------------------------------------------------------

inline constexpr uint16_t kMusicInfoBlockGeneralStatusArea = 0x8100;    ///< §6.2.1; carries tx/rx capability, latency
inline constexpr uint16_t kMusicInfoBlockOutputPlugStatusArea = 0x8101; ///< §6.2.2; [number_of_source_plugs] + nested 8102
inline constexpr uint16_t kMusicInfoBlockSourcePlugStatus = 0x8102;     ///< §6.2.3; [source_plug_number] + nested 8103..8107
inline constexpr uint16_t kMusicInfoBlockAudioInfo = 0x8103;            ///< §6.2.3.1
inline constexpr uint16_t kMusicInfoBlockMidiInfo = 0x8104;             ///< §6.2.3.2
inline constexpr uint16_t kMusicInfoBlockSmpteTimeCodeInfo = 0x8105;    ///< §6.2.3.3
inline constexpr uint16_t kMusicInfoBlockSampleCountInfo = 0x8106;      ///< §6.2.3.4
inline constexpr uint16_t kMusicInfoBlockAudioSyncInfo = 0x8107;        ///< §6.2.3.5
// 8108..810B are not in TA 2001007 1.0. Their numbers come from Apple's AVCVideoServices
// MusicSubunitController.h:68-79 (APSL, behaviour only) and are confirmed by the Phase 88 and
// Duet descriptor captures (documentation/avc-rebuild/fixtures).
inline constexpr uint16_t kMusicInfoBlockRoutingStatus = 0x8108;
inline constexpr uint16_t kMusicInfoBlockSubunitPlugInfo = 0x8109;
inline constexpr uint16_t kMusicInfoBlockClusterInfo = 0x810A;
inline constexpr uint16_t kMusicInfoBlockMusicPlugInfo = 0x810B;

// Music plug port types (cluster info 810A). Not in TA 2001007 1.0 (no port type table in the
// local copy); from Apple's AVCVideoServices MusicSubunitController.h:107-122 (APSL, behaviour only).
inline constexpr uint8_t kMusicPortTypeSpeaker = 0x00;
inline constexpr uint8_t kMusicPortTypeHeadphone = 0x01;
inline constexpr uint8_t kMusicPortTypeMicrophone = 0x02;
inline constexpr uint8_t kMusicPortTypeLine = 0x03;
inline constexpr uint8_t kMusicPortTypeSpdif = 0x04;
inline constexpr uint8_t kMusicPortTypeAdat = 0x05;
inline constexpr uint8_t kMusicPortTypeTdif = 0x06;
inline constexpr uint8_t kMusicPortTypeMadi = 0x07;
inline constexpr uint8_t kMusicPortTypeAnalog = 0x08;
inline constexpr uint8_t kMusicPortTypeDigital = 0x09;
inline constexpr uint8_t kMusicPortTypeMidi = 0x0A;
inline constexpr uint8_t kMusicPortTypeAesEbu = 0x0B;
inline constexpr uint8_t kMusicPortTypeNone = 0xFF;

// Music plug type and routing support (music plug info 810B, primary bytes 0 and 3). FFADO
// avc_descriptor_music.cpp:376-470 gives the field order (type, id, routing support, source, destination);
// the values are from Apple's MusicSubunitController.h:148-165.
inline constexpr uint8_t kMusicPlugTypeAudio = 0x00;
inline constexpr uint8_t kMusicPlugTypeMidi = 0x01;
inline constexpr uint8_t kMusicPlugTypeSmpte = 0x02;
inline constexpr uint8_t kMusicPlugTypeSampleCount = 0x03;
inline constexpr uint8_t kMusicPlugTypeSync = 0x80;
inline constexpr uint8_t kMusicRoutingSupportFixed = 0x00;
inline constexpr uint8_t kMusicRoutingSupportCluster = 0x01;
inline constexpr uint8_t kMusicRoutingSupportFlexible = 0x02;
inline constexpr uint8_t kMusicRoutingSupportUnknown = 0xFF;

// Music subunit plug usage (subunit plug info 8109, primary byte 3). Not in the local TA 2001007 1.0;
// from Apple's MusicSubunitController.h:96-105 (kIsochStreamSubunitPlug ...).
inline constexpr uint8_t kMusicPlugUsageIsochronousStream = 0x00;
inline constexpr uint8_t kMusicPlugUsageAsynchronousStream = 0x01;
inline constexpr uint8_t kMusicPlugUsageMidi = 0x02;
inline constexpr uint8_t kMusicPlugUsageSync = 0x03;
inline constexpr uint8_t kMusicPlugUsageAnalogAudio = 0x04;
inline constexpr uint8_t kMusicPlugUsageDigitalAudio = 0x05;

// ---------------------------------------------------------------------------
// General capability bits of the Music Subunit (TA 2001007 §5.2.1 Tables 5.5 and 5.6)
// ---------------------------------------------------------------------------

inline constexpr uint8_t kMusicCapabilityNonBlockingBit = 0x01;
inline constexpr uint8_t kMusicCapabilityBlockingBit = 0x02;

// ---------------------------------------------------------------------------
// Audio Subunit object lists (TA 1999008)
// ---------------------------------------------------------------------------

inline constexpr uint8_t kAudioListTypeTextDatabase = 0x86;        ///< Table 7.1
inline constexpr uint8_t kAudioEntryTypeChildDirectory = 0x90;     ///< Table 6.1
inline constexpr uint8_t kAudioEntryTypeTextDatabase = 0x93;       ///< Table 6.1

// ---------------------------------------------------------------------------
// List and entry descriptor attribute bits (TA 2002013 Tables 11 and 12)
// ---------------------------------------------------------------------------

inline constexpr uint8_t kAttributeHasMoreAttributes = 0x80;  ///< both tables; must be 0 in this version
inline constexpr uint8_t kAttributeSkip = 0x40;               ///< both tables
inline constexpr uint8_t kListAttributeEntriesHaveObjectId = 0x10;  ///< Table 11
inline constexpr uint8_t kEntryAttributeHasChildId = 0x20;          ///< Table 12
inline constexpr uint8_t kAttributeUpToDate = 0x08;                 ///< both tables

} // namespace ASFW::Protocols::AVC::Descriptors
