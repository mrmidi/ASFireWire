// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// StreamFormatCommand.cpp - Stream format query and set: STREAM FORMAT SUPPORT
// (0x2F, TA 2001002) and EXTENDED STREAM FORMAT INFORMATION (0xBF, unpublished
// draft).
//
// Layouts and logic citations:
// - ta1394 stream-format/src/lib.rs (MIT): PlugAddr :785-1060, SupportStatus :1080-1083,
//   CompoundAm824 :454-649, ExtendedStreamFormatSingle :1166-1238,
//   ExtendedStreamFormatList :1239-1300.
// - Linux firewire/oxfw/oxfw-command.c:20-31 (0xBF single CONTROL/STATUS),
//   bebob/bebob_command.c:289-331 (0x2F list, BridgeCo).
// Fresh clean-room implementation.

#include "StreamFormatCommand.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// PlugAddress
// ---------------------------------------------------------------------------

Expected<PlugAddress> PlugAddress::Decode(std::span<const uint8_t> raw) noexcept {
    if (raw.size() < 5) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(raw.size()));
    }

    if (raw[0] > static_cast<uint8_t>(PlugDirection::kOutput)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    const auto direction = static_cast<PlugDirection>(raw[0]);

    if (raw[1] > static_cast<uint8_t>(PlugAddressMode::kFunctionBlock)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 1);
    }
    const auto mode = static_cast<PlugAddressMode>(raw[1]);

    switch (mode) {
        case PlugAddressMode::kUnit: {
            if (raw[2] > static_cast<uint8_t>(UnitPlugType::kAsync)) {
                return FailAt(AvcErrorKind::kMalformedOperands, 2);
            }
            const auto unitPlugType = static_cast<UnitPlugType>(raw[2]);
            const uint8_t plugId = raw[3];
            return PlugAddress{direction, mode, unitPlugType, 0xFF, 0xFF, plugId};
        }
        case PlugAddressMode::kSubunit: {
            const uint8_t plugId = raw[2];
            return PlugAddress{direction, mode, UnitPlugType::kPcr, 0xFF, 0xFF, plugId};
        }
        case PlugAddressMode::kFunctionBlock: {
            const uint8_t fbType = raw[2];
            const uint8_t fbId = raw[3];
            const uint8_t plugId = raw[4];
            return PlugAddress{direction, mode, UnitPlugType::kPcr, fbType, fbId, plugId};
        }
    }
}

// ---------------------------------------------------------------------------
// Format Block Parsing & Encoding
// ---------------------------------------------------------------------------

Expected<StreamFormat> ParseStreamFormatBlock(std::span<const uint8_t> block) noexcept {
    if (block.empty()) {
        return FailAt(AvcErrorKind::kOperandsTooShort, 0);
    }

    if (block[0] != kFormatRootAm || block.size() < 2 || block[1] != kFormatLevel1CompoundAm824) {
        return StreamFormat{
            .kind = StreamFormat::Kind::kOther,
            .compound = {},
            .raw = block,
        };
    }

    // Compound AM824: [90][40][rate][flags][entry count][count, format]...
    if (block.size() < 5) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(block.size()));
    }

    const uint8_t entryCount = block[4];
    if (entryCount > kMaxCompoundEntries) {
        return Fail(AvcErrorKind::kUnsupported);
    }

    const size_t requiredSize = 5u + 2u * static_cast<size_t>(entryCount);
    if (block.size() < requiredSize) {
        return FailAt(AvcErrorKind::kMalformedOperands, 4);
    }

    CompoundAm824 compound{};
    compound.rate = static_cast<StreamFormatRate>(block[2]);
    compound.syncSource = (block[3] & 0x04) != 0;
    compound.rateControl = static_cast<RateControl>(block[3] & 0x03);
    compound.entryCount = entryCount;

    for (size_t i = 0; i < entryCount; ++i) {
        compound.entries[i] = CompoundEntry{
            .count = block[5 + 2 * i],
            .format = static_cast<Am824Format>(block[5 + 2 * i + 1]),
        };
    }

    return StreamFormat{
        .kind = StreamFormat::Kind::kCompoundAm824,
        .compound = compound,
        .raw = block.subspan(0, requiredSize),
    };
}

