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
#include <vector>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// Generic Function Block Frame Codec
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildFunctionBlock(
    CommandType type,
    SubunitAddress subunit,
    const FunctionBlockFrame& frame) noexcept {
    if (frame.selectorBytes.size() > 255) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }

    std::vector<uint8_t> operands;
    operands.reserve(4 + frame.selectorBytes.size() + 1 + frame.dataBytes.size());

    operands.push_back(static_cast<uint8_t>(frame.type));
    operands.push_back(frame.functionBlockId);
    operands.push_back(static_cast<uint8_t>(frame.attribute));
    operands.push_back(static_cast<uint8_t>(frame.selectorBytes.size()));
    operands.insert(operands.end(), frame.selectorBytes.begin(), frame.selectorBytes.end());

    if (frame.type == FunctionBlockType::kFeature || frame.type == FunctionBlockType::kProcessing) {
        operands.push_back(static_cast<uint8_t>(frame.dataBytes.size()));
        operands.insert(operands.end(), frame.dataBytes.begin(), frame.dataBytes.end());
    } else if (!frame.dataBytes.empty()) {
        operands.insert(operands.end(), frame.dataBytes.begin(), frame.dataBytes.end());
    }

    return CommandFrame::Make(type, subunit, Opcode::kFunctionBlock, operands);
}

Expected<FunctionBlockReply> ParseFunctionBlock(std::span<const uint8_t> operands) noexcept {
    if (operands.size() < 4) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }

    const auto fbType = static_cast<FunctionBlockType>(operands[0]);
    const uint8_t fbId = operands[1];
    const auto attr = static_cast<ControlAttribute>(operands[2]);
    const uint8_t selectorLen = operands[3];

    if (operands.size() < 4 + selectorLen) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }

    auto selectorBytes = operands.subspan(4, selectorLen);
    std::span<const uint8_t> dataBytes{};

    if (fbType == FunctionBlockType::kFeature || fbType == FunctionBlockType::kProcessing) {
        const size_t dataLenOffset = 4 + selectorLen;
        if (operands.size() < dataLenOffset + 1) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
        }
        const uint8_t dataLen = operands[dataLenOffset];
        if (operands.size() < dataLenOffset + 1 + dataLen) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
        }
        dataBytes = operands.subspan(dataLenOffset + 1, dataLen);
    } else if (operands.size() > 4 + selectorLen) {
        dataBytes = operands.subspan(4 + selectorLen);
    }

    return FunctionBlockReply{
        .type = fbType,
        .functionBlockId = fbId,
        .attribute = attr,
        .selectorBytes = selectorBytes,
        .dataBytes = dataBytes,
    };
}

// ---------------------------------------------------------------------------
// Selector Command
// ---------------------------------------------------------------------------

Expected<CommandFrame> SelectorCommand::Encode(CommandType type) const noexcept {
    const uint8_t plug = (type == CommandType::kStatus) ? 0xFF : inputPlug;
    const std::array<uint8_t, 2> selectorBytes = { plug, kSelectorControl };

    FunctionBlockFrame frame{
        .type = FunctionBlockType::kSelector,
        .functionBlockId = functionBlockId,
        .attribute = ControlAttribute::kCurrent,
        .selectorBytes = selectorBytes,
        .dataBytes = {},
    };

    return BuildFunctionBlock(type, subunit, frame);
}

Expected<SelectorValue> SelectorCommand::Decode(std::span<const uint8_t> operands) noexcept {
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
// Feature Command (Generic)
// ---------------------------------------------------------------------------

Expected<CommandFrame> FeatureCommand::Encode(CommandType type) const noexcept {
    const std::array<uint8_t, 2> selectorBytes = {
        channel,
        static_cast<uint8_t>(control),
    };

    std::vector<uint8_t> dataBuf;
    std::span<const uint8_t> payload = controlData;

    if (type == CommandType::kStatus && controlData.empty()) {
        auto width = FeatureControlDataWidth(control);
        if (width) {
            dataBuf.assign(*width, 0xFF);
            payload = dataBuf;
        }
    }

    FunctionBlockFrame frame{
        .type = FunctionBlockType::kFeature,
        .functionBlockId = functionBlockId,
        .attribute = attribute,
        .selectorBytes = selectorBytes,
        .dataBytes = payload,
    };

    return BuildFunctionBlock(type, subunit, frame);
}

Expected<FeatureReply> FeatureCommand::Decode(std::span<const uint8_t> operands) noexcept {
    auto parsed = ParseFunctionBlock(operands);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    if (parsed->type != FunctionBlockType::kFeature) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    if (parsed->selectorBytes.size() < 2) {
        return FailAt(AvcErrorKind::kMalformedOperands, 3);
    }

    return FeatureReply{
        .functionBlockId = parsed->functionBlockId,
        .channel = parsed->selectorBytes[0],
        .control = static_cast<FeatureControl>(parsed->selectorBytes[1]),
        .attribute = parsed->attribute,
        .data = parsed->dataBytes,
    };
}

// ---------------------------------------------------------------------------
// Feature: Mute Command
// ---------------------------------------------------------------------------

Expected<CommandFrame> FeatureMuteCommand::Encode(CommandType type) const noexcept {
    const std::array<uint8_t, 1> muteData = {
        (type == CommandType::kStatus) ? uint8_t{0xFF} : (muted ? kBooleanTrue : kBooleanFalse)
    };

    FeatureCommand cmd{
        .functionBlockId = functionBlockId,
        .channel = channel,
        .control = FeatureControl::kMute,
        .attribute = ControlAttribute::kCurrent,
        .controlData = muteData,
        .subunit = subunit,
    };

    return cmd.Encode(type);
}

Expected<bool> FeatureMuteCommand::Decode(std::span<const uint8_t> operands) noexcept {
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
// Feature: Volume Command
// ---------------------------------------------------------------------------

Expected<CommandFrame> FeatureVolumeCommand::Encode(CommandType type) const noexcept {
    const uint16_t rawVal = (type == CommandType::kStatus)
        ? static_cast<uint16_t>(kVolumeInvalid)
        : static_cast<uint16_t>(volume.Raw());

    const std::array<uint8_t, 2> volData = {
        static_cast<uint8_t>((rawVal >> 8) & 0xFF),
        static_cast<uint8_t>(rawVal & 0xFF),
    };

    FeatureCommand cmd{
        .functionBlockId = functionBlockId,
        .channel = channel,
        .control = FeatureControl::kVolume,
        .attribute = attribute,
        .controlData = volData,
        .subunit = subunit,
    };

    return cmd.Encode(type);
}

Expected<AvcVolume> FeatureVolumeCommand::Decode(std::span<const uint8_t> operands) noexcept {
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
    return AvcVolume::FromRaw(static_cast<int16_t>(rawVal));
}

} // namespace ASFW::AVC::Cmd
