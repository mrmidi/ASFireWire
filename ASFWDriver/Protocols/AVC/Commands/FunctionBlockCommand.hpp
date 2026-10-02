// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FunctionBlockCommand.hpp - FUNCTION BLOCK (0xB8) operand codecs.
//
// Layouts and logic citations:
// - TA 1999008 Audio Subunit 1.0 §10.2 (Selector), §10.3 (Feature), §10.3.1 (Mute), §10.3.2 (Volume).
// - ta1394 audio/src/lib.rs (MIT): AudioFuncBlk :180-260, AudioSelector :283-350,
//   FeatureCtl :773-816, AudioFeature :993-1066.
//
// Operands: [fb type][fb id][control attribute][selector length][selector data][control]
//   Selector: [80][fb id][attr][02][input plug][01]
//   Feature:  [81][fb id][attr][02][channel][control selector][data length][data]

#pragma once

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::AVC::Cmd {

enum class FunctionBlockType : uint8_t {
    kSelector = 0x80,
    kFeature = 0x81,
    kProcessing = 0x82,
};

enum class ControlAttribute : uint8_t {
    kResolution = 0x01,
    kMinimum = 0x02,
    kMaximum = 0x03,
    kDefault = 0x04,
    kDuration = 0x08,
    kCurrent = 0x10,
    kMove = 0x18,
    kDelta = 0x19,
};

/// Feature block control selectors. ta1394 audio lib.rs:802-813.
enum class FeatureControl : uint8_t {
    kMute = 0x01,
    kVolume = 0x02,
    kLrBalance = 0x03,
    kFrBalance = 0x04,
    kBass = 0x05,
    kMid = 0x06,
    kTreble = 0x07,
    kGraphicEqualizer = 0x08,
    kAutomaticGain = 0x09,
    kDelay = 0x0A,
    kBassBoost = 0x0B,
    kLoudness = 0x0C,
};

inline constexpr uint8_t kSelectorControl = 0x01;   ///< ta1394 lib.rs:289
inline constexpr uint8_t kMasterChannel = 0x00;     ///< ta1394 lib.rs:961
inline constexpr uint8_t kBooleanTrue = 0x70;       ///< ta1394 lib.rs:815 (mute on)
inline constexpr uint8_t kBooleanFalse = 0x60;      ///< ta1394 lib.rs:816 (mute off)

/// Per-control data widths from ta1394 audio/src/lib.rs:820-862;
/// zero denotes the variable-width graphic equalizer payload.
struct FeatureControlWidth {
    FeatureControl control;
    uint8_t width;
};

inline constexpr std::array<FeatureControlWidth, 12> kFeatureControlWidths{{
    {FeatureControl::kMute, 1}, {FeatureControl::kVolume, 2},
    {FeatureControl::kLrBalance, 2}, {FeatureControl::kFrBalance, 2},
    {FeatureControl::kBass, 1}, {FeatureControl::kMid, 1},
    {FeatureControl::kTreble, 1}, {FeatureControl::kGraphicEqualizer, 0},
    {FeatureControl::kAutomaticGain, 1}, {FeatureControl::kDelay, 2},
    {FeatureControl::kBassBoost, 1}, {FeatureControl::kLoudness, 1},
}};

[[nodiscard]] constexpr std::optional<uint8_t> FeatureControlDataWidth(FeatureControl control) noexcept {
    for (const auto& entry : kFeatureControlWidths) {
        if (entry.control == control) {
            return entry.width == 0 ? std::nullopt : std::optional<uint8_t>{entry.width};
        }
    }
    return std::nullopt;
}

// ===========================================================================
// Generic Function Block Frame Operands (Opcode 0xB8)
// ===========================================================================

struct FunctionBlockReply {
    FunctionBlockType type{FunctionBlockType::kFeature};
    uint8_t functionBlockId{0};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::array<uint8_t, 16> selectorBytes{};
    uint8_t selectorLength{0};
    std::array<uint8_t, 32> dataBytes{};
    uint8_t dataLength{0};

    [[nodiscard]] std::span<const uint8_t> Selector() const noexcept {
        return {selectorBytes.data(), selectorLength};
    }
    [[nodiscard]] std::span<const uint8_t> Data() const noexcept {
        return {dataBytes.data(), dataLength};
    }
};

struct FunctionBlockOperands {
    static constexpr Opcode kOpcode = Opcode::kFunctionBlock;

    FunctionBlockType type{FunctionBlockType::kFeature};
    uint8_t functionBlockId{0};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::array<uint8_t, 16> selectorBytes{};
    uint8_t selectorLength{0};
    std::array<uint8_t, 32> dataBytes{};
    uint8_t dataLength{0};

    using Reply = FunctionBlockReply;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType /*t*/) const noexcept {
        auto r1 = w.Append(static_cast<uint8_t>(type));
        if (!r1) return r1;
        auto r2 = w.Append(functionBlockId);
        if (!r2) return r2;
        auto r3 = w.Append(static_cast<uint8_t>(attribute));
        if (!r3) return r3;
        auto r4 = w.Append(selectorLength);
        if (!r4) return r4;
        auto r5 = w.Append(std::span<const uint8_t>{selectorBytes.data(), selectorLength});
        if (!r5) return r5;

        if (type == FunctionBlockType::kFeature || type == FunctionBlockType::kProcessing) {
            auto r6 = w.Append(dataLength);
            if (!r6) return r6;
            return w.Append(std::span<const uint8_t>{dataBytes.data(), dataLength});
        }
        if (dataLength > 0) {
            return w.Append(std::span<const uint8_t>{dataBytes.data(), dataLength});
        }
        return {};
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 4) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }

        const auto fbType = static_cast<FunctionBlockType>(in[0]);
        const uint8_t fbId = in[1];
        const auto attr = static_cast<ControlAttribute>(in[2]);
        const uint8_t selLen = in[3];

        if (in.size() < 4u + selLen) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }

        FunctionBlockReply reply{
            .type = fbType,
            .functionBlockId = fbId,
            .attribute = attr,
            .selectorLength = selLen,
        };
        const size_t copySel = std::min<size_t>(selLen, reply.selectorBytes.size());
        std::copy_n(in.begin() + 4, copySel, reply.selectorBytes.begin());

        if (fbType == FunctionBlockType::kFeature || fbType == FunctionBlockType::kProcessing) {
            const size_t dataLenOffset = 4u + selLen;
            if (in.size() < dataLenOffset + 1) {
                return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
            }
            const uint8_t dLen = in[dataLenOffset];
            if (in.size() < dataLenOffset + 1 + dLen) {
                return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
            }
            reply.dataLength = dLen;
            const size_t copyData = std::min<size_t>(dLen, reply.dataBytes.size());
            std::copy_n(in.begin() + dataLenOffset + 1, copyData, reply.dataBytes.begin());
        }

        return reply;
    }
};