Expected<size_t> EncodeCompoundAm824(const CompoundAm824& format, std::span<uint8_t> out) noexcept {
    if (format.entryCount > kMaxCompoundEntries) {
        return Fail(AvcErrorKind::kUnsupported);
    }

    const size_t requiredSize = 5u + 2u * static_cast<size_t>(format.entryCount);
    if (out.size() < requiredSize) {
        return Fail(AvcErrorKind::kFrameTooLong);
    }

    out[0] = kFormatRootAm;
    out[1] = kFormatLevel1CompoundAm824;
    out[2] = static_cast<uint8_t>(format.rate);

    uint8_t flags = static_cast<uint8_t>(format.rateControl) & 0x03;
    if (format.syncSource) {
        flags |= 0x04;
    }
    out[3] = flags;
    out[4] = format.entryCount;

    for (size_t i = 0; i < format.entryCount; ++i) {
        out[5 + 2 * i] = format.entries[i].count;
        out[5 + 2 * i + 1] = static_cast<uint8_t>(format.entries[i].format);
    }

    return requiredSize;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildStreamFormatSingleStatus(StreamFormatOpcode opcode,
                                                     SubunitAddress address,
                                                     const PlugAddress& plug) noexcept {
    const auto plugBytes = plug.Encode();
    const std::array<uint8_t, 7> operands = {
        static_cast<uint8_t>(StreamFormatSubfunction::kSingle),
        plugBytes[0], plugBytes[1], plugBytes[2], plugBytes[3], plugBytes[4],
        static_cast<uint8_t>(SupportStatus::kNotUsed),
    };

    return CommandFrame::Make(CommandType::kStatus, address, static_cast<Opcode>(opcode), operands);
}

Expected<CommandFrame> BuildStreamFormatListStatus(StreamFormatOpcode opcode,
                                                   SubunitAddress address,
                                                   const PlugAddress& plug,
                                                   uint8_t index) noexcept {
    const auto plugBytes = plug.Encode();
    const std::array<uint8_t, 8> operands = {
        static_cast<uint8_t>(StreamFormatSubfunction::kList),
        plugBytes[0], plugBytes[1], plugBytes[2], plugBytes[3], plugBytes[4],
        static_cast<uint8_t>(SupportStatus::kNotUsed),
        index,
    };

    return CommandFrame::Make(CommandType::kStatus, address, static_cast<Opcode>(opcode), operands);
}

Expected<CommandFrame> BuildStreamFormatSingleControl(StreamFormatOpcode opcode,
                                                     SubunitAddress address,
                                                     const PlugAddress& plug,
                                                     const CompoundAm824& format) noexcept {
    std::array<uint8_t, kMaxOperandBytes> operands{};
    operands[0] = static_cast<uint8_t>(StreamFormatSubfunction::kSingle);
    const auto plugBytes = plug.Encode();
    std::copy(plugBytes.begin(), plugBytes.end(), operands.begin() + 1);
    operands[6] = static_cast<uint8_t>(SupportStatus::kNotUsed);

    auto encodeRes = EncodeCompoundAm824(format, std::span<uint8_t>{operands}.subspan(7));
    if (!encodeRes) {
        return std::unexpected(encodeRes.error());
    }

    const size_t totalLength = 7u + *encodeRes;
    return CommandFrame::Make(CommandType::kControl, address, static_cast<Opcode>(opcode),
                              std::span<const uint8_t>{operands.data(), totalLength});
}

Expected<StreamFormatSingle> ParseStreamFormatSingle(const Response& response,
                                                    ResponseCode expected) noexcept {
    auto operandsRes = OperandsIf(response, expected);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 7) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != static_cast<uint8_t>(StreamFormatSubfunction::kSingle)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }

    auto plugRes = PlugAddress::Decode(operands.subspan(1, 5));
    if (!plugRes) {
        return std::unexpected(plugRes.error());
    }

    StreamFormat format{};
    if (operands.size() > 7) {
        auto formatRes = ParseStreamFormatBlock(operands.subspan(7));
        if (!formatRes) {
            return std::unexpected(formatRes.error());
        }
        format = *formatRes;
    }

    return StreamFormatSingle{
        .plug = *plugRes,
        .status = static_cast<SupportStatus>(operands[6]),
        .format = format,
    };
}

Expected<StreamFormatListEntry> ParseStreamFormatList(const Response& response,
                                                     uint8_t requestedIndex) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 8) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != static_cast<uint8_t>(StreamFormatSubfunction::kList)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }

    auto plugRes = PlugAddress::Decode(operands.subspan(1, 5));
    if (!plugRes) {
        return std::unexpected(plugRes.error());
    }

    if (operands[7] != requestedIndex) {
        return FailAt(AvcErrorKind::kMalformedOperands, 7);
    }

    StreamFormat format{};
    if (operands.size() > 8) {
        auto formatRes = ParseStreamFormatBlock(operands.subspan(8));
        if (!formatRes) {
            return std::unexpected(formatRes.error());
        }
        format = *formatRes;
    }

    return StreamFormatListEntry{
        .plug = *plugRes,
        .status = static_cast<SupportStatus>(operands[6]),
        .index = operands[7],
        .format = format,
    };
}

} // namespace ASFW::AVC::Cmd
