// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FunctionBlockCommand.cpp - FUNCTION BLOCK (0xB8) on the audio subunit:
// selector blocks, and the mute and volume controls of feature blocks.
//
// Layouts and logic citations:
// - TA 1999008 Audio Subunit 1.0 §10.2 (Selector), §10.3 (Feature), §10.3.1 (Mute), §10.3.2 (Volume).
// - ta1394 audio/src/lib.rs (MIT): AudioFuncBlk :180-260, AudioSelector :283-350,
//   FeatureCtl :773-816, AudioFeature :993-1066.
// - Linux firewire/bebob/bebob_command.c:10-86.
// Fresh clean-room implementation.

#include "FunctionBlockCommand.hpp"

#include <array>
#include <cstdint>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// Selector Block
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildSelectorStatus(SubunitAddress subunit, uint8_t functionBlockId) noexcept {
    const std::array<uint8_t, 6> operands = {
        static_cast<uint8_t>(FunctionBlockType::kSelector),
        functionBlockId,
        static_cast<uint8_t>(ControlAttribute::kCurrent),
        0x02,
        0xFF,
        kSelectorControl,
    };

    return CommandFrame::Make(CommandType::kStatus, subunit, Opcode::kFunctionBlock, operands);
}

Expected<CommandFrame> BuildSelectorControl(SubunitAddress subunit, uint8_t functionBlockId,
                                            uint8_t inputPlug) noexcept {
    const std::array<uint8_t, 6> operands = {
        static_cast<uint8_t>(FunctionBlockType::kSelector),
        functionBlockId,
        static_cast<uint8_t>(ControlAttribute::kCurrent),
        0x02,
        inputPlug,
        kSelectorControl,
    };

    return CommandFrame::Make(CommandType::kControl, subunit, Opcode::kFunctionBlock, operands);
}

Expected<SelectorValue> ParseSelector(const Response& response, ResponseCode expected) noexcept {
    auto operandsRes = OperandsIf(response, expected);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 6) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != static_cast<uint8_t>(FunctionBlockType::kSelector)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    if (operands[3] != 0x02) {
        return FailAt(AvcErrorKind::kMalformedOperands, 3);
    }
    if (operands[5] != kSelectorControl) {
        return FailAt(AvcErrorKind::kMalformedOperands, 5);
    }

    return SelectorValue{
        .functionBlockId = operands[1],
        .inputPlug = operands[4],
    };
}

// ---------------------------------------------------------------------------
// Feature Block: Mute
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildFeatureMuteStatus(SubunitAddress subunit, uint8_t functionBlockId,
                                              uint8_t channel) noexcept {
    const std::array<uint8_t, 8> operands = {
        static_cast<uint8_t>(FunctionBlockType::kFeature),
        functionBlockId,
        static_cast<uint8_t>(ControlAttribute::kCurrent),
        0x02,
        channel,
        static_cast<uint8_t>(FeatureControl::kMute),
        0x01,
        0xFF,
    };

    return CommandFrame::Make(CommandType::kStatus, subunit, Opcode::kFunctionBlock, operands);
}

Expected<CommandFrame> BuildFeatureMuteControl(SubunitAddress subunit, uint8_t functionBlockId,
                                               uint8_t channel, bool muted) noexcept {
    const std::array<uint8_t, 8> operands = {
        static_cast<uint8_t>(FunctionBlockType::kFeature),
        functionBlockId,
        static_cast<uint8_t>(ControlAttribute::kCurrent),
        0x02,
        channel,
        static_cast<uint8_t>(FeatureControl::kMute),
        0x01,
        muted ? kBooleanTrue : kBooleanFalse,
    };

    return CommandFrame::Make(CommandType::kControl, subunit, Opcode::kFunctionBlock, operands);
}

Expected<bool> ParseFeatureMute(const Response& response, ResponseCode expected) noexcept {
    auto operandsRes = OperandsIf(response, expected);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 8) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != static_cast<uint8_t>(FunctionBlockType::kFeature)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    if (operands[3] != 0x02) {
        return FailAt(AvcErrorKind::kMalformedOperands, 3);
    }
    if (operands[5] != static_cast<uint8_t>(FeatureControl::kMute)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 5);
    }
    if (operands[6] < 1) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }

    const uint8_t muteVal = operands[7];
    if (muteVal == kBooleanTrue) {
        return true;
    }
    if (muteVal == kBooleanFalse) {
        return false;
    }

    return FailAt(AvcErrorKind::kMalformedOperands, 7);
}

// ---------------------------------------------------------------------------
// Feature Block: Volume
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildFeatureVolumeStatus(SubunitAddress subunit, uint8_t functionBlockId,
                                                uint8_t channel, ControlAttribute attribute) noexcept {
    const auto rawInvalid = static_cast<uint16_t>(kVolumeInvalid);
    const std::array<uint8_t, 9> operands = {
        static_cast<uint8_t>(FunctionBlockType::kFeature),
        functionBlockId,
        static_cast<uint8_t>(attribute),
        0x02,
        channel,
        static_cast<uint8_t>(FeatureControl::kVolume),
        0x02,
        static_cast<uint8_t>((rawInvalid >> 8) & 0xFF),
        static_cast<uint8_t>(rawInvalid & 0xFF),
    };

    return CommandFrame::Make(CommandType::kStatus, subunit, Opcode::kFunctionBlock, operands);
}

Expected<CommandFrame> BuildFeatureVolumeControl(SubunitAddress subunit, uint8_t functionBlockId,
                                                 uint8_t channel, int16_t value) noexcept {
    const auto rawValue = static_cast<uint16_t>(value);
    const std::array<uint8_t, 9> operands = {
        static_cast<uint8_t>(FunctionBlockType::kFeature),
        functionBlockId,
        static_cast<uint8_t>(ControlAttribute::kCurrent),
        0x02,
        channel,
        static_cast<uint8_t>(FeatureControl::kVolume),
        0x02,
        static_cast<uint8_t>((rawValue >> 8) & 0xFF),
        static_cast<uint8_t>(rawValue & 0xFF),
    };

    return CommandFrame::Make(CommandType::kControl, subunit, Opcode::kFunctionBlock, operands);
}

Expected<int16_t> ParseFeatureVolume(const Response& response, ResponseCode expected) noexcept {
    auto operandsRes = OperandsIf(response, expected);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 9) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != static_cast<uint8_t>(FunctionBlockType::kFeature)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    if (operands[3] != 0x02) {
        return FailAt(AvcErrorKind::kMalformedOperands, 3);
    }
    if (operands[5] != static_cast<uint8_t>(FeatureControl::kVolume)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 5);
    }
    if (operands[6] < 2) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }

    const uint16_t rawVal = (static_cast<uint16_t>(operands[7]) << 8) | operands[8];
    return static_cast<int16_t>(rawVal);
}

} // namespace ASFW::AVC::Cmd
