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

struct AudioSourceId {
    uint8_t type{0xFE};  ///< 0xF0 = Subunit Destination Plug, 0x80..0x83 = Function Block, 0xFE = Not Connected
    uint8_t id{0xFF};    ///< Destination plug number or Function Block ID

    [[nodiscard]] constexpr bool IsSubunitDestPlug() const noexcept { return type == 0xF0; }
    [[nodiscard]] constexpr bool IsFunctionBlock() const noexcept { return (type & 0xF0) == 0x80; }
    [[nodiscard]] constexpr bool IsConnected() const noexcept { return type != 0xFE; }

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
    uint16_t nameIndex{0xFFFF};           ///< Text DB object position or 0xFFFF if none
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
    [[nodiscard]] const AudioFunctionBlockInfo* FindBlock(AudioFunctionBlockType type, uint8_t id) const noexcept {
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
    [[nodiscard]] static Parsed<AudioSubunitIdentifier> ParseIdentifierDescriptor(
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

} // namespace ASFW::Protocols::AVC::Descriptors

namespace ASFW::AVC::Descriptors {
using namespace ASFW::Protocols::AVC::Descriptors;
} // namespace ASFW::AVC::Descriptors