using FunctionBlockCommand = Command<FunctionBlockOperands>;

// ===========================================================================
// Typed Selector Operands
// ===========================================================================

struct SelectorValue {
    uint8_t functionBlockId{0};
    uint8_t inputPlug{0xFF};
};

struct SelectorOperands {
    static constexpr Opcode kOpcode = Opcode::kFunctionBlock;

    uint8_t functionBlockId{0};
    uint8_t inputPlug{0xFF};

    using Reply = SelectorValue;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        // FFADO avc_function_block.cpp:348-358 retains the requested
        // selector input; INQUIRY asks whether that CONTROL would be accepted.
        const uint8_t plug = (t == CommandType::kControl || t == CommandType::kSpecificInquiry) ? inputPlug : 0xFF;
        const std::array<uint8_t, 6> ops = {
            static_cast<uint8_t>(FunctionBlockType::kSelector),
            functionBlockId,
            static_cast<uint8_t>(ControlAttribute::kCurrent),
            0x02,
            plug,
            kSelectorControl,
        };
        return w.Append(ops);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 6) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        if (in[0] != static_cast<uint8_t>(FunctionBlockType::kSelector)) {
            return FailAt(AvcErrorKind::kMalformedOperands, 0);
        }
        if (in[3] != 0x02) {
            return FailAt(AvcErrorKind::kMalformedOperands, 3);
        }
        if (in[5] != kSelectorControl) {
            return FailAt(AvcErrorKind::kMalformedOperands, 5);
        }

        return SelectorValue{
            .functionBlockId = in[1],
            .inputPlug = in[4],
        };
    }
};

using SelectorCommand = Command<SelectorOperands>;

// ===========================================================================
// Typed Feature Operands
// ===========================================================================

struct FeatureReply {
    uint8_t functionBlockId{0};
    uint8_t channel{0};
    FeatureControl control{FeatureControl::kMute};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::array<uint8_t, 8> data{};
    uint8_t dataLength{0};

    [[nodiscard]] bool AsMute() const noexcept {
        return dataLength > 0 && data[0] == kBooleanTrue;
    }

