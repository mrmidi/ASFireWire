// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicSubunitIdentifier.hpp - The Music Subunit identifier descriptor (specifier 0x00).
//
// Specification: TA Document 2001007, AV/C Music Subunit 1.0, §5 (Figures 5.1-5.17, Tables
// 5.1-5.12); the generic descriptor frame is TA 2002013 §5. This is the STATIC description of
// what the subunit can do. The dynamic one is the status descriptor (specifier 0x80,
// MusicSubunitDescriptor.hpp), whose info blocks 8100-8107 reuse the same numbers for
// different things.
//
// Seen on one device: a TerraTec Phase 88 answered OPEN / READ / CLOSE of specifier 00 on 2026-10-03 with this
// descriptor (documentation/avc-rebuild/fixtures/phase88_music_identifier.json), and the parser reads it with every
// length adding up. No reference stack (Linux, FFADO, Apple IOFireWire / AVCVideoServices) reads it. Discovery reads
// it once per Music subunit, after that subunit's status descriptor was read (DiscoveryReducer.cpp); a device that
// refuses or fails it costs the capabilities only.

// Layout (nesting from Figures 5.1 and 5.2, read the way the Audio subunit's is):
//   descriptor_length (2) | generation_ID | size_of_list_ID | size_of_object_ID |
//   size_of_object_position | number_of_root_object_lists (2) | root_object_list_id[n] |
//   type_dependent_information_length (2) { dependent_info_fields_length (2) {
//       attributes (chained) | music_subunit_version |
//       specific_information_length (2) { capability_attributes (chained) |
//           general | audio | MIDI | SMPTE | sample count | audio SYNC capability
//           (each present when its bit is set, each led by a one-byte length) }
//       optional info blocks for future expansion } } |
//   manufacture_dependent_information_length (2) | manufacture_dependent_information
//
// Reading is lenient where the spec reserves room: a capability field longer than its known
// layout is accepted (the extra bytes are bounded and ignored); a shorter one is an error.

#pragma once

#include "DescriptorTypeCodes.hpp"
#include "ParseReader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Protocols::AVC::Descriptors {

/// general_capability (Figure 5.5): how the subunit can transmit and receive.
struct MusicGeneralCapability {
    uint8_t transmit{0};  ///< transmit_capability (Table 5.5): kMusicCapabilityNonBlockingBit / BlockingBit
    uint8_t receive{0};   ///< receive_capability (Table 5.6)
    uint32_t latency{kMusicLatencyNotSpecified};
};

/// One available_audio_format_info (Figure 5.8).
struct MusicAudioFormatInfo {
    uint16_t maxInputChannels{0};
    uint16_t maxOutputChannels{0};
    uint8_t fdf{0};         ///< FDF (Figure 5.9)
    uint8_t am824Label{0};  ///< AM824 LABEL (Table 5.8); 00 unless the FDF is 00 (Table 5.7)
};

/// MIDI_capability_information (Figure 5.11).
struct MusicMidiCapabilityInfo {
    uint8_t version{0};
    uint8_t revision{0};
    uint8_t adaptationLayerVersion{0};  ///< Table 5.9
    uint16_t maxInputPorts{0};
    uint16_t maxOutputPorts{0};
};

struct MusicSubunitIdentifier {
    // Identifier header (Figure 5.1, Table 5.1).
    uint8_t generationId{0};
    uint8_t sizeOfListId{0};
    uint8_t sizeOfObjectId{0};
    uint8_t sizeOfObjectPosition{0};
    std::vector<uint64_t> rootListIds;

    // Music subunit dependent information (Figure 5.2).
    std::vector<uint8_t> attributes;  ///< attributes chain (Table 5.2); the first has no defined bits but 7
    uint8_t version{0};               ///< music_subunit_version (Table 5.3)
    size_t optionalInfoBytes{0};      ///< bytes of "optional info blocks for future expansion", not read

