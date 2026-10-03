// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioFunctionBlock.hpp - The Audio subunit FUNCTION BLOCK (0xB8) command for every function block
// type: Feature (all twelve controls, one-channel and all-channel forms), Processing (Enable, Mode, Mixer
// and the type-specific controls), CODEC, and Selector.
//
// Source: TA 1999008 §9, §10 (Figures 10.1-10.96, Tables 9.1-9.3, 10.1-10.17, A.4). Mixer and Feature
// layouts are confirmed by Phase 88 captures (documentation/avc-rebuild/fixtures); the CODEC and
// type-specific Processing layouts are spec-only: no reference stack and no captured device uses them.
//
// FUNCTION BLOCK frame (Figure 10.1):
//   [function_block_type][function_block_ID][control_attribute][selector_length = n+1]
//   [audio_selector_data x n][control_selector]  then, for every block but a Selector block:
//   [control data length][control_data]
//   Selector block:  selector data = input fb-plug                                    (Figure 10.2)
//   Feature block:   selector data = audio channel number                             (Figure 10.3)
//   Processing:      selector data = fb-plug, input channel, output channel           (Figure 10.48)
//   CODEC:           no selector data                                                  (Figure 10.96)
//
// Named requests (build these, do not assemble bytes). Every one takes the Audio subunit it is addressed
// to; CONTROL, STATUS and NOTIFY are chosen when the command is encoded.
//   Feature:     SetFeatureControl, QueryFeatureControl, StepFeatureControl,
//                SetFeatureControlAllChannels, QueryFeatureControlAllChannels, SetGraphicEqualizer
//   Processing:  SetProcessingControl, QueryProcessingControl, EnableProcessing, SelectProcessingMode,
//                QueryProcessingMode, SetMixerControl, QueryMixerControl, SetMixerProgrammable,
//                QueryMixerProgrammable, SetMixerAll, QueryMixerAll
//   CODEC:       SetCodecControl, QueryCodecControl, EnableCodec, SelectCodecMode, QueryCodecMode
// The existing typed Selector and Feature operands in FunctionBlockCommand.hpp are the proven path discovery
// uses; they stay as they are.

#pragma once

#include "AudioControlTypes.hpp"

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// Validity of a value in a CONTROL command
// ---------------------------------------------------------------------------