    [[nodiscard]] AvcVolume AsVolume() const noexcept {
        if (dataLength < 2) {
            return AvcVolume::Invalid();
        }
        const uint16_t raw = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        return AvcVolume::FromRaw(static_cast<int16_t>(raw));
    }
};

struct FeatureOperands {
    static constexpr Opcode kOpcode = Opcode::kFunctionBlock;

    uint8_t functionBlockId{0};
    uint8_t channel{kMasterChannel};
    FeatureControl control{FeatureControl::kMute};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::array<uint8_t, 8> data{};
    uint8_t dataLength{0};

    using Reply = FeatureReply;

    // Helper constructors (no function per control; just builders on the operand type)
    static FeatureOperands Mute(uint8_t fbId, uint8_t ch, bool muted) noexcept {
        FeatureOperands op{
            .functionBlockId = fbId,
            .channel = ch,
            .control = FeatureControl::kMute,
            .attribute = ControlAttribute::kCurrent,
            .data = { muted ? kBooleanTrue : kBooleanFalse },
            .dataLength = 1,
        };
        return op;
    }

    static FeatureOperands MuteStatus(uint8_t fbId, uint8_t ch) noexcept {
        FeatureOperands op{
            .functionBlockId = fbId,
            .channel = ch,
            .control = FeatureControl::kMute,
            .attribute = ControlAttribute::kCurrent,
            .data = {},
            .dataLength = 0,
        };
        return op;
    }

    static FeatureOperands Volume(uint8_t fbId, uint8_t ch, AvcVolume vol,
                                  ControlAttribute attr = ControlAttribute::kCurrent) noexcept {
        const uint16_t raw = static_cast<uint16_t>(vol.Raw());
        FeatureOperands op{
            .functionBlockId = fbId,
            .channel = ch,
            .control = FeatureControl::kVolume,
            .attribute = attr,
            .data = { static_cast<uint8_t>((raw >> 8) & 0xFF), static_cast<uint8_t>(raw & 0xFF) },
            .dataLength = 2,
        };
        return op;
    }

    static FeatureOperands VolumeStatus(uint8_t fbId, uint8_t ch,
                                        ControlAttribute attr = ControlAttribute::kCurrent) noexcept {
        FeatureOperands op{
            .functionBlockId = fbId,
            .channel = ch,
            .control = FeatureControl::kVolume,
            .attribute = attr,
            .data = {},
            .dataLength = 0,
        };
        return op;
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        const std::array<uint8_t, 6> prefix = {
            static_cast<uint8_t>(FunctionBlockType::kFeature),
            functionBlockId,
            static_cast<uint8_t>(attribute),
            0x02,
            channel,
            static_cast<uint8_t>(control),
        };
        auto r1 = w.Append(prefix);
        if (!r1) return r1;

        if (t == CommandType::kControl) {
            auto r2 = w.Append(dataLength);
            if (!r2) return r2;
            return w.Append(std::span<const uint8_t>{data.data(), dataLength});
        }
        if (t == CommandType::kStatus) {
            if (dataLength > 0) {
                auto r2 = w.Append(dataLength);
                if (!r2) return r2;
                return w.Append(std::span<const uint8_t>{data.data(), dataLength});
            }
            auto width = FeatureControlDataWidth(control);
            if (!width.has_value()) {
                return Fail(AvcErrorKind::kInvalidArgument);
            }
            auto r2 = w.Append(*width);
            if (!r2) return r2;
            for (uint8_t i = 0; i < *width; ++i) {
                auto r3 = w.Append(0xFF);
                if (!r3) return r3;
            }
            return {};
        }
        return Fail(AvcErrorKind::kInvalidArgument);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 7) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        if (in[0] != static_cast<uint8_t>(FunctionBlockType::kFeature)) {
            return FailAt(AvcErrorKind::kMalformedOperands, 0);
        }
        if (in[3] != 0x02) {
            return FailAt(AvcErrorKind::kMalformedOperands, 3);
        }

        const uint8_t dLen = in[6];
        if (in.size() < 7u + dLen) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }

        FeatureReply reply{
            .functionBlockId = in[1],
            .channel = in[4],
            .control = static_cast<FeatureControl>(in[5]),
            .attribute = static_cast<ControlAttribute>(in[2]),
            .dataLength = dLen,
        };
        const size_t copyN = std::min<size_t>(dLen, reply.data.size());
        std::copy_n(in.begin() + 7, copyN, reply.data.begin());
        return reply;
    }
};

using FeatureCommand = Command<FeatureOperands>;

} // namespace ASFW::AVC::Cmd
