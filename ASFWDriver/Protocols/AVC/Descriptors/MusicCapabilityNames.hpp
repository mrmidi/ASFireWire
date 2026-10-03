// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicCapabilityNames.hpp - Spec names for the Music subunit's capability and activity bit fields (TA 2001007
// Tables 5.4-5.6, 5.8, 5.10-5.12, 6.6-6.8), as plain names with no raw value. The discovery log appends the raw
// value ("blocking(0x02)"); the discovery document carries the name next to the number.

#pragma once

#include "DescriptorTypeCodes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Descriptors {

/// transmit_capability / receive_capability (Tables 5.5, 5.6): "non-blocking", "blocking", both joined by "+", or "none".
[[nodiscard]] inline std::string TransferCapabilityName(uint8_t bits) {
    std::string text;
    if (bits & kMusicCapabilityNonBlockingBit) text += "non-blocking";
    if (bits & kMusicCapabilityBlockingBit) text += std::string(text.empty() ? "" : "+") + "blocking";
    return text.empty() ? "none" : text;
}

/// An activity or capability byte of SMPTE time code, sample count or audio SYNC (Tables 5.10-5.12, 6.6-6.8): bit 0 and
/// bit 1 by the names given, "none" for neither, and any other set bit as UNKNOWN(bit:N), so a reserved bit is shown.
[[nodiscard]] inline std::string ActivityName(uint8_t bits, const std::string& bit0, const std::string& bit1) {
    std::string text;
    const auto add = [&text](const std::string& name) { text += (text.empty() ? "" : "+") + name; };
    if (bits & kMusicCapabilityRxBit) add(bit0);
    if (bits & kMusicCapabilityTxBit) add(bit1);
    for (unsigned bit = 2; bit < 8; ++bit) {
        if (bits & (1u << bit)) add("UNKNOWN(bit:" + std::to_string(bit) + ")");
    }
    return text.empty() ? "none" : text;
}

/// The AM824 label range of Table 5.8, or nullopt for a reserved label.
[[nodiscard]] inline std::optional<std::string> Am824LabelName(uint8_t label) {
    const auto in = [label](unsigned low, unsigned high) { return label >= low && label <= high; };
    if (in(0x00, 0x3F)) return "IEC 60958 conformant";
    if (in(0x40, 0x4F)) return "multi-bit linear audio";
    if (in(0x50, 0x57)) return "one bit audio (plain)";
    if (in(0x58, 0x5F)) return "one bit audio (encoded)";
    if (in(0x60, 0x67)) return "high precision multi-bit linear audio";
    if (in(0x80, 0x83)) return "MIDI conformant";
    if (in(0x88, 0x8B)) return "SMPTE time code conformant";
    if (in(0x8C, 0x8F)) return "sample count";
    if (in(0xC0, 0xEF)) return "ancillary data";
    return std::nullopt;
}

/// The fields capability_attributes announces (Table 5.4), in table order.
[[nodiscard]] inline std::vector<std::string> CapabilityAttributeNames(const std::vector<uint8_t>& attributes) {
    std::vector<std::string> names;
    const uint8_t first = attributes.empty() ? 0 : attributes.front();
    if (first & kMusicCapabilityGeneralBit) names.emplace_back("general");
    if (first & kMusicCapabilityAudioBit) names.emplace_back("audio");
    if (first & kMusicCapabilityMidiBit) names.emplace_back("MIDI");
    if (first & kMusicCapabilitySmpteBit) names.emplace_back("SMPTE time code");
    if (first & kMusicCapabilitySampleCountBit) names.emplace_back("sample count");
    if (first & kMusicCapabilityAudioSyncBit) names.emplace_back("audio SYNC");
    constexpr uint8_t kKnownBits = 0x3F;
    for (unsigned bit = 0; bit < 8; ++bit) {
        if ((first & (1u << bit)) && !((kKnownBits | kMusicHasMoreAttributesBit) & (1u << bit))) {
            names.push_back("UNKNOWN(capability_bit:" + std::to_string(bit) + ")");
        }
    }
    return names;
}

/// music_subunit_version / MIDI version: major in the high nibble ("1.0").
[[nodiscard]] inline std::string VersionName(uint8_t version) {
    return std::to_string(version >> 4) + "." + std::to_string(version & 0x0F);
}

} // namespace ASFW::Protocols::AVC::Descriptors