/// True for the encodings the spec calls invalid or reserved ("shall not be used for a control command").
[[nodiscard]] constexpr bool IsInvalidControlValue(const ControlValue& value) noexcept {
    switch (value.kind) {
        case AudioValueKind::kBoolean:
            return value.bytes[0] != kBooleanTrue && value.bytes[0] != kBooleanFalse;
        case AudioValueKind::kInputPlug:
        case AudioValueKind::kPercent:
        case AudioValueKind::kReverbType:
        case AudioValueKind::kScale:
            return value.bytes[0] == ControlValue::kByteInvalid;
        case AudioValueKind::kTone:
            return value.bytes[0] == ControlValue::kToneInvalid;
        case AudioValueKind::kVolume:
        case AudioValueKind::kBalance:
            return value.Raw16() == ControlValue::kSignedWordInvalid;
        case AudioValueKind::kDelay:
        case AudioValueKind::kSeconds:
        case AudioValueKind::kHertz:
        case AudioValueKind::kMilliseconds:
        case AudioValueKind::kRatio:
            return value.Raw16() == ControlValue::kWordInvalid;
        case AudioValueKind::kLowHighScale:
        case AudioValueKind::kHighLowScale:
            return value.bytes[0] == ControlValue::kByteInvalid || value.bytes[1] == ControlValue::kByteInvalid;
        case AudioValueKind::kRaw8:
        case AudioValueKind::kGraphicEqualizer:
        case AudioValueKind::kProcessingMode:
        case AudioValueKind::kCodecMode:
        case AudioValueKind::kGuid:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Graphic equalizer (§10.3.8)
// ---------------------------------------------------------------------------

/// The first-form Graphic Equalizer parameter block: which of the 30 third-octave bands (numbers 14..43,
/// Table 10.11) and 32 extra sixth-octave bands (Table 10.12) it carries, and one gain per set bit in
/// ascending order. The four bytes of each bitmap are read as one big-endian number with bit 0 the
/// least significant; the spec does not say which byte is which (Table 10.13 names only bit numbers).
struct GraphicEqualizerBands {
    static constexpr uint32_t kBandsPresentMask = 0x3FFFFFFF;  ///< bits 30 and 31 are reserved (Table 10.13)
    static constexpr size_t kBitmapBytes = 4;

    uint32_t bandsPresent{0};
    uint32_t extraBandsPresent{0};
    std::vector<uint8_t> gains;  ///< quarter-dB tone gains (Table 10.15), lowest band first

    /// The band number (14..43) of bit `bit` of BandsPresent.
    [[nodiscard]] static constexpr uint8_t BandNumber(unsigned bit) noexcept { return static_cast<uint8_t>(14 + bit); }

    [[nodiscard]] constexpr size_t BandCount() const noexcept {
        return static_cast<size_t>(std::popcount(bandsPresent & kBandsPresentMask)) +
               static_cast<size_t>(std::popcount(extraBandsPresent));
    }
    /// The number of gains must equal the number of bits set, or the target answers NOT IMPLEMENTED (§10.3.8).
    [[nodiscard]] constexpr bool IsConsistent() const noexcept {
        return (bandsPresent & ~kBandsPresentMask) == 0 && gains.size() == BandCount();
    }

    [[nodiscard]] std::vector<uint8_t> ToBytes() const {
        std::vector<uint8_t> bytes;
        bytes.reserve(2 * kBitmapBytes + gains.size());
        for (const uint32_t bitmap : {bandsPresent, extraBandsPresent}) {
            for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(bitmap >> shift));
        }
        bytes.insert(bytes.end(), gains.begin(), gains.end());
        return bytes;
    }
    [[nodiscard]] static std::optional<GraphicEqualizerBands> FromBytes(std::span<const uint8_t> bytes) {
        if (bytes.size() < 2 * kBitmapBytes) return std::nullopt;
        const auto word = [&bytes](size_t at) {
            return (static_cast<uint32_t>(bytes[at]) << 24) | (static_cast<uint32_t>(bytes[at + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
        };
        GraphicEqualizerBands bands{word(0), word(kBitmapBytes), {bytes.begin() + 2 * kBitmapBytes, bytes.end()}};
        return bands;
    }
};

// ---------------------------------------------------------------------------
// The generic FUNCTION BLOCK operands and reply
// ---------------------------------------------------------------------------

struct FunctionBlockControlReply {
    FunctionBlockType type{FunctionBlockType::kFeature};
    uint8_t functionBlockId{0};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::vector<uint8_t> audioSelectorData;
    uint8_t controlSelector{0};
    std::vector<uint8_t> controlData;  ///< empty for a Selector block, which has none

    /// The catalogue entry of the control, given the block's process or CODEC type (kAnySubType for
    /// Selector and Feature blocks, and when it is not known: Enable and Mode then still resolve).
    [[nodiscard]] const ControlSpec* Control(uint8_t subType = kAnySubType) const noexcept {
        return FindControl(type, subType, controlSelector);
    }

    /// The control data read as a fixed-width value of `kind`; nullopt when the length does not match.
    [[nodiscard]] std::optional<ControlValue> Value(AudioValueKind kind) const noexcept {
        const auto width = FixedWidth(kind);
        if (!width || controlData.size() != *width) return std::nullopt;
        ControlValue value{kind, *width, {}};
        std::copy(controlData.begin(), controlData.end(), value.bytes.begin());
        return value;
    }
    /// A second-form or third-form reply: the control data as consecutive values of `kind`.
    [[nodiscard]] std::vector<ControlValue> Values(AudioValueKind kind) const {
        std::vector<ControlValue> values;
        const auto width = FixedWidth(kind);
        if (!width || controlData.size() % *width != 0) return values;
        for (size_t at = 0; at < controlData.size(); at += *width) {
            ControlValue value{kind, *width, {}};
            std::copy_n(controlData.begin() + static_cast<std::ptrdiff_t>(at), *width, value.bytes.begin());
            values.push_back(value);
        }
        return values;
    }
    /// A MOVE or DELTA reply: a signed step count; nullopt for 7FFF (invalid) or a wrong length.
    [[nodiscard]] std::optional<Steps> StepCount() const noexcept {
        if (controlData.size() != 2) return std::nullopt;
        const auto raw = static_cast<uint16_t>((controlData[0] << 8) | controlData[1]);
        if (raw == ControlValue::kSignedWordInvalid) return std::nullopt;
        return Steps::Of(static_cast<int16_t>(raw));
    }
    /// A Processing Mode reply (§10.4.2.2): Size_of_modes, then that many bytes of mode number, big-endian.
    [[nodiscard]] std::optional<uint32_t> ProcessingMode() const noexcept {
        if (controlData.empty() || controlData.size() != 1u + controlData[0] || controlData[0] == 0 ||
            controlData[0] > sizeof(uint32_t)) {
            return std::nullopt;
        }
        uint32_t mode = 0;
        for (size_t i = 1; i < controlData.size(); ++i) mode = (mode << 8) | controlData[i];
        return mode;
    }
    /// A CODEC Mode reply (§10.6.3): the mode bytes, big-endian.
    [[nodiscard]] std::optional<uint32_t> CodecMode() const noexcept {
        if (controlData.empty() || controlData.size() > sizeof(uint32_t)) return std::nullopt;
        uint32_t mode = 0;
        for (const uint8_t b : controlData) mode = (mode << 8) | b;
        return mode;
    }
    [[nodiscard]] std::optional<GraphicEqualizerBands> GraphicEqualizer() const {
        return GraphicEqualizerBands::FromBytes(controlData);
    }
};

struct FunctionBlockControlOperands {
    static constexpr Opcode kOpcode = Opcode::kFunctionBlock;

    FunctionBlockType type{FunctionBlockType::kFeature};
    uint8_t functionBlockId{0};
    ControlAttribute attribute{ControlAttribute::kCurrent};
    std::vector<uint8_t> audioSelectorData;
    ControlId control{};
    std::vector<uint8_t> controlData;  ///< what CONTROL sends; empty for a Selector block
    std::vector<uint8_t> statusData;   ///< what STATUS and NOTIFY send: FF placeholders (§10.2, §10.3.1, §10.4.2.2)
    /// The kind the supplied value was built as; Write refuses it unless it is the catalogue's kind for
    /// `control`. Unset for the variable-width controls, whose bytes are laid out by their own builder.
    std::optional<AudioValueKind> valueKind{std::nullopt};
    /// True for MOVE and DELTA, whose data is a step count and not a value of the control's kind.
    bool isStepCount{false};

    using Reply = FunctionBlockControlReply;

    [[nodiscard]] Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        if (address.IsUnit() || address.Type() != SubunitType::kAudio) return Fail(AvcErrorKind::kInvalidArgument);
        return {};
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        const bool isControl = t == CommandType::kControl;
        if (!(isControl || t == CommandType::kStatus || t == CommandType::kNotify)) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        if (control.block != type || audioSelectorData.size() + 1 > 0xFF) return Fail(AvcErrorKind::kInvalidArgument);
        const bool hasData = type != FunctionBlockType::kSelector;
        const auto& data = isControl ? controlData : statusData;
        if (hasData && data.empty()) return Fail(AvcErrorKind::kInvalidArgument);
        if (hasData && data.size() > 0xFF) return Fail(AvcErrorKind::kInvalidArgument);
        // A fixed-width value must be the kind the catalogue says this control carries, and a CONTROL must
        // not send an invalid encoding.
        if (hasData && valueKind) {
            const auto* spec = FindControl(control);
            if (!spec || (!isStepCount && spec->kind != *valueKind)) return Fail(AvcErrorKind::kInvalidArgument);
            const auto width = FixedWidth(*valueKind);
            if (!width || data.size() != *width) return Fail(AvcErrorKind::kInvalidArgument);
            if (isControl && !isStepCount) {
                ControlValue value{*valueKind, *width, {}};
                std::copy(data.begin(), data.end(), value.bytes.begin());
                if (IsInvalidControlValue(value)) return Fail(AvcErrorKind::kInvalidArgument);
            }
        }

        const std::array<uint8_t, 4> header = {static_cast<uint8_t>(type), functionBlockId,
                                               static_cast<uint8_t>(attribute),
                                               static_cast<uint8_t>(audioSelectorData.size() + 1)};
        if (auto written = w.Append(header); !written) return written;
        if (auto written = w.Append(std::span<const uint8_t>{audioSelectorData}); !written) return written;
        if (auto written = w.Append(control.selector); !written) return written;
        if (!hasData) return {};
        if (auto written = w.Append(static_cast<uint8_t>(data.size())); !written) return written;
        return w.Append(std::span<const uint8_t>{data});
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        constexpr size_t kFixedBytes = 4;  // type, ID, attribute, selector_length
        if (in.size() < kFixedBytes) return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        const size_t selectorLength = in[3];
        if (selectorLength == 0) return FailAt(AvcErrorKind::kMalformedOperands, 3);
        if (in.size() < kFixedBytes + selectorLength) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        Reply reply;
        reply.type = static_cast<FunctionBlockType>(in[0]);
        reply.functionBlockId = in[1];
        reply.attribute = static_cast<ControlAttribute>(in[2]);
        const auto selectorData = in.subspan(kFixedBytes, selectorLength - 1);
        reply.audioSelectorData.assign(selectorData.begin(), selectorData.end());
        reply.controlSelector = in[kFixedBytes + selectorLength - 1];
        if (reply.type == FunctionBlockType::kSelector) return reply;

        const size_t lengthAt = kFixedBytes + selectorLength;
        if (in.size() < lengthAt + 1) return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        const size_t dataLength = in[lengthAt];
        if (in.size() < lengthAt + 1 + dataLength) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        const auto data = in.subspan(lengthAt + 1, dataLength);
        reply.controlData.assign(data.begin(), data.end());
        return reply;
    }
};

using FunctionBlockControlCommand = Command<FunctionBlockControlOperands>;

namespace audio_detail {

inline constexpr uint8_t kInvalidByte = ControlValue::kByteInvalid;

[[nodiscard]] inline std::vector<uint8_t> Placeholder(size_t width) { return std::vector<uint8_t>(width, kInvalidByte); }

[[nodiscard]] inline std::vector<uint8_t> BytesOf(const ControlValue& value) {
    const auto bytes = value.Bytes();
    return {bytes.begin(), bytes.end()};
}

/// A control addressed with a fixed-width value: CONTROL carries the value, STATUS carries FF placeholders
/// of the catalogue's width.
[[nodiscard]] inline FunctionBlockControlOperands Fixed(FunctionBlockType type, uint8_t fbId,
                                                        ControlAttribute attribute,
                                                        std::vector<uint8_t> selectorData, ControlId control,
                                                        std::optional<ControlValue> value) {
    FunctionBlockControlOperands ops;
    ops.type = type;
    ops.functionBlockId = fbId;
    ops.attribute = attribute;
    ops.audioSelectorData = std::move(selectorData);
    ops.control = control;
    const auto* spec = FindControl(control);
    const auto width = spec ? FixedWidth(spec->kind) : std::nullopt;
    if (value) {
        ops.controlData = BytesOf(*value);
        ops.valueKind = value->kind;
        ops.statusData = Placeholder(value->width);
    } else {
        ops.valueKind = spec ? std::optional<AudioValueKind>{spec->kind} : std::nullopt;
        ops.statusData = Placeholder(width.value_or(0));
    }
    return ops;
}

} // namespace audio_detail

// ---------------------------------------------------------------------------
// Feature function block (§10.3)
// ---------------------------------------------------------------------------

/// CONTROL: set `feature` of `channel` (0 = master) to `value` (first form). `attribute` is CURRENT unless a
/// target documents otherwise (§10.3).
[[nodiscard]] inline FunctionBlockControlCommand SetFeatureControl(SubunitAddress audio, uint8_t functionBlockId,
                                                                   uint8_t channel, ControlId feature,
                                                                   ControlValue value,
                                                                   ControlAttribute attribute = ControlAttribute::kCurrent) {
    return {.address = audio,
            .operands = audio_detail::Fixed(FunctionBlockType::kFeature, functionBlockId, attribute, {channel},
                                            feature, value)};
}

/// STATUS or NOTIFY: read `feature` of `channel` (first form).
[[nodiscard]] inline FunctionBlockControlCommand QueryFeatureControl(
    SubunitAddress audio, uint8_t functionBlockId, uint8_t channel, ControlId feature,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return {.address = audio,
            .operands = audio_detail::Fixed(FunctionBlockType::kFeature, functionBlockId, attribute, {channel},
                                            feature, std::nullopt)};
}

/// CONTROL with MOVE or DELTA: change the control by `steps` (§9.1.4, Table 10.3). Only controls whose
/// value is two bytes (volume, balance, delay) take a step count.
[[nodiscard]] inline FunctionBlockControlCommand StepFeatureControl(SubunitAddress audio, uint8_t functionBlockId,
                                                                    uint8_t channel, ControlId feature,
                                                                    ControlAttribute attribute, Steps steps) {
    auto ops = audio_detail::Fixed(FunctionBlockType::kFeature, functionBlockId, attribute, {channel}, feature,
                                   std::nullopt);
    const bool stepAttribute = attribute == ControlAttribute::kMove || attribute == ControlAttribute::kDelta;
    ops.controlData = {HighByte(steps.Raw()), LowByte(steps.Raw())};
    ops.isStepCount = true;
    if (!stepAttribute) ops.controlData.clear();  // Write refuses it
    return {.address = audio, .operands = std::move(ops)};
}

/// CONTROL, second form: set `feature` on every channel at once; `values` must be exactly the number of
/// controls the block has (§10.3, "NrAv"), in ascending channel order.
[[nodiscard]] inline FunctionBlockControlCommand SetFeatureControlAllChannels(
    SubunitAddress audio, uint8_t functionBlockId, ControlId feature, std::span<const ControlValue> values,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    FunctionBlockControlOperands ops;
    ops.type = FunctionBlockType::kFeature;
    ops.functionBlockId = functionBlockId;
    ops.attribute = attribute;
    ops.audioSelectorData = {kAudioChannelAll};
    ops.control = feature;
    const auto* spec = FindControl(feature);
    bool kindsMatch = spec != nullptr;
    for (const auto& value : values) {
        kindsMatch = kindsMatch && value.kind == spec->kind && !IsInvalidControlValue(value);
        const auto bytes = value.Bytes();
        ops.controlData.insert(ops.controlData.end(), bytes.begin(), bytes.end());
    }
    ops.statusData = audio_detail::Placeholder(ops.controlData.size());
    if (!kindsMatch) ops.controlData.clear();  // Write refuses it
    return {.address = audio, .operands = std::move(ops)};
}

/// STATUS, second form: read `feature` on all `count` channels of the block.
[[nodiscard]] inline FunctionBlockControlCommand QueryFeatureControlAllChannels(
    SubunitAddress audio, uint8_t functionBlockId, ControlId feature, size_t count,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    FunctionBlockControlOperands ops;
    ops.type = FunctionBlockType::kFeature;
    ops.functionBlockId = functionBlockId;
    ops.attribute = attribute;
    ops.audioSelectorData = {kAudioChannelAll};
    ops.control = feature;
    const auto* spec = FindControl(feature);
    const auto width = spec ? FixedWidth(spec->kind).value_or(0) : 0;
    ops.statusData = audio_detail::Placeholder(count * width);
    return {.address = audio, .operands = std::move(ops)};
}

/// CONTROL: set the Graphic Equalizer of `channel` (first form only, §10.3.8).
[[nodiscard]] inline FunctionBlockControlCommand SetGraphicEqualizer(SubunitAddress audio, uint8_t functionBlockId,
                                                                     uint8_t channel,
                                                                     const GraphicEqualizerBands& bands) {
    FunctionBlockControlOperands ops;
    ops.type = FunctionBlockType::kFeature;
    ops.functionBlockId = functionBlockId;
    ops.audioSelectorData = {channel};
    ops.control = controls::kGraphicEqualizerControl;
    ops.controlData = bands.ToBytes();
    ops.statusData = ops.controlData;
    // The count of gains must match the bitmaps; an inconsistent block is not sent.
    if (!bands.IsConsistent()) ops.controlData.clear();
    return {.address = audio, .operands = std::move(ops)};
}

// ---------------------------------------------------------------------------
// Processing function block (§10.4)
// ---------------------------------------------------------------------------

/// Which fb-plug and channels a Processing control addresses (Figure 10.48). A control that does not act
/// per channel takes kAudioChannelVoid for the input channel and, when the output channel has no meaning,
/// for the output channel too (§10.4).
struct ProcessingTarget {
    uint8_t inputPlug{kSingleInputFbPlug};
    uint8_t inputChannel{kAudioChannelVoid};
    uint8_t outputChannel{kAudioChannelVoid};

    /// A control on the block as a whole (Enable, Mode, ...): both channels VOID.
    [[nodiscard]] static constexpr ProcessingTarget Block() noexcept { return ProcessingTarget{}; }
    /// The master controls of the output: input VOID, output channel 0 (§10.4).
    [[nodiscard]] static constexpr ProcessingTarget MasterOutput() noexcept {
        return ProcessingTarget{kSingleInputFbPlug, kAudioChannelVoid, kAudioChannelMaster};
    }
    /// A per-output-channel control: input VOID, one output channel.
    [[nodiscard]] static constexpr ProcessingTarget OutputChannel(uint8_t channel) noexcept {
        return ProcessingTarget{kSingleInputFbPlug, kAudioChannelVoid, channel};
    }
};

namespace audio_detail {

[[nodiscard]] inline std::vector<uint8_t> ProcessingSelector(const ProcessingTarget& t) {
    return {t.inputPlug, t.inputChannel, t.outputChannel};
}

} // namespace audio_detail

/// CONTROL: set a Processing control to a fixed-width value.
[[nodiscard]] inline FunctionBlockControlCommand SetProcessingControl(
    SubunitAddress audio, uint8_t functionBlockId, ProcessingTarget target, ControlId control, ControlValue value,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return {.address = audio,
            .operands = audio_detail::Fixed(FunctionBlockType::kProcessing, functionBlockId, attribute,
                                            audio_detail::ProcessingSelector(target), control, value)};
}

/// STATUS or NOTIFY: read a Processing control that carries a fixed-width value.
[[nodiscard]] inline FunctionBlockControlCommand QueryProcessingControl(
    SubunitAddress audio, uint8_t functionBlockId, ProcessingTarget target, ControlId control,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return {.address = audio,
            .operands = audio_detail::Fixed(FunctionBlockType::kProcessing, functionBlockId, attribute,
                                            audio_detail::ProcessingSelector(target), control, std::nullopt)};
}

/// CONTROL: enable the block, or bypass it (§10.4.2.1).
[[nodiscard]] inline FunctionBlockControlCommand EnableProcessing(SubunitAddress audio, uint8_t functionBlockId,
                                                                  bool enabled) {
    return SetProcessingControl(audio, functionBlockId, ProcessingTarget::Block(), controls::kEnableProcessingControl,
                                ControlValue::Boolean(enabled));
}

/// CONTROL: select mode 1..number_of_modes (§10.4.2.2). `modeBytes` is Size_of_modes, the width of the mode
/// number, normally 1.
[[nodiscard]] inline FunctionBlockControlCommand SelectProcessingMode(SubunitAddress audio, uint8_t functionBlockId,
                                                                      uint32_t mode, uint8_t modeBytes = 1) {
    FunctionBlockControlOperands ops;
    ops.type = FunctionBlockType::kProcessing;
    ops.functionBlockId = functionBlockId;
    ops.audioSelectorData = audio_detail::ProcessingSelector(ProcessingTarget::Block());
    ops.control = controls::kProcessingModeControl;
    // Size_of_modes is the width of the mode number: 1..4 bytes, big-endian. Anything else is not sent.
    if (modeBytes != 0 && modeBytes <= sizeof(uint32_t)) {
        ops.controlData.push_back(modeBytes);
        for (int i = modeBytes - 1; i >= 0; --i) ops.controlData.push_back(static_cast<uint8_t>(mode >> (8 * i)));
        ops.statusData.assign(1u + modeBytes, audio_detail::kInvalidByte);
        ops.statusData.front() = modeBytes;
    }
    return {.address = audio, .operands = std::move(ops)};
}

/// STATUS or NOTIFY: the current mode; every mode byte is FF in the request (§10.4.2.2).
[[nodiscard]] inline FunctionBlockControlCommand QueryProcessingMode(SubunitAddress audio, uint8_t functionBlockId,
                                                                     uint8_t modeBytes = 1) {
    auto command = SelectProcessingMode(audio, functionBlockId, 0, modeBytes);
    command.operands.controlData.clear();  // there is no CONTROL form of a query
    return command;
}

// --- Mixer (§10.4.3, §10.4.4) ---------------------------------------------------------------------

/// CONTROL: set the mixer gain from input channel `inputChannel` of fb-plug `inputPlug` to output channel
/// `outputChannel` (first form).
[[nodiscard]] inline FunctionBlockControlCommand SetMixerControl(
    SubunitAddress audio, uint8_t functionBlockId, uint8_t inputPlug, uint8_t inputChannel, uint8_t outputChannel,
    AvcVolume setting, ControlAttribute attribute = ControlAttribute::kCurrent) {
    return SetProcessingControl(audio, functionBlockId, ProcessingTarget{inputPlug, inputChannel, outputChannel},
                                controls::kMixerControl, ControlValue::Volume(setting), attribute);
}

/// STATUS or NOTIFY: the mixer gain of one input/output channel pair.
[[nodiscard]] inline FunctionBlockControlCommand QueryMixerControl(
    SubunitAddress audio, uint8_t functionBlockId, uint8_t inputPlug, uint8_t inputChannel, uint8_t outputChannel,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return QueryProcessingControl(audio, functionBlockId, ProcessingTarget{inputPlug, inputChannel, outputChannel},
                                  controls::kMixerControl, attribute);
}

namespace audio_detail {

[[nodiscard]] inline FunctionBlockControlCommand MixerList(SubunitAddress audio, uint8_t functionBlockId,
                                                           uint8_t channel, std::span<const AvcVolume> settings,
                                                           size_t count, ControlAttribute attribute) {
    FunctionBlockControlOperands ops;
    ops.type = FunctionBlockType::kProcessing;
    ops.functionBlockId = functionBlockId;
    ops.attribute = attribute;
    ops.audioSelectorData = {kSingleInputFbPlug, channel, channel};
    ops.control = controls::kMixerControl;
    for (const auto& setting : settings) {
        const auto raw = static_cast<uint16_t>(setting.Raw());
        ops.controlData.push_back(HighByte(raw));
        ops.controlData.push_back(LowByte(raw));
    }
    ops.statusData = Placeholder(count * 2);
    return {.address = audio, .operands = std::move(ops)};
}

} // namespace audio_detail

/// CONTROL, second form: set every programmable mixer control, in the bit order of the block's Controls
/// bitmap (input and output channel both FF).
[[nodiscard]] inline FunctionBlockControlCommand SetMixerProgrammable(
    SubunitAddress audio, uint8_t functionBlockId, std::span<const AvcVolume> settings,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return audio_detail::MixerList(audio, functionBlockId, kAudioChannelAll, settings, settings.size(), attribute);
}

/// STATUS, second form: read all `count` programmable mixer controls.
[[nodiscard]] inline FunctionBlockControlCommand QueryMixerProgrammable(
    SubunitAddress audio, uint8_t functionBlockId, size_t count,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return audio_detail::MixerList(audio, functionBlockId, kAudioChannelAll, {}, count, attribute);
}

/// CONTROL, third form: set every mixer control, programmable or not (input and output channel both 00).
[[nodiscard]] inline FunctionBlockControlCommand SetMixerAll(
    SubunitAddress audio, uint8_t functionBlockId, std::span<const AvcVolume> settings,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return audio_detail::MixerList(audio, functionBlockId, kAudioChannelMaster, settings, settings.size(), attribute);
}

/// STATUS, third form: read all `count` mixer controls.
[[nodiscard]] inline FunctionBlockControlCommand QueryMixerAll(
    SubunitAddress audio, uint8_t functionBlockId, size_t count,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return audio_detail::MixerList(audio, functionBlockId, kAudioChannelMaster, {}, count, attribute);
}

// ---------------------------------------------------------------------------
// CODEC function block (§10.6)
// ---------------------------------------------------------------------------

/// CONTROL: set a CODEC control to a fixed-width value. A CODEC command has no selector data (Figure 10.96).
[[nodiscard]] inline FunctionBlockControlCommand SetCodecControl(
    SubunitAddress audio, uint8_t functionBlockId, ControlId control, ControlValue value,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return {.address = audio,
            .operands = audio_detail::Fixed(FunctionBlockType::kCodec, functionBlockId, attribute, {}, control, value)};
}

/// STATUS or NOTIFY: read a CODEC control that carries a fixed-width value.
[[nodiscard]] inline FunctionBlockControlCommand QueryCodecControl(
    SubunitAddress audio, uint8_t functionBlockId, ControlId control,
    ControlAttribute attribute = ControlAttribute::kCurrent) {
    return {.address = audio,
            .operands = audio_detail::Fixed(FunctionBlockType::kCodec, functionBlockId, attribute, {}, control,
                                            std::nullopt)};
}

/// CONTROL: enable the CODEC, or bypass it (§10.6.2).
[[nodiscard]] inline FunctionBlockControlCommand EnableCodec(SubunitAddress audio, uint8_t functionBlockId,
                                                             bool enabled) {
    return SetCodecControl(audio, functionBlockId, controls::kEnableCodecControl, ControlValue::Boolean(enabled));
}

/// CONTROL: select CODEC mode 1..NrModes (§10.6.3). The CODEC's mode data is the mode bytes alone, with no
/// Size_of_modes field (Figure 10.99), unlike the Processing block's.
[[nodiscard]] inline FunctionBlockControlCommand SelectCodecMode(SubunitAddress audio, uint8_t functionBlockId,
                                                                 uint8_t mode) {
    FunctionBlockControlOperands ops;
    ops.type = FunctionBlockType::kCodec;
    ops.functionBlockId = functionBlockId;
    ops.control = controls::kCodecModeControl;
    ops.controlData = {mode};
    ops.statusData = {audio_detail::kInvalidByte};
    return {.address = audio, .operands = std::move(ops)};
}

/// STATUS or NOTIFY: the current CODEC mode.
[[nodiscard]] inline FunctionBlockControlCommand QueryCodecMode(SubunitAddress audio, uint8_t functionBlockId) {
    auto command = SelectCodecMode(audio, functionBlockId, 0);
    command.operands.controlData.clear();
    return command;
}

} // namespace ASFW::AVC::Cmd
