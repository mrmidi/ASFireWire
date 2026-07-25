// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeVendorCodec.hpp - Apogee's vendor command table and operand encoding.
//
// The generic VENDOR-DEPENDENT prefix lives in Oxford/OxfwVendorDependent.hpp.
// What is here is Apogee's alone: the 21 command codes, the two argument bytes
// it always emits after the code, and the per-code argument shape.
//
// Reference: snd-firewire-ctl-services protocols/oxfw/src/apogee.rs (read as a
// behavioural source; no code copied).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ApogeeCaps.hpp"
#include "../OxfwVendorDependent.hpp"

namespace ASFW::Audio::Oxford::Apogee {

/// Apogee follows the shared prefix with two argument bytes, so its header is
/// wider than the OXFW-common one. Tascam, by contrast, puts a single operand
/// straight after the code - which is why this constant is here and not in the
/// framing template.
inline constexpr size_t kApogeeArgCount = 2;
inline constexpr size_t kApogeeHeaderSize = kVendorPrefixSize + kApogeeArgCount;

/// Argument-slot fillers. 0xFF means "no argument in this slot"; 0x80 marks a
/// channel/element specifier whose value follows in the second slot.
inline constexpr uint8_t kApogeeArgDefault = 0xFF;
inline constexpr uint8_t kApogeeArgIndexed = 0x80;

/// Mixer source index -> the packed nibble pair Apogee expects (apogee.rs).
[[nodiscard]] constexpr uint8_t EncodeMixerSource(uint8_t source) noexcept {
    return static_cast<uint8_t>(((source / 2U) << 4U) | (source % 2U));
}

/// One Apogee vendor command: the code plus whichever of the value fields that
/// code uses. Kept as a single struct rather than a variant because it is also
/// the carrier the params serdes fills in (see FW-128).
struct ApogeeVendorCommand {
    enum class Code : uint8_t {
        MicPolarity = 0x00,
        XlrIsMicLevel = 0x01,
        XlrIsConsumerLevel = 0x02,
        MicPhantom = 0x03,
        OutIsConsumerLevel = 0x04,
        InGain = 0x05,
        HwState = 0x07,
        OutMute = 0x09,
        InputSourceIsPhone = 0x0C,
        MixerSrc = 0x10,
        OutSourceIsMixer = 0x11,
        DisplayOverholdTwoSec = 0x13,
        DisplayClear = 0x14,
        OutVolume = 0x15,
        MuteForLineOut = 0x16,
        MuteForHpOut = 0x17,
        UnmuteForLineOut = 0x18,
        UnmuteForHpOut = 0x19,
        DisplayIsInput = 0x1B,
        InClickless = 0x1E,
        DisplayFollowToKnob = 0x22,
    };

    Code code{};
    uint8_t index{0};
    uint8_t index2{0};
    bool boolValue{false};
    uint8_t u8Value{0};
    uint16_t u16Value{0};
    std::array<uint8_t, 11> hwState{};

    static ApogeeVendorCommand Bool(Code code, bool value);
    static ApogeeVendorCommand IndexedBool(Code code, uint8_t index, bool value);
    static ApogeeVendorCommand InGain(uint8_t index, uint8_t value);
    static ApogeeVendorCommand OutVolume(uint8_t value);
    static ApogeeVendorCommand MixerSrc(uint8_t source, uint8_t destination, uint16_t gain);
    static ApogeeVendorCommand HwState(const std::array<uint8_t, 11>& raw);
    static ApogeeVendorCommand Make(Code code);

    /// OUI + magic + code + the two argument bytes, without the value payload.
    [[nodiscard]] std::vector<uint8_t> BuildOperandBase() const;

    /// Append the value bytes this code carries. Status queries omit these.
    void AppendControlValue(std::vector<uint8_t>& operands) const;

    /// Validate and decode a status response into this command's value fields.
    /// Returns false - never a partially-filled command - on any mismatch.
    [[nodiscard]] bool ParseStatusPayload(std::span<const uint8_t> payload);
};

/// The Duet's framing, bound to its spec.
using ApogeeFraming = VendorDependentFraming<ApogeeDuetSpec>;

} // namespace ASFW::Audio::Oxford::Apogee
