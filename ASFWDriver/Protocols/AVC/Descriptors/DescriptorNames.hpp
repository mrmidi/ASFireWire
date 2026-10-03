// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DescriptorNames.hpp - Spec names for descriptor, info block and music plug codes, for logs and
// diagnostics. Same contract as Core/AvcNames.hpp: `name(0xNN)` for a value the table knows,
// `UNKNOWN(<table>:0xNN)` for one it does not. Each table cites the table that defines the values;
// the values themselves live in DescriptorTypeCodes.hpp.

#pragma once

#include "AudioSubunitDescriptor.hpp"
#include "DescriptorTypeCodes.hpp"
#include "MusicSubunitDescriptor.hpp"
#include "ParseReader.hpp"
#include "../Core/AvcNames.hpp"

#include <string>

namespace ASFW::Protocols::AVC::Descriptors {

namespace names {

using ::ASFW::AVC::NameEntry;

// TA 1999045 §4.8, §4.9; TA 2001007 §6.2; 8108-810B from Apple's MusicSubunitController.h:68-79.
inline constexpr std::array kInfoBlockTypes{
    NameEntry{kInfoBlockRawText, "raw text"},
    NameEntry{kInfoBlockName, "name"},
    NameEntry{kMusicInfoBlockGeneralStatusArea, "general music subunit status area"},
    NameEntry{kMusicInfoBlockOutputPlugStatusArea, "music output plug status area"},
    NameEntry{kMusicInfoBlockSourcePlugStatus, "source plug status"},
    NameEntry{kMusicInfoBlockAudioInfo, "audio info"},
    NameEntry{kMusicInfoBlockMidiInfo, "MIDI info"},
    NameEntry{kMusicInfoBlockSmpteTimeCodeInfo, "SMPTE time code info"},
    NameEntry{kMusicInfoBlockSampleCountInfo, "sample count info"},
    NameEntry{kMusicInfoBlockAudioSyncInfo, "audio SYNC info"},
    NameEntry{kMusicInfoBlockRoutingStatus, "routing status"},
    NameEntry{kMusicInfoBlockSubunitPlugInfo, "subunit plug info"},
    NameEntry{kMusicInfoBlockClusterInfo, "cluster info"},
    NameEntry{kMusicInfoBlockMusicPlugInfo, "music plug info"},
};

// Apple MusicSubunitController.h:107-122.
inline constexpr std::array kMusicPortTypes{
    NameEntry{kMusicPortTypeSpeaker, "speaker"},
    NameEntry{kMusicPortTypeHeadphone, "headphone"},
    NameEntry{kMusicPortTypeMicrophone, "microphone"},
    NameEntry{kMusicPortTypeLine, "line"},
    NameEntry{kMusicPortTypeSpdif, "S/PDIF"},
    NameEntry{kMusicPortTypeAdat, "ADAT"},
    NameEntry{kMusicPortTypeTdif, "TDIF"},
    NameEntry{kMusicPortTypeMadi, "MADI"},
    NameEntry{kMusicPortTypeAnalog, "analog"},
    NameEntry{kMusicPortTypeDigital, "digital"},
    NameEntry{kMusicPortTypeMidi, "MIDI"},
    NameEntry{kMusicPortTypeAesEbu, "AES/EBU"},
    NameEntry{kMusicPortTypeNone, "no type"},
};

// Apple MusicSubunitController.h:148-165; field order from FFADO avc_descriptor_music.cpp:376-470.
inline constexpr std::array kMusicPlugTypes{
    NameEntry{kMusicPlugTypeAudio, "audio"},
    NameEntry{kMusicPlugTypeMidi, "MIDI"},
    NameEntry{kMusicPlugTypeSmpte, "SMPTE time code"},
    NameEntry{kMusicPlugTypeSampleCount, "sample count"},
    NameEntry{kMusicPlugTypeSync, "audio SYNC"},  // TA 2001007 Table 7.3
};
inline constexpr std::array kMusicRoutingSupports{
    NameEntry{kMusicRoutingSupportFixed, "fixed"},
    NameEntry{kMusicRoutingSupportCluster, "cluster"},
    NameEntry{kMusicRoutingSupportFlexible, "flexible"},
    NameEntry{kMusicRoutingSupportUnknown, "unknown"},
};

// Apple MusicSubunitController.h:96-105.
inline constexpr std::array kMusicPlugUsages{
    NameEntry{kMusicPlugUsageIsochronousStream, "isochronous stream"},
    NameEntry{kMusicPlugUsageAsynchronousStream, "asynchronous stream"},
    NameEntry{kMusicPlugUsageMidi, "MIDI"},
    NameEntry{kMusicPlugUsageSync, "sync"},
    NameEntry{kMusicPlugUsageAnalogAudio, "analog audio"},
    NameEntry{kMusicPlugUsageDigitalAudio, "digital audio"},
};

// TA 1999008 Table 6.1 (entry types), Table 7.1 (list type).
inline constexpr std::array kAudioListTypes{
    NameEntry{kAudioListTypeTextDatabase, "text database list"},
};
inline constexpr std::array kAudioEntryTypes{
    NameEntry{kAudioEntryTypeChildDirectory, "child directory object"},
    NameEntry{kAudioEntryTypeTextDatabase, "text database object"},
};

// TA 1999008 Table 10.2 (the descriptor's copy of the function block types).
inline constexpr std::array kAudioFunctionBlockTypes{
    NameEntry{static_cast<uint32_t>(AudioFunctionBlockType::kSelector), "Selector"},
    NameEntry{static_cast<uint32_t>(AudioFunctionBlockType::kFeature), "Feature"},
    NameEntry{static_cast<uint32_t>(AudioFunctionBlockType::kProcessing), "Processing"},
    NameEntry{static_cast<uint32_t>(AudioFunctionBlockType::kCodec), "CODEC"},
};

inline constexpr std::array kParseErrorKinds{
    NameEntry{static_cast<uint32_t>(ParseErrorKind::Truncated), "truncated"},
    NameEntry{static_cast<uint32_t>(ParseErrorKind::InvalidLength), "invalid length"},
    NameEntry{static_cast<uint32_t>(ParseErrorKind::InvalidValue), "invalid value"},
    NameEntry{static_cast<uint32_t>(ParseErrorKind::BudgetExceeded), "budget exceeded"},
};

// The function type of a music plug endpoint (music plug info 810B): F0 subunit destination plug,
// F1 subunit source plug (FFADO avc_descriptor_music.cpp:451-461); FF is "not set".
inline constexpr std::array kMusicEndpointFunctionTypes{
    NameEntry{MusicPlugEndpoint::kSubunitDestinationPlug, "subunit destination plug"},
    NameEntry{MusicPlugEndpoint::kSubunitSourcePlug, "subunit source plug"},
    NameEntry{MusicPlugEndpoint::kUnset, "not set"},
};

} // namespace names

[[nodiscard]] inline std::string DescribeParseError(const ParseError& error) {
    return ::ASFW::AVC::DescribeValue(names::kParseErrorKinds, "parse_error", static_cast<uint32_t>(error.kind), 2) +
           " at offset " + std::to_string(error.offset);
}
/// 0xFF in an endpoint field is the wire's "no value".
[[nodiscard]] inline std::string EndpointField(uint8_t value) {
    return value == MusicPlugEndpoint::kUnset ? std::string("none") : std::to_string(value);
}
[[nodiscard]] inline std::string DescribeEndpoint(const MusicPlugEndpoint& e) {
    return ::ASFW::AVC::DescribeValue(names::kMusicEndpointFunctionTypes, "endpoint_function_type", e.functionType, 2) +
           " plug=" + EndpointField(e.plugId) + " fb=" + EndpointField(e.functionBlockId) + " position=" +
           EndpointField(e.streamPosition) + " location=" + EndpointField(e.streamLocation);
}

[[nodiscard]] inline std::string DescribeInfoBlockType(uint16_t type) {
    return ::ASFW::AVC::DescribeValue(names::kInfoBlockTypes, "info_block_type", type, 4);
}
[[nodiscard]] inline std::string DescribeMusicPortType(uint8_t type) {
    return ::ASFW::AVC::DescribeValue(names::kMusicPortTypes, "music_port_type", type, 2);
}
[[nodiscard]] inline std::string DescribeMusicPlugType(uint8_t type) {
    return ::ASFW::AVC::DescribeValue(names::kMusicPlugTypes, "music_plug_type", type, 2);
}
[[nodiscard]] inline std::string DescribeMusicRoutingSupport(uint8_t support) {
    return ::ASFW::AVC::DescribeValue(names::kMusicRoutingSupports, "routing_support", support, 2);
}
[[nodiscard]] inline std::string DescribeMusicPlugUsage(uint8_t usage) {
    return ::ASFW::AVC::DescribeValue(names::kMusicPlugUsages, "music_plug_usage", usage, 2);
}
[[nodiscard]] inline std::string DescribeAudioListType(uint8_t type) {
    return ::ASFW::AVC::DescribeValue(names::kAudioListTypes, "audio_list_type", type, 2);
}
[[nodiscard]] inline std::string DescribeAudioEntryType(uint8_t type) {
    return ::ASFW::AVC::DescribeValue(names::kAudioEntryTypes, "audio_entry_type", type, 2);
}
[[nodiscard]] inline std::string Describe(AudioFunctionBlockType type) {
    return ::ASFW::AVC::DescribeValue(names::kAudioFunctionBlockTypes, "function_block_type",
                                      static_cast<uint32_t>(type), 2);
}

/// "not connected", "subunit destination plug 3" or "Feature function block 4", with the bytes as sent
/// (TA 1999008 Tables 8.2, 9.1).
[[nodiscard]] inline std::string Describe(const AudioSourceId& source) {
    std::string text;
    if (!source.IsConnected()) {
        text = "not connected";
    } else if (source.IsSubunitDestPlug()) {
        text = "subunit destination plug " + std::to_string(source.id);
    } else if (source.IsFunctionBlock()) {
        text = Describe(static_cast<AudioFunctionBlockType>(source.type)) + " function block " + std::to_string(source.id);
    } else {
        text = "UNKNOWN(source_id_type:" + ::ASFW::AVC::Hex(source.type) + ") " + std::to_string(source.id);
    }
    const std::array<uint8_t, 2> bytes{source.type, source.id};
    return text + " [" + ::ASFW::AVC::HexBytes(bytes) + "]";
}

} // namespace ASFW::Protocols::AVC::Descriptors
