// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioSubunitDescriptor.hpp - AV/C Audio Subunit Descriptor Parser
// Specifications:
//   - TA Document 1999008: AV/C Audio Subunit Specification 1.0 (Clauses 6, 7, 8)
//   - TA Document 2002013: AV/C Descriptor Mechanism 1.2
// References:
//   - Apple AppleFWAudio graph rules & discovery
//   - FFADO libavc/descriptors/audiosubunit/avc_descriptor_audio.cpp
//

#pragma once

#include "ParseReader.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ASFW::Protocols::AVC::Descriptors {

//==============================================================================
// Function Block Types (Audio Subunit 1.0 §8)
//==============================================================================

enum class AudioFunctionBlockType : uint8_t {
    kSelector   = 0x80,  ///< Selector Function Block (§8.2)
    kFeature    = 0x81,  ///< Feature Function Block (§8.3)
    kProcessing = 0x82,  ///< Processing Function Block (§8.4, e.g. Mixer)
    kCodec      = 0x83,  ///< CODEC Function Block (§8.5)
};

//==============================================================================
// Source ID (Audio Subunit 1.0 §8.1 Table 8.2)
// Describes the input connection of a function block plug or subunit source plug.
//==============================================================================

/// function_block_type values of a source_ID (TA 1999008 Table 8.2, Table 9.1): F0 = subunit
/// destination plug, F1 = subunit source plug, 80..8F = audio subunit function blocks, FE = not connected.
inline constexpr uint8_t kSourceIdSubunitDestinationPlug = 0xF0;
inline constexpr uint8_t kSourceIdNotConnected = 0xFE;
inline constexpr uint8_t kFunctionBlockTypeClassMask = 0xF0;  ///< high nibble of an audio function block type
inline constexpr uint8_t kFunctionBlockTypeClass = 0x80;      ///< 80..8F: audio subunit dependent (Table 9.1)
/// Our "no id yet" marker. FF is reserved for extension in the spec (§9.1.3), so it never names a block.
inline constexpr uint8_t kUnsetSourceId = 0xFF;
/// Our "no name" marker for a text database object position.
inline constexpr uint16_t kNoNameIndex = 0xFFFF;

struct AudioSourceId {
    uint8_t type{kSourceIdNotConnected};  ///< kSourceIdSubunitDestinationPlug, 0x80..0x83 function block, or not connected
    uint8_t id{kUnsetSourceId};           ///< Destination plug number or Function Block ID

    [[nodiscard]] constexpr bool IsSubunitDestPlug() const noexcept { return type == kSourceIdSubunitDestinationPlug; }
    [[nodiscard]] constexpr bool IsFunctionBlock() const noexcept {
        return (type & kFunctionBlockTypeClassMask) == kFunctionBlockTypeClass;
    }
    [[nodiscard]] constexpr bool IsConnected() const noexcept { return type != kSourceIdNotConnected; }

    [[nodiscard]] constexpr bool operator==(const AudioSourceId& other) const noexcept {
        return type == other.type && id == other.id;
    }
};

//==============================================================================
// Feature Block Controls (Audio Subunit 1.0 §8.3 Table 8.3)
//==============================================================================

namespace FeatureControlMask {
    constexpr uint16_t kMute            = 0x0001;  // Bit 0: Mute Control
    constexpr uint16_t kVolume          = 0x0002;  // Bit 1: Volume Control
    constexpr uint16_t kLRBalance       = 0x0004;  // Bit 2: LR Balance Control
    constexpr uint16_t kFRBalance       = 0x0008;  // Bit 3: FR Balance Control
    constexpr uint16_t kBass            = 0x0010;  // Bit 4: Bass Control
    constexpr uint16_t kMid             = 0x0020;  // Bit 5: Mid Control
    constexpr uint16_t kTreble          = 0x0040;  // Bit 6: Treble Control
    constexpr uint16_t kGraphicEQ       = 0x0080;  // Bit 7: Graphic Equalizer
    constexpr uint16_t kAGC             = 0x0100;  // Bit 8: Automatic Gain Control
    constexpr uint16_t kDelay           = 0x0200;  // Bit 9: Delay Control
    constexpr uint16_t kBassBoost       = 0x0400;  // Bit 10: Bass Boost
    constexpr uint16_t kLoudness        = 0x0800;  // Bit 11: Loudness Control
}

//==============================================================================
// Function Block Information
//==============================================================================

struct AudioFunctionBlockInfo {
    AudioFunctionBlockType type{AudioFunctionBlockType::kFeature};
    uint8_t id{0};
    uint16_t nameIndex{kNoNameIndex};     ///< Text DB object position or kNoNameIndex if none
    std::string name;                     ///< Resolved from text DB if available

    std::vector<AudioSourceId> inputSources; ///< Upstream sources for input fb-plugs (p elements)

