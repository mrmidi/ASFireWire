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

Expected<StreamFormat> DecodeStreamFormatBlock(std::span<const uint8_t> block) noexcept {
    if (block.empty()) {
        return FailAt(AvcErrorKind::kOperandsTooShort, 0);
    }

    if (block[0] != kFormatRootAm || block.size() < 2 || block[1] != kFormatLevel1CompoundAm824) {
        StreamFormat format{.kind = StreamFormat::Kind::kOther};
        if (block.size() > format.rawBytes.size()) {
            return Fail(AvcErrorKind::kFrameTooLong);
        }
        format.rawLength = static_cast<uint16_t>(block.size());
        std::copy(block.begin(), block.end(), format.rawBytes.begin());
        return format;
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

    StreamFormat format{
        .kind = StreamFormat::Kind::kCompoundAm824,
        .compound = compound,
    };
    format.rawLength = static_cast<uint16_t>(requiredSize);
    std::copy_n(block.begin(), requiredSize, format.rawBytes.begin());
    return format;
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
// StreamFormatOperands
// ---------------------------------------------------------------------------

Expected<void> StreamFormatOperands::Write(OperandWriter& w, CommandType t) const noexcept {
    if (form == StreamFormatSubfunction::kList && t != CommandType::kStatus) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }
    if (t != CommandType::kStatus && t != CommandType::kControl) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }
    if (t == CommandType::kControl && !controlFormat.has_value()) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }
    auto r1 = w.Append(static_cast<uint8_t>(form));
    if (!r1) return r1;
    auto plugBytes = plug.Encode();
    auto r2 = w.Append(plugBytes);
    if (!r2) return r2;
    auto r3 = w.Append(static_cast<uint8_t>(SupportStatus::kNotUsed));
    if (!r3) return r3;

    if (form == StreamFormatSubfunction::kList) {
        return w.Append(index);
    }
    if (t == CommandType::kControl) {
        std::array<uint8_t, 5 + 2 * kMaxCompoundEntries> buf{};
        auto encoded = EncodeCompoundAm824(*controlFormat, buf);
        if (!encoded) return std::unexpected(encoded.error());
        return w.Append(std::span<const uint8_t>{buf.data(), *encoded});
    }
    return {};
}

Expected<StreamFormatReply> StreamFormatOperands::Read(std::span<const uint8_t> in) const noexcept {
    const size_t minimum = form == StreamFormatSubfunction::kList ? 8 : 7;
    if (in.size() < minimum) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
    }
    if (in[0] != static_cast<uint8_t>(form)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    auto plugResult = PlugAddress::Decode(in.subspan(1, 5));
    if (!plugResult) return std::unexpected(plugResult.error());
    if (form == StreamFormatSubfunction::kList && in[7] != index) {
        return FailAt(AvcErrorKind::kMalformedOperands, 7);
    }
    StreamFormatReply reply{
        .form = form,
        .plug = *plugResult,
        .status = static_cast<SupportStatus>(in[6]),
        .index = form == StreamFormatSubfunction::kList ? in[7] : uint8_t{0},
    };
    if (in.size() > minimum) {
        auto formatResult = DecodeStreamFormatBlock(in.subspan(minimum));
        if (!formatResult) return std::unexpected(formatResult.error());
        reply.format = *formatResult;
    }
    return reply;
}

} // namespace ASFW::AVC::Cmd