    // Music subunit specific information (Figure 5.3).
    std::vector<uint8_t> capabilityAttributes;  ///< capability_attributes chain (Table 5.4)
    std::optional<MusicGeneralCapability> general;
    std::optional<std::vector<MusicAudioFormatInfo>> audio;
    std::optional<MusicMidiCapabilityInfo> midi;
    std::optional<uint8_t> smpteTimeCode;  ///< Tx/Rx bits (Table 5.10)
    std::optional<uint8_t> sampleCount;    ///< Tx/Rx bits (Table 5.11)
    std::optional<uint8_t> audioSync;      ///< Bus/Ex bits (Table 5.12)

    bool hasManufacturerInformation{false};
    size_t manufacturerInformationBytes{0};
};

class MusicSubunitIdentifierParser {
public:
    /// Parse the Music Subunit Identifier Descriptor (specifier 0x00), as DescriptorAccessor returns
    /// it: starting at descriptor_length, without the AV/C or FCP headers.
    [[nodiscard]] static Parsed<MusicSubunitIdentifier> Parse(std::span<const uint8_t> data) noexcept;

private:
    static constexpr size_t kMaxListIdBytes = sizeof(uint64_t);

    [[nodiscard]] static Parsed<std::vector<uint8_t>> AttributeChain(ParseReader& reader);
    [[nodiscard]] static Parsed<ParseReader> LengthPrefixed(ParseReader& reader);
    [[nodiscard]] static Parsed<void> SpecificInformation(ParseReader specific, MusicSubunitIdentifier& out);
};

// ---------------------------------------------------------------------------
// Implementation (header-only, like the Audio subunit identifier parser)
// ---------------------------------------------------------------------------

inline Parsed<std::vector<uint8_t>> MusicSubunitIdentifierParser::AttributeChain(ParseReader& reader) {
    std::vector<uint8_t> chain;
    for (;;) {
        auto byte = reader.U8();
        if (!byte) return std::unexpected(byte.error());
        chain.push_back(*byte);
        if (!(*byte & kMusicHasMoreAttributesBit)) return chain;
    }
}

inline Parsed<ParseReader> MusicSubunitIdentifierParser::LengthPrefixed(ParseReader& reader) {
    auto length = reader.U8();
    if (!length) return std::unexpected(length.error());
    const auto base = reader.Offset();
    auto bytes = reader.Take(*length);
    if (!bytes) return std::unexpected(bytes.error());
    return ParseReader(*bytes, base);
}

inline Parsed<void> MusicSubunitIdentifierParser::SpecificInformation(ParseReader specific,
                                                                      MusicSubunitIdentifier& out) {
    auto chain = AttributeChain(specific);
    if (!chain) return std::unexpected(chain.error());
    out.capabilityAttributes = std::move(*chain);
    // The first byte names the fields that follow; later bytes are reserved (Table 5.4).
    const uint8_t present = out.capabilityAttributes.front();

    if (present & kMusicCapabilityGeneralBit) {
        auto field = LengthPrefixed(specific);
        if (!field) return std::unexpected(field.error());
        MusicGeneralCapability general;
        if (auto read = field->Fields(general.transmit, general.receive, general.latency); !read) return read;
        out.general = general;
    }
    if (present & kMusicCapabilityAudioBit) {
        auto field = LengthPrefixed(specific);
        if (!field) return std::unexpected(field.error());
        uint8_t count{};
        if (auto read = field->Fields(count); !read) return read;
        std::vector<MusicAudioFormatInfo> formats;
        for (size_t i = 0; i < count; ++i) {
            MusicAudioFormatInfo format;
            if (auto read = field->Fields(format.maxInputChannels, format.maxOutputChannels, format.fdf,
                                          format.am824Label);
                !read) {
                return read;
            }
            formats.push_back(format);
        }
        out.audio = std::move(formats);
    }
    if (present & kMusicCapabilityMidiBit) {
        auto field = LengthPrefixed(specific);
        if (!field) return std::unexpected(field.error());
        uint8_t versionRevision{};
        MusicMidiCapabilityInfo midi;
        if (auto read = field->Fields(versionRevision, midi.adaptationLayerVersion, midi.maxInputPorts,
                                      midi.maxOutputPorts);
            !read) {
            return read;
        }
        constexpr unsigned kMidiVersionShift = 4;  // MIDI_version is the high nibble (Figure 5.11)
        constexpr uint8_t kMidiRevisionMask = 0x0F;
        midi.version = static_cast<uint8_t>(versionRevision >> kMidiVersionShift);
        midi.revision = static_cast<uint8_t>(versionRevision & kMidiRevisionMask);
        out.midi = midi;
    }
    const auto flagsField = [&specific](uint8_t bit, std::optional<uint8_t>& slot, uint8_t present) -> Parsed<void> {
        if (!(present & bit)) return {};
        auto field = LengthPrefixed(specific);
        if (!field) return std::unexpected(field.error());
        uint8_t flags{};
        if (auto read = field->Fields(flags); !read) return read;
        slot = flags;
        return {};
    };
    if (auto read = flagsField(kMusicCapabilitySmpteBit, out.smpteTimeCode, present); !read) return read;
    if (auto read = flagsField(kMusicCapabilitySampleCountBit, out.sampleCount, present); !read) return read;
    return flagsField(kMusicCapabilityAudioSyncBit, out.audioSync, present);
}