    // Feature Block specifics
    uint8_t clusterChannels{0};           ///< Number of channels from cluster info
    uint8_t generalTag{0};                ///< Volume purpose (0: gen, 1: master, 2: in trim, 3: out trim)
    uint16_t masterControls{0};           ///< Channel 0 control bitmap
    std::vector<uint16_t> channelControls; ///< Logical channels 1..ch control bitmaps

    // Processing Block specifics (e.g. Mixer)
    uint8_t processType{0};
};

//==============================================================================
// Audio Subunit Identifier Descriptor (§5.1, §8.1)
//==============================================================================

struct AudioSubunitIdentifier {
    uint8_t generationId{0};
    uint8_t sizeOfListId{2};
    uint8_t sizeOfObjectId{0};
    uint8_t sizeOfObjectPosition{2};

    std::vector<uint16_t> rootListIds;
    std::vector<AudioSourceId> sourcePlugLinks;  ///< Subunit source plug connections
    std::vector<AudioFunctionBlockInfo> functionBlocks;

    /// Look up a function block by type and ID
    [[nodiscard]] constexpr const AudioFunctionBlockInfo* FindBlock(AudioFunctionBlockType type, uint8_t id) const noexcept {
        for (const auto& fb : functionBlocks) {
            if (fb.type == type && fb.id == id) return &fb;
        }
        return nullptr;
    }
};

//==============================================================================
// Text Database Dictionary (§7.2, §7.3)
// Maps object_position (16-bit) -> text string (minimal English ASCII)
//==============================================================================

using TextDatabase = std::unordered_map<uint16_t, std::string>;

//==============================================================================
// Parser Interface
//==============================================================================

class AudioSubunitDescriptorParser {
public:
    /// Parse the Audio Subunit Identifier Descriptor (specifier 0x00).
    /// @param data Raw descriptor bytes starting from header (excluding FCP/AV/C headers)
    /// constexpr: captured fixtures are parsed in static_asserts.
    [[nodiscard]] static constexpr Parsed<AudioSubunitIdentifier> ParseIdentifierDescriptor(
        std::span<const uint8_t> data) noexcept;

    /// Parse a Text Database List Descriptor (specifier 0x10 <list_id:2>, e.g. 0x1801).
    /// Extracts mapping from object_position (index) to name string.
    [[nodiscard]] static TextDatabase ParseTextDatabaseList(
        std::span<const uint8_t> data) noexcept;
    [[nodiscard]] static Parsed<TextDatabase> ParseTextDatabaseListChecked(
        std::span<const uint8_t> data) noexcept;

    /// Return child list IDs from a structurally valid list descriptor.
    [[nodiscard]] static Parsed<std::vector<uint16_t>> ParseChildListIds(
        std::span<const uint8_t> data, uint8_t listIdSize = 2,
        uint8_t objectIdSize = 0) noexcept;

    /// Resolve and populate function block names in an identifier descriptor using a text DB.
    static void ResolveNames(AudioSubunitIdentifier& identifier, const TextDatabase& textDb) noexcept;
};


//==============================================================================
// Identifier descriptor parsing (Audio Subunit 1.0 §5.1, §8.1; Descriptor 1.2)
// Every length is a bounded Section(); the pipeline stops at the first error.
//==============================================================================

