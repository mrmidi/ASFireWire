// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FunctionBlockCommand.hpp - FUNCTION BLOCK (0xB8) on the audio subunit:
// selector blocks, and the mute and volume controls of feature blocks.
// Phase 1 covers what today's callers use (the Phase 88 mixer map: selectors,
// mutes, volumes). Other feature controls and processing blocks come later.
//
// Layout: ta1394 audio/src/lib.rs (MIT): AudioFuncBlkType :35-46,
// CtlAttr :54-100, AudioFuncBlk :180-260 (OPCODE :256), AudioSelector :283-350,
// FeatureCtl :773-816, AudioCh :943-963, AudioFeature :993-1066.
// Linux bebob_command.c:10-86 (selector CONTROL/STATUS). Fresh implementation.
//
// Operands: [fb type][fb id][control attribute][selector length][selector data][control]
//   selector length = bytes of audio_selector_data + the control selector
//   (TA 1999008 Audio Subunit 1.0, line "selector_length (n+1)"; see tmp/specs).
//   Selector: [80][fb id][attr][02][input plug][01]. Linux bebob_command.c:20-28
//     (input plug 0xFF for STATUS). Today's AudioFunctionBlockCommand sends the same.
//   Feature:  [81][fb id][attr][02][channel][control selector][data length][data].
//     "The selector_length field (Operand[3]) for feature function block shall
//     always be set to 2" (TA 1999008 §10.3, p.74). Mute: data length 1, Mute_On
//     0x70 = TRUE (muted), 0x60 = FALSE (not muted), 0xFF invalid in CONTROL
//     (§10.3.1, p.75). Volume: data length 2, big-endian int16.
//
// BYTE RULE:
//   - Selector blocks: byte-equal to today's AudioFunctionBlockCommand (it matches
//     Linux and the spec).
//   - Feature blocks: follow the SPEC, not today's code. Today's
//     BeBoBProtocol::SetFeatureMute/Volume send selector length 4 / 5 and 0x00 for
//     "mute", which break §10.3 and §10.3.1; they were only ever sent best-effort
//     (BeBoBProtocol.cpp:187, failures ignored), so nothing proves a device accepted
//     them. The differential test records old vs new bytes as an INTENDED difference.
//     The phase-1 capture (FB STATUS + SPECIFIC INQUIRY on the Phase 88) shows what
//     the device accepts.
//
// Implementation: FunctionBlockCommand.cpp (phase 1).

#pragma once

#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

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

inline constexpr uint8_t kSelectorControl = 0x01;   ///< ta1394 lib.rs:289; Linux bebob_command.c:28
inline constexpr uint8_t kMasterChannel = 0x00;     ///< ta1394 lib.rs:961
inline constexpr uint8_t kBooleanTrue = 0x70;       ///< ta1394 lib.rs:815 (mute on)
inline constexpr uint8_t kBooleanFalse = 0x60;      ///< ta1394 lib.rs:816 (mute off)
inline constexpr int16_t kVolumeInvalid = 0x7FFF;   ///< ta1394 lib.rs:359

/// Per-control data width table (TA 1999008 §10.3 Table 10.8, p.74).
[[nodiscard]] constexpr std::optional<uint8_t> FeatureControlDataWidth(FeatureControl control) noexcept {
    switch (control) {
        case FeatureControl::kMute: return 1;
        case FeatureControl::kVolume: return 2;
        case FeatureControl::kLrBalance: return 2;
        case FeatureControl::kFrBalance: return 2;
        case FeatureControl::kBass: return 1;
        case FeatureControl::kMid: return 1;
        case FeatureControl::kTreble: return 1;
        case FeatureControl::kGraphicEqualizer: return std::nullopt; // complex/variable
        case FeatureControl::kAutomaticGain: return 1;
        case FeatureControl::kDelay: return 2;
        case FeatureControl::kBassBoost: return 1;
        case FeatureControl::kLoudness: return 1;
    }
    return std::nullopt;
}

// ===========================================================================
// Generic Function Block Frame Codec (Opcode 0xB8)
// ===========================================================================

struct FunctionBlockFrame {
    FunctionBlockType type{FunctionBlockType::kFeature};
    uint8_t functionBlockId{0};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::span<const uint8_t> selectorBytes{};
    std::span<const uint8_t> dataBytes{};
};

struct FunctionBlockReply {
    FunctionBlockType type{FunctionBlockType::kFeature};
    uint8_t functionBlockId{0};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::span<const uint8_t> selectorBytes{};
    std::span<const uint8_t> dataBytes{};
};

[[nodiscard]] Expected<CommandFrame> BuildFunctionBlock(
    CommandType type,
    SubunitAddress subunit,
    const FunctionBlockFrame& frame) noexcept;

[[nodiscard]] Expected<FunctionBlockReply> ParseFunctionBlock(
    std::span<const uint8_t> operands) noexcept;

// ===========================================================================
// Typed Commands (per block type)
// ===========================================================================

// ---- Selector -------------------------------------------------------------

struct SelectorValue {
    uint8_t functionBlockId{0};
    uint8_t inputPlug{0xFF};
};

struct SelectorCommand {
    uint8_t functionBlockId{0};
    uint8_t inputPlug{0xFF};
    SubunitAddress subunit{kAudioSubunit0};

    using Reply = SelectorValue;

    [[nodiscard]] Expected<CommandFrame> Encode(CommandType type = CommandType::kStatus) const noexcept;
    [[nodiscard]] static Expected<Reply> Decode(std::span<const uint8_t> operands) noexcept;
};

// ---- Feature: Generic -----------------------------------------------------