inline Parsed<MusicSubunitIdentifier> MusicSubunitIdentifierParser::Parse(std::span<const uint8_t> data) noexcept {
    auto body = DescriptorBody(data);
    if (!body) return std::unexpected(body.error());
    ParseReader& reader = *body;

    MusicSubunitIdentifier id;
    uint16_t rootCount{};
    if (auto read = reader.Fields(id.generationId, id.sizeOfListId, id.sizeOfObjectId, id.sizeOfObjectPosition,
                                  rootCount);
        !read) {
        return std::unexpected(read.error());
    }
    // size_of_list_ID is the byte width of each root list ID; offset 3 is its position (Figure 5.1).
    constexpr size_t kSizeOfListIdOffset = 3;
    if (id.sizeOfListId > kMaxListIdBytes || (rootCount != 0 && id.sizeOfListId == 0)) {
        return std::unexpected(ParseError{kSizeOfListIdOffset, ParseErrorKind::InvalidValue});
    }
    for (size_t i = 0; i < rootCount; ++i) {
        auto bytes = reader.Take(id.sizeOfListId);
        if (!bytes) return std::unexpected(bytes.error());
        uint64_t listId = 0;
        for (const uint8_t b : *bytes) listId = (listId << 8) | b;
        id.rootListIds.push_back(listId);
    }

    auto dependent = reader.Section();  // music_subunit_type_dependent_information
    if (!dependent) return std::unexpected(dependent.error());
    auto fields = dependent->Section();  // music_subunit_dependent_info_fields
    if (!fields) return std::unexpected(fields.error());

    auto attributes = AttributeChain(*fields);
    if (!attributes) return std::unexpected(attributes.error());
    id.attributes = std::move(*attributes);
    if (auto read = fields->Fields(id.version); !read) return std::unexpected(read.error());
    auto specific = fields->Section();  // music_subunit_specific_information
    if (!specific) return std::unexpected(specific.error());
    if (auto read = SpecificInformation(*specific, id); !read) return std::unexpected(read.error());
    id.optionalInfoBytes = fields->Remaining();

    if (reader.Remaining() != 0) {  // manufacture_dependent_information
        auto manufacturer = reader.Section();
        if (!manufacturer) return std::unexpected(manufacturer.error());
        id.hasManufacturerInformation = true;
        id.manufacturerInformationBytes = manufacturer->Remaining();
    }
    return id;
}

} // namespace ASFW::Protocols::AVC::Descriptors