namespace AudioIdentifierParse {
[[nodiscard]] constexpr Parsed<void> Fail(size_t offset, ParseErrorKind kind) {
    return std::unexpected(ParseError{offset, kind});
}
[[nodiscard]] constexpr Parsed<void> Sources(ParseReader& reader, size_t count, std::vector<AudioSourceId>& out) {
    for (size_t i = 0; i < count; ++i) {
        AudioSourceId source{};
        if (auto read = reader.Fields(source.type, source.id); !read) return read;
        out.push_back(source);
    }
    return {};
}
[[nodiscard]] constexpr Parsed<void> ListIds(ParseReader& reader, size_t count, std::vector<uint16_t>& out) {
    for (size_t i = 0; i < count; ++i) {
        auto id = reader.BE16();
        if (!id) return std::unexpected(id.error());
        out.push_back(*id);
    }
    return {};
}
/// Master then per-channel control bitmaps, each `width` bytes (1 or 2).
[[nodiscard]] constexpr Parsed<void> Controls(ParseReader& fields, uint16_t width, AudioFunctionBlockInfo& block) {
    const auto read = [&fields, width]() -> Parsed<uint16_t> {
        if (width == 1) return fields.U8().transform([](uint8_t value) { return static_cast<uint16_t>(value); });
        return fields.BE16();
    };
    return read().and_then([&](uint16_t master) -> Parsed<void> {
        block.masterControls = master;
        while (fields.Remaining()) {
            auto channel = read();
            if (!channel) return std::unexpected(channel.error());
            block.channelControls.push_back(*channel);
        }
        return {};
    });
}
/// Feature function block specific fields (Audio Subunit Table 8.3, local
/// spec text:2304-2340). The Duet uses one-byte length/width fields; only its
/// exact captured 08 02 00 layout is accepted for that form.
[[nodiscard]] constexpr Parsed<void> Feature(ParseReader reader, AudioFunctionBlockInfo& block) {
    const auto base = reader.Offset();
    return reader.Take(reader.Remaining()).and_then([&](std::span<const uint8_t> bytes) -> Parsed<void> {
        ParseReader fields(bytes, base);
        if (bytes.size() == 9 && bytes[0] == 8 && bytes[1] == 2 && bytes[2] == 0) {
            uint8_t shortLength{}, width{};
            return fields.Fields(shortLength, width, block.generalTag)
                .and_then([&] { return Controls(fields, 2, block); });
        }
        return fields.Section().and_then([&](ParseReader specific) {
            return fields.End().and_then([&]() -> Parsed<void> {
                const auto widthAt = specific.Offset();
                return specific.BE16().and_then([&](uint16_t width) -> Parsed<void> {
                    if (width != 1 && width != 2) return Fail(widthAt, ParseErrorKind::InvalidValue);
                    return specific.Fields(block.generalTag).and_then([&] { return Controls(specific, width, block); });
                });
            });
        });
    });
}
[[nodiscard]] constexpr Parsed<AudioFunctionBlockInfo> FunctionBlock(ParseReader& configuration) {
    return configuration.Section().and_then([](ParseReader reader) -> Parsed<AudioFunctionBlockInfo> {
        AudioFunctionBlockInfo block;
        uint8_t type{}, inputs{};
        return reader.Fields(type, block.id, block.nameIndex, inputs)
            .and_then([&] {
                block.type = static_cast<AudioFunctionBlockType>(type);
                return Sources(reader, inputs, block.inputSources);
            })
            .and_then([&] { return reader.Section(); })
            .and_then([&](ParseReader cluster) -> Parsed<void> {
                if (!cluster.Remaining()) return {};
                return cluster.Fields(block.clusterChannels);
            })
            .and_then([&] { return reader.Section(); })
            .and_then([&](ParseReader dependent) -> Parsed<void> {
                // Bytes past the known fields are reserved/device-specific; their
                // bounds were already checked by Section().
                if (!dependent.Remaining()) return {};
                if (block.type == AudioFunctionBlockType::kFeature) return Feature(dependent, block);
                if (block.type == AudioFunctionBlockType::kProcessing) return dependent.Fields(block.processType);
                return {};
            })
            .transform([&] { return std::move(block); });
    });
}
/// Audio subunit information: name, cluster, source plug links, function blocks.
[[nodiscard]] constexpr Parsed<void> Information(ParseReader info, AudioSubunitIdentifier& identifier) {
    uint16_t name{};
    uint8_t sourceCount{}, blockCount{};
    return info.Fields(name)
        .and_then([&] { return info.Section(); })
        .and_then([&](ParseReader) { return info.Fields(sourceCount); })
        .and_then([&] { return Sources(info, sourceCount, identifier.sourcePlugLinks); })
        .and_then([&] { return info.Fields(blockCount); })
        .and_then([&]() -> Parsed<void> {
            for (size_t i = 0; i < blockCount; ++i) {
                auto block = FunctionBlock(info);
                if (!block) return std::unexpected(block.error());
                identifier.functionBlocks.push_back(std::move(*block));
            }
            return {};
        });
}
} // namespace AudioIdentifierParse

constexpr Parsed<AudioSubunitIdentifier> AudioSubunitDescriptorParser::ParseIdentifierDescriptor(
    std::span<const uint8_t> data) noexcept {
    using namespace AudioIdentifierParse;
    return DescriptorBody(data).and_then([](ParseReader reader) -> Parsed<AudioSubunitIdentifier> {
        AudioSubunitIdentifier identifier;
        uint16_t rootCount{};
        return reader.Fields(identifier.generationId, identifier.sizeOfListId,
                             identifier.sizeOfObjectId, identifier.sizeOfObjectPosition, rootCount)
            .and_then([&]() -> Parsed<void> {
                if (rootCount && identifier.sizeOfListId != 2) return Fail(3, ParseErrorKind::InvalidValue);
                return ListIds(reader, rootCount, identifier.rootListIds);
            })
            .and_then([&] { return reader.Section(); })                               // dependent info
            .and_then([](ParseReader dependent) { return dependent.Section(); })      // configuration
            .and_then([&](ParseReader configuration) {
                uint16_t configurationId{};
                return configuration.Fields(configurationId)
                    .and_then([&] { return configuration.Section(); })
                    .and_then([&](ParseReader info) { return Information(info, identifier); });
            })
            .transform([&] { return std::move(identifier); });
    });
}
} // namespace ASFW::Protocols::AVC::Descriptors

namespace ASFW::AVC::Descriptors {
using namespace ASFW::Protocols::AVC::Descriptors;
} // namespace ASFW::AVC::Descriptors