struct FeatureReply {
    uint8_t functionBlockId{0};
    uint8_t channel{0};
    FeatureControl control{FeatureControl::kMute};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::span<const uint8_t> data{};
};

struct FeatureCommand {
    uint8_t functionBlockId{0};
    uint8_t channel{kMasterChannel};
    FeatureControl control{FeatureControl::kMute};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::span<const uint8_t> controlData{};
    SubunitAddress subunit{kAudioSubunit0};

    using Reply = FeatureReply;

    [[nodiscard]] Expected<CommandFrame> Encode(CommandType type = CommandType::kStatus) const noexcept;
    [[nodiscard]] static Expected<Reply> Decode(std::span<const uint8_t> operands) noexcept;
};

// ---- Feature: Mute --------------------------------------------------------

struct FeatureMuteCommand {
    uint8_t functionBlockId{0};
    uint8_t channel{kMasterChannel};
    bool muted{false};
    SubunitAddress subunit{kAudioSubunit0};

    using Reply = bool;

    [[nodiscard]] Expected<CommandFrame> Encode(CommandType type = CommandType::kStatus) const noexcept;
    [[nodiscard]] static Expected<Reply> Decode(std::span<const uint8_t> operands) noexcept;
};

// ---- Feature: Volume ------------------------------------------------------

struct FeatureVolumeCommand {
    uint8_t functionBlockId{0};
    uint8_t channel{kMasterChannel};
    AvcVolume volume{AvcVolume::Invalid()};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    SubunitAddress subunit{kAudioSubunit0};

    using Reply = AvcVolume;

    [[nodiscard]] Expected<CommandFrame> Encode(CommandType type = CommandType::kStatus) const noexcept;
    [[nodiscard]] static Expected<Reply> Decode(std::span<const uint8_t> operands) noexcept;
};

// ===========================================================================
// Compatibility Wrappers (used until callers migrate in phase 2c)
// ===========================================================================

[[nodiscard]] inline Expected<CommandFrame> BuildSelectorStatus(SubunitAddress subunit, uint8_t functionBlockId) noexcept {
    return SelectorCommand{functionBlockId, 0xFF, subunit}.Encode(CommandType::kStatus);
}

[[nodiscard]] inline Expected<CommandFrame> BuildSelectorControl(SubunitAddress subunit, uint8_t functionBlockId,
                                                                 uint8_t inputPlug) noexcept {
    return SelectorCommand{functionBlockId, inputPlug, subunit}.Encode(CommandType::kControl);
}

[[nodiscard]] inline Expected<SelectorValue> ParseSelector(std::span<const uint8_t> operands) noexcept {
    return SelectorCommand::Decode(operands);
}

[[nodiscard]] inline Expected<SelectorValue> ParseSelector(const Response& response,
                                                           ResponseCode expected = ResponseCode::kImplementedStable) noexcept {
    if (response.code != expected) {
        return std::unexpected(AvcError::Unexpected(response.code));
    }
    return SelectorCommand::Decode(response.operands);
}

[[nodiscard]] inline Expected<CommandFrame> BuildFeatureMuteStatus(SubunitAddress subunit, uint8_t functionBlockId,
                                                                   uint8_t channel) noexcept {
    return FeatureMuteCommand{functionBlockId, channel, false, subunit}.Encode(CommandType::kStatus);
}

[[nodiscard]] inline Expected<CommandFrame> BuildFeatureMuteControl(SubunitAddress subunit, uint8_t functionBlockId,
                                                                    uint8_t channel, bool muted) noexcept {
    return FeatureMuteCommand{functionBlockId, channel, muted, subunit}.Encode(CommandType::kControl);
}

[[nodiscard]] inline Expected<bool> ParseFeatureMute(std::span<const uint8_t> operands) noexcept {
    return FeatureMuteCommand::Decode(operands);
}

[[nodiscard]] inline Expected<bool> ParseFeatureMute(const Response& response,
                                                     ResponseCode expected = ResponseCode::kImplementedStable) noexcept {
    if (response.code != expected) {
        return std::unexpected(AvcError::Unexpected(response.code));
    }
    return FeatureMuteCommand::Decode(response.operands);
}

[[nodiscard]] inline Expected<CommandFrame> BuildFeatureVolumeStatus(
    SubunitAddress subunit, uint8_t functionBlockId,
    uint8_t channel,
    ControlAttribute attribute = ControlAttribute::kCurrent) noexcept {
    return FeatureVolumeCommand{functionBlockId, channel, AvcVolume::Invalid(), attribute, subunit}.Encode(CommandType::kStatus);
}

[[nodiscard]] inline Expected<CommandFrame> BuildFeatureVolumeControl(
    SubunitAddress subunit, uint8_t functionBlockId,
    uint8_t channel, int16_t value) noexcept {
    return FeatureVolumeCommand{functionBlockId, channel, AvcVolume::FromRaw(value), ControlAttribute::kCurrent, subunit}.Encode(CommandType::kControl);
}

[[nodiscard]] inline Expected<int16_t> ParseFeatureVolume(std::span<const uint8_t> operands) noexcept {
    auto res = FeatureVolumeCommand::Decode(operands);
    if (!res) return std::unexpected(res.error());
    return res->Raw();
}

[[nodiscard]] inline Expected<int16_t> ParseFeatureVolume(const Response& response,
                                                          ResponseCode expected = ResponseCode::kImplementedStable) noexcept {
    if (response.code != expected) {
        return std::unexpected(AvcError::Unexpected(response.code));
    }
    return ParseFeatureVolume(response.operands);
}

} // namespace ASFW::AVC::Cmd
