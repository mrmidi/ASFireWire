// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioNames.hpp - Spec names for the Audio subunit control vocabulary and replies, for logs and
// diagnostics (TA 1999008).
//
// Same contract as Core/AvcNames.hpp: `name(0xNN)` for a value the table knows, `UNKNOWN(<table>:0xNN)`
// for one it does not. A control value prints as its meaning and its raw wire value: "-3.0000 dB(0xfd00)",
// "TRUE(0x70)", "invalid(0x7fff)". Control selectors print by the spec's identifier (Table A.4).

#pragma once

#include "AudioFunctionBlock.hpp"
#include "ChangeConfigurationCommand.hpp"
#include "CommandNames.hpp"

#include "../Core/AvcNames.hpp"

#include <cstdlib>
#include <string>

namespace ASFW::AVC::Cmd {

namespace names {

// TA 1999008 Table A.2.
inline constexpr std::array kProcessingTypes{
    NameEntry{static_cast<uint32_t>(ProcessingType::kMixer), "MIXER"},
    NameEntry{static_cast<uint32_t>(ProcessingType::kGeneric), "GENERIC"},
    NameEntry{static_cast<uint32_t>(ProcessingType::kUpDownMix), "UP_DOWN"},
    NameEntry{static_cast<uint32_t>(ProcessingType::kDolbyProLogic), "DOLBY_PRO_LOGIC"},
    NameEntry{static_cast<uint32_t>(ProcessingType::kStereoExtender3d), "3-D STEREO EXTENDER"},
    NameEntry{static_cast<uint32_t>(ProcessingType::kReverberation), "REVERBERATION"},
    NameEntry{static_cast<uint32_t>(ProcessingType::kChorus), "CHORUS"},
    NameEntry{static_cast<uint32_t>(ProcessingType::kDynamicRangeCompression), "DYNAMIC_RANGE_COMPRESSION"},
};

// TA 1999008 Table A.3.
inline constexpr std::array kCodecTypes{
    NameEntry{static_cast<uint32_t>(CodecType::kAc3Decoder), "AC3_DECODER"},
    NameEntry{static_cast<uint32_t>(CodecType::kMpegDecoder), "MPEG_DECODER"},
    NameEntry{static_cast<uint32_t>(CodecType::kDtsDecoder), "DTS_DECODER"},
};

// TA 1999008 §10.5.2: the predefined reverberation types, 0..7.
inline constexpr std::array kReverbTypes{
    NameEntry{0, "Room 1"}, NameEntry{1, "Room 2"}, NameEntry{2, "Room 3"}, NameEntry{3, "Hall 1"},
    NameEntry{4, "Hall 2"}, NameEntry{5, "Plate"},  NameEntry{6, "Delay"},  NameEntry{7, "Panning Delay"},
};

} // namespace names

[[nodiscard]] inline std::string Describe(ProcessingType value) {
    return DescribeValue(names::kProcessingTypes, "process_type", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(CodecType value) {
    return DescribeValue(names::kCodecTypes, "codec_type", static_cast<uint32_t>(value), 2);
}
/// A raw process_type or CODEC type byte, named for the block type it belongs to.
[[nodiscard]] inline std::string DescribeSubType(FunctionBlockType block, uint8_t subType) {
    if (block == FunctionBlockType::kProcessing) {
        return DescribeValue(names::kProcessingTypes, "process_type", subType, 2);
    }
    if (block == FunctionBlockType::kCodec) return DescribeValue(names::kCodecTypes, "codec_type", subType, 2);
    return Hex(subType);
}

[[nodiscard]] inline std::string Describe(const ControlId& id) {
    return DescribeControl(id.block, id.subType, id.selector);
}

namespace audio_names_detail {

/// `value` / `divisor` as a decimal with `decimals` digits, rounded to nearest, sign kept.
[[nodiscard]] inline std::string Decimal(int64_t value, int64_t divisor, int decimals) {
    int64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const bool negative = value < 0;
    const int64_t magnitude = negative ? -value : value;
    const int64_t scaled = (magnitude * scale + divisor / 2) / divisor;
    std::string fraction = std::to_string(scaled % scale);
    while (static_cast<int>(fraction.size()) < decimals) fraction.insert(fraction.begin(), '0');
    std::string text = (negative && scaled != 0 ? "-" : "") + std::to_string(scaled / scale);
    if (decimals > 0) text += "." + fraction;
    return text;
}

[[nodiscard]] inline std::string WithRaw(const std::string& meaning, const ControlValue& value) {
    return meaning + "(" + Hex(value.Raw16(), value.width * 2u) + ")";
}

} // namespace audio_names_detail

/// A fixed-width control value by what it means: "-3.0000 dB(0xfd00)", "TRUE(0x70)", "+12.00 dB(0x30)",
/// "invalid(0x7fff)", or UNKNOWN(<kind>:0x..) for a byte the kind does not define.
[[nodiscard]] inline std::string Describe(const ControlValue& value) {
    using audio_names_detail::Decimal;
    using audio_names_detail::WithRaw;
    const auto invalid = [&value] { return WithRaw("invalid", value); };
    const auto signed16 = static_cast<int16_t>(value.Raw16());
    switch (value.kind) {
        case AudioValueKind::kInputPlug:
            return value.bytes[0] == ControlValue::kByteInvalid ? invalid() : WithRaw("fb-plug " + std::to_string(value.bytes[0]), value);
        case AudioValueKind::kBoolean:
            if (value.AsBoolean()) return WithRaw(*value.AsBoolean() ? "TRUE" : "FALSE", value);
            return value.bytes[0] == ControlValue::kByteInvalid ? invalid()
                                                                : "UNKNOWN(boolean:" + Hex(value.bytes[0]) + ")";
        case AudioValueKind::kVolume:
            if (value.Raw16() == ControlValue::kSignedWordInvalid) return invalid();
            if (signed16 == AvcVolume::kNegativeInfinityRaw) return WithRaw("-inf dB", value);
            return WithRaw(Decimal(signed16, 256, 4) + " dB", value);
        case AudioValueKind::kBalance:
            if (value.Raw16() == ControlValue::kSignedWordInvalid) return invalid();
            return WithRaw(Decimal(signed16, 256, 4) + " dB", value);
        case AudioValueKind::kTone:
            if (value.bytes[0] == ControlValue::kToneInvalid) return invalid();
            return WithRaw(Decimal(static_cast<int8_t>(value.bytes[0]) * 25, 100, 2) + " dB", value);
        case AudioValueKind::kDelay:
            if (value.Raw16() == ControlValue::kWordInvalid) return invalid();
            return WithRaw(Decimal(value.Raw16(), 64, 4) + " ms", value);
        case AudioValueKind::kPercent:
            if (value.bytes[0] == ControlValue::kByteInvalid) return invalid();
            return WithRaw(std::to_string(value.bytes[0]) + " %", value);
        case AudioValueKind::kReverbType:
            if (value.bytes[0] == ControlValue::kByteInvalid) return invalid();
            if (const auto name = LookupName(names::kReverbTypes, value.bytes[0])) return WithRaw(std::string(*name), value);
            return WithRaw("type " + std::to_string(value.bytes[0]), value);
        case AudioValueKind::kSeconds:
        case AudioValueKind::kHertz:
        case AudioValueKind::kMilliseconds:
        case AudioValueKind::kRatio: {
            if (value.Raw16() == ControlValue::kWordInvalid) return invalid();
            const char* unit = value.kind == AudioValueKind::kSeconds   ? " s"
                               : value.kind == AudioValueKind::kHertz   ? " Hz"
                               : value.kind == AudioValueKind::kRatio   ? " dB/dB"
                                                                        : " ms";
            return WithRaw(Decimal(value.Raw16(), 256, 4) + unit, value);
        }
        case AudioValueKind::kScale:
            if (value.bytes[0] == ControlValue::kByteInvalid) return invalid();
            return WithRaw(std::to_string(value.bytes[0]) + "/256", value);
        case AudioValueKind::kLowHighScale:
            return "low=" + Hex(value.bytes[0]) + " high=" + Hex(value.bytes[1]);
        case AudioValueKind::kHighLowScale:
            return "high=" + Hex(value.bytes[0]) + " low=" + Hex(value.bytes[1]);
        case AudioValueKind::kRaw8:
            return Hex(value.bytes[0]);
        case AudioValueKind::kGraphicEqualizer:
        case AudioValueKind::kProcessingMode:
        case AudioValueKind::kCodecMode:
        case AudioValueKind::kGuid:
            break;
    }
    return "UNKNOWN(control_value:" + HexBytes(value.Bytes()) + ")";
}

/// A step count (MOVE and DELTA, Table 10.3): "+3 steps(0x0003)", "invalid(0x7fff)".
[[nodiscard]] inline std::string Describe(Steps steps) {
    if (steps.Raw() == ControlValue::kSignedWordInvalid) return "invalid(" + Hex(steps.Raw(), 4) + ")";
    return (steps.count > 0 ? "+" : "") + std::to_string(steps.count) + " steps(" + Hex(steps.Raw(), 4) + ")";
}

/// The Graphic Equalizer block (§10.3.8): "bands=[14,15,18] extra=[] gains=[+1.50,0.00,-2.00] dB".
[[nodiscard]] inline std::string Describe(const GraphicEqualizerBands& bands) {
    std::string bandList;
    for (unsigned bit = 0; bit < 30; ++bit) {
        if (bands.bandsPresent & (1u << bit)) bandList += (bandList.empty() ? "" : ",") + std::to_string(GraphicEqualizerBands::BandNumber(bit));
    }
    std::string extraList;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (bands.extraBandsPresent & (1u << bit)) extraList += (extraList.empty() ? "" : ",") + std::to_string(bit + 1);
    }
    std::string gains;
    for (const uint8_t gain : bands.gains) {
        gains += (gains.empty() ? "" : ",") + Describe(ControlValue::Tone(static_cast<int8_t>(gain)));
    }
    std::string text = "bands=[" + bandList + "] extra=[" + extraList + "] gains=[" + gains + "]";
    if (bands.bandsPresent & ~GraphicEqualizerBands::kBandsPresentMask) text += " reserved_bits_set=" + Hex(bands.bandsPresent, 8);
    if (!bands.IsConsistent()) text += " INCONSISTENT(count)";
    return text;
}

/// A FUNCTION BLOCK reply: "Feature(0x81) id=1 attribute=CURRENT(0x10) data=[0x01] control=VOLUME_CONTROL(0x02) value=...".
/// `subType` is the block's process or CODEC type when the caller knows it.
[[nodiscard]] inline std::string Describe(const FunctionBlockControlReply& reply, uint8_t subType = kAnySubType) {
    std::string text = Describe(reply.type) + " id=" + std::to_string(reply.functionBlockId) +
                       " attribute=" + Describe(reply.attribute);
    if (!reply.audioSelectorData.empty()) text += " selector_data=[" + HexBytes(reply.audioSelectorData) + "]";
    text += " control=" + DescribeControl(reply.type, subType, reply.controlSelector);
    if (reply.type == FunctionBlockType::kSelector) return text;
    const auto* spec = reply.Control(subType);
    if (reply.attribute == ControlAttribute::kMove || reply.attribute == ControlAttribute::kDelta) {
        if (const auto steps = reply.StepCount()) return text + " steps=" + Describe(*steps);
    } else if (spec) {
        if (spec->kind == AudioValueKind::kGraphicEqualizer) {
            if (const auto bands = reply.GraphicEqualizer()) return text + " " + Describe(*bands);
        } else if (spec->kind == AudioValueKind::kProcessingMode) {
            if (const auto mode = reply.ProcessingMode()) return text + " mode=" + std::to_string(*mode);
        } else if (spec->kind == AudioValueKind::kCodecMode) {
            if (const auto mode = reply.CodecMode()) return text + " mode=" + std::to_string(*mode);
        } else if (const auto value = reply.Value(spec->kind)) {
            return text + " value=" + Describe(*value);
        } else if (const auto list = reply.Values(spec->kind); !list.empty()) {
            std::string values;
            for (const auto& v : list) values += (values.empty() ? "" : ", ") + Describe(v);
            return text + " values=[" + values + "]";
        }
    }
    return text + " data=[" + HexBytes(reply.controlData) + "]";
}

/// A configuration_ID: "0x0001", "invalid(0xffff)", "reserved(0x0000)" (Table 11.2).
[[nodiscard]] inline std::string Describe(ConfigurationId id) {
    if (id.Raw() == ConfigurationId::kInvalid) return "invalid(" + Hex(id.Raw(), 4) + ")";
    if (id.Raw() == ConfigurationId::kReserved) return "reserved(" + Hex(id.Raw(), 4) + ")";
    return Hex(id.Raw(), 4);
}

} // namespace ASFW::AVC::Cmd
