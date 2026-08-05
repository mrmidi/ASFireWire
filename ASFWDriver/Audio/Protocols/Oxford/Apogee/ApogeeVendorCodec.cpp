// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeVendorCodec.cpp - Apogee vendor command operand encoding/decoding.

#include "ApogeeVendorCodec.hpp"

namespace ASFW::Audio::Oxford::Apogee {

using Code = ApogeeVendorCommand::Code;

//==============================================================================
// Constructors
//==============================================================================

ApogeeVendorCommand ApogeeVendorCommand::Bool(Code code, bool value) {
    ApogeeVendorCommand command{};
    command.code = code;
    command.boolValue = value;
    return command;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ApogeeVendorCommand ApogeeVendorCommand::IndexedBool(Code code, uint8_t index, bool value) {
    ApogeeVendorCommand command{};
    command.code = code;
    command.index = index;
    command.boolValue = value;
    return command;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ApogeeVendorCommand ApogeeVendorCommand::InGain(uint8_t index, uint8_t value) {
    ApogeeVendorCommand command{};
    command.code = Code::InGain;
    command.index = index;
    command.u8Value = value;
    return command;
}

ApogeeVendorCommand ApogeeVendorCommand::OutVolume(uint8_t value) {
    ApogeeVendorCommand command{};
    command.code = Code::OutVolume;
    command.u8Value = value;
    return command;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ApogeeVendorCommand ApogeeVendorCommand::MixerSrc(uint8_t source, uint8_t destination,
                                                  uint16_t gain) {
    ApogeeVendorCommand command{};
    command.code = Code::MixerSrc;
    command.index = source;
    command.index2 = destination;
    command.u16Value = gain;
    return command;
}

ApogeeVendorCommand ApogeeVendorCommand::HwState(const std::array<uint8_t, 11>& raw) {
    ApogeeVendorCommand command{};
    command.code = Code::HwState;
    command.hwState = raw;
    return command;
}

ApogeeVendorCommand ApogeeVendorCommand::Make(Code code) {
    ApogeeVendorCommand command{};
    command.code = code;
    return command;
}

//==============================================================================
// Encoding
//==============================================================================

std::vector<uint8_t> ApogeeVendorCommand::BuildOperandBase() const {
    std::vector<uint8_t> operands;
    operands.reserve(kApogeeHeaderSize);

    // OUI + magic + code come from the OXFW-common framing; only the two
    // argument bytes below are Apogee's own.
    ApogeeFraming::WritePrefix(operands, static_cast<uint8_t>(code));
    operands.push_back(kApogeeArgDefault);
    operands.push_back(kApogeeArgDefault);

    static constexpr size_t kArg1 = kVendorPrefixSize;
    static constexpr size_t kArg2 = kVendorPrefixSize + 1U;

    switch (code) {
        // Per-channel controls: 0x80 marks the specifier, the index follows.
        case Code::MicPolarity:
        case Code::XlrIsMicLevel:
        case Code::XlrIsConsumerLevel:
        case Code::MicPhantom:
        case Code::InGain:
        case Code::InputSourceIsPhone:
            operands[kArg1] = kApogeeArgIndexed;
            operands[kArg2] = index;
            break;
        // Output-side controls: specifier only, no index.
        case Code::OutIsConsumerLevel:
        case Code::OutMute:
        case Code::OutVolume:
        case Code::MuteForLineOut:
        case Code::MuteForHpOut:
        case Code::UnmuteForLineOut:
        case Code::UnmuteForHpOut:
            operands[kArg1] = kApogeeArgIndexed;
            break;
        // Mixer: both argument slots carry routing.
        case Code::MixerSrc:
            operands[kArg1] = EncodeMixerSource(index);
            operands[kArg2] = index2;
            break;
        // No arguments; both slots stay at the default filler.
        case Code::HwState:
        case Code::OutSourceIsMixer:
        case Code::DisplayOverholdTwoSec:
        case Code::DisplayClear:
        case Code::DisplayIsInput:
        case Code::InClickless:
        case Code::DisplayFollowToKnob:
            break;
    }

    return operands;
}

void ApogeeVendorCommand::AppendControlValue(std::vector<uint8_t>& operands) const {
    switch (code) {
        case Code::MicPolarity:
        case Code::XlrIsMicLevel:
        case Code::XlrIsConsumerLevel:
        case Code::MicPhantom:
        case Code::OutIsConsumerLevel:
        case Code::OutMute:
        case Code::InputSourceIsPhone:
        case Code::OutSourceIsMixer:
        case Code::DisplayOverholdTwoSec:
        case Code::MuteForLineOut:
        case Code::MuteForHpOut:
        case Code::UnmuteForLineOut:
        case Code::UnmuteForHpOut:
        case Code::DisplayIsInput:
        case Code::InClickless:
        case Code::DisplayFollowToKnob:
            operands.push_back(ToAvcBool(boolValue));
            break;
        case Code::InGain:
        case Code::OutVolume:
            operands.push_back(u8Value);
            break;
        case Code::MixerSrc:
            // Gain is big-endian on the wire, like every IEEE 1394 payload.
            operands.push_back(static_cast<uint8_t>((u16Value >> 8U) & 0xFFU));
            operands.push_back(static_cast<uint8_t>(u16Value & 0xFFU));
            break;
        case Code::HwState:
            operands.insert(operands.end(), hwState.begin(), hwState.end());
            break;
        case Code::DisplayClear:
            break;
    }
}

//==============================================================================
// Decoding
//==============================================================================

bool ApogeeVendorCommand::ParseStatusPayload(std::span<const uint8_t> payload) {
    // Vendor, magic and the echoed code are the OXFW-common structural check.
    if (!ApogeeFraming::ValidatePrefix(payload, static_cast<uint8_t>(code))) {
        return false;
    }
    if (payload.size() < kApogeeHeaderSize) {
        return false;
    }

    static constexpr size_t kArg1 = kVendorPrefixSize;
    static constexpr size_t kArg2 = kVendorPrefixSize + 1U;
    static constexpr size_t kValue = kApogeeHeaderSize;

    switch (code) {
        case Code::MicPolarity:
        case Code::XlrIsMicLevel:
        case Code::XlrIsConsumerLevel:
        case Code::MicPhantom:
        case Code::InputSourceIsPhone:
            if (payload[kArg2] != index || payload.size() < (kValue + 1U)) {
                return false;
            }
            boolValue = FromAvcBool(payload[kValue]);
            return true;
        case Code::OutIsConsumerLevel:
        case Code::OutMute:
        case Code::OutSourceIsMixer:
        case Code::DisplayOverholdTwoSec:
        case Code::MuteForLineOut:
        case Code::MuteForHpOut:
        case Code::UnmuteForLineOut:
        case Code::UnmuteForHpOut:
        case Code::DisplayIsInput:
        case Code::InClickless:
        case Code::DisplayFollowToKnob:
            if (payload.size() < (kValue + 1U)) {
                return false;
            }
            boolValue = FromAvcBool(payload[kValue]);
            return true;
        case Code::InGain:
            if (payload[kArg2] != index || payload.size() < (kValue + 1U)) {
                return false;
            }
            u8Value = payload[kValue];
            return true;
        case Code::MixerSrc:
            if (payload[kArg1] != EncodeMixerSource(index) || payload[kArg2] != index2 ||
                payload.size() < (kValue + 2U)) {
                return false;
            }
            u16Value = static_cast<uint16_t>((static_cast<uint16_t>(payload[kValue]) << 8U) |
                                             static_cast<uint16_t>(payload[kValue + 1U]));
            return true;
        case Code::HwState:
            if (payload.size() < (kValue + hwState.size())) {
                return false;
            }
            for (size_t i = 0; i < hwState.size(); ++i) {
                hwState[i] = payload[kValue + i];
            }
            return true;
        case Code::OutVolume:
            if (payload.size() < (kValue + 1U)) {
                return false;
            }
            u8Value = payload[kValue];
            return true;
        case Code::DisplayClear:
            return true;
    }

    return false;
}

} // namespace ASFW::Audio::Oxford::Apogee
