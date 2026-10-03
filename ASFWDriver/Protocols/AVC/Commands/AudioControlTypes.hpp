// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioControlTypes.hpp - The Audio subunit control vocabulary: processing and CODEC types, the control
// catalogue (which control_selector means what in which function block), and the value types a control
// carries.
//
// Source: TA Document 1999008, AV/C Audio Subunit Specification 1.0 (24 Oct 2000), referred to below as
// "Audio". Section, Table and Figure numbers are that document's. Cross-checked against ta1394
// audio/src/lib.rs (the Feature controls) and the Phase 88 / Duet captures for the Feature and Mixer
// layouts. Fresh implementation; no reference code copied.
//
// Contract:
// - A control is named by its ControlId (function block type, sub-type, control_selector): the same
//   control_selector value means different things in different blocks (Audio Table A.4), so the number alone
//   is never enough.
// - A value is a typed ControlValue built by a named factory and checked against the catalogue's value
//   kind. A raw byte only exists inside these types.
// - Reading is lenient: every field keeps its raw bytes. A named view returns std::nullopt for an
//   invalid or reserved value.

#pragma once

#include "FunctionBlockCommand.hpp"

#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// Channel numbers (Audio §10.3, §10.4)
// ---------------------------------------------------------------------------

/// audio_channel_number "VOID": the field has no meaning for this control_selector.
inline constexpr uint8_t kAudioChannelVoid = 0xFE;
/// audio_channel_number "all": a Feature command addresses every control of the type (second form), and a
/// Mixer command's input and output channel addresses every programmable mixer control (second form).
inline constexpr uint8_t kAudioChannelAll = 0xFF;
/// Mixer third form: input and output channel are both 00 and the data holds every mixer control (§10.4.3).
inline constexpr uint8_t kAudioChannelMaster = kMasterChannel;

/// A Processing block with a single input fb-plug addresses it as 00 (§10.4).
inline constexpr uint8_t kSingleInputFbPlug = 0x00;

// ---------------------------------------------------------------------------
// Processing and CODEC types (Audio Tables A.2, A.3)
// ---------------------------------------------------------------------------

enum class ProcessingType : uint8_t {
    kMixer = 0x01,
    kGeneric = 0x02,
    kUpDownMix = 0x03,
    kDolbyProLogic = 0x04,
    kStereoExtender3d = 0x05,
    kReverberation = 0x06,
    kChorus = 0x07,
    kDynamicRangeCompression = 0x08,
};

enum class CodecType : uint8_t {
    kAc3Decoder = 0x01,
    kMpegDecoder = 0x02,
    kDtsDecoder = 0x03,
};

/// The sub-type slot of a ControlId that means "any sub-type of this block" (Audio Table A.4: Enable and
/// Mode apply to every Processing and CODEC block). Process and CODEC types start at 1.
inline constexpr uint8_t kAnySubType = 0x00;

// ---------------------------------------------------------------------------
// Control identity (Audio Table A.4)
// ---------------------------------------------------------------------------

struct ControlId {
    FunctionBlockType block{FunctionBlockType::kFeature};
    uint8_t subType{kAnySubType};  ///< ProcessingType or CodecType; kAnySubType for Selector and Feature blocks
    uint8_t selector{0};           ///< control_selector

    friend constexpr bool operator==(const ControlId&, const ControlId&) noexcept = default;
};

namespace controls {

constexpr ControlId Make(FunctionBlockType block, uint8_t subType, uint8_t selector) noexcept {
    return ControlId{block, subType, selector};
}
constexpr ControlId Make(FunctionBlockType block, ProcessingType type, uint8_t selector) noexcept {
    return ControlId{block, static_cast<uint8_t>(type), selector};
}
constexpr ControlId Make(FunctionBlockType block, CodecType type, uint8_t selector) noexcept {
    return ControlId{block, static_cast<uint8_t>(type), selector};
}

inline constexpr auto kS = FunctionBlockType::kSelector;
inline constexpr auto kF = FunctionBlockType::kFeature;
inline constexpr auto kP = FunctionBlockType::kProcessing;
inline constexpr auto kC = FunctionBlockType::kCodec;

// Selector block
inline constexpr ControlId kSelectorControl = Make(kS, kAnySubType, 0x01);
// Feature block (Audio §10.3)
inline constexpr ControlId kMuteControl = Make(kF, kAnySubType, 0x01);
inline constexpr ControlId kVolumeControl = Make(kF, kAnySubType, 0x02);
inline constexpr ControlId kLrBalanceControl = Make(kF, kAnySubType, 0x03);
inline constexpr ControlId kFrBalanceControl = Make(kF, kAnySubType, 0x04);
inline constexpr ControlId kBassControl = Make(kF, kAnySubType, 0x05);
inline constexpr ControlId kMidControl = Make(kF, kAnySubType, 0x06);
inline constexpr ControlId kTrebleControl = Make(kF, kAnySubType, 0x07);
inline constexpr ControlId kGraphicEqualizerControl = Make(kF, kAnySubType, 0x08);
inline constexpr ControlId kAutomaticGainControl = Make(kF, kAnySubType, 0x09);
inline constexpr ControlId kDelayControl = Make(kF, kAnySubType, 0x0A);
inline constexpr ControlId kBassBoostControl = Make(kF, kAnySubType, 0x0B);
inline constexpr ControlId kLoudnessControl = Make(kF, kAnySubType, 0x0C);
// Processing block, every type (Audio §10.4.2)
inline constexpr ControlId kEnableProcessingControl = Make(kP, kAnySubType, 0x01);
inline constexpr ControlId kProcessingModeControl = Make(kP, kAnySubType, 0x02);
// Mixer (§10.4.4)
inline constexpr ControlId kMixerControl = Make(kP, ProcessingType::kMixer, 0x03);
// Generic processing (Table A.4)
inline constexpr ControlId kGuidControl = Make(kP, ProcessingType::kGeneric, 0x03);
// 3D stereo extender (§10.5.1)
inline constexpr ControlId kSpaciousnessControl = Make(kP, ProcessingType::kStereoExtender3d, 0x03);
// Reverberation (§10.5.2-§10.5.6)
inline constexpr ControlId kReverbTypeControl = Make(kP, ProcessingType::kReverberation, 0x03);
inline constexpr ControlId kReverbLevelControl = Make(kP, ProcessingType::kReverberation, 0x04);
inline constexpr ControlId kReverbTimeControl = Make(kP, ProcessingType::kReverberation, 0x05);
inline constexpr ControlId kReverbEarlyTimeControl = Make(kP, ProcessingType::kReverberation, 0x06);
inline constexpr ControlId kReverbDelayFeedbackControl = Make(kP, ProcessingType::kReverberation, 0x07);
// Chorus (§10.5.8-§10.5.9). §10.5.7 defines a Chorus Level control and Table 8.12 gives it bit 1 of the
// Controls field, but Table A.4 assigns it no control_selector, so it has no ControlId here.
inline constexpr ControlId kChorusRateControl = Make(kP, ProcessingType::kChorus, 0x03);
inline constexpr ControlId kChorusDepthControl = Make(kP, ProcessingType::kChorus, 0x04);
// Dynamic range compressor (§10.5.10-§10.5.14)
inline constexpr ControlId kCompressionRatioControl = Make(kP, ProcessingType::kDynamicRangeCompression, 0x03);
inline constexpr ControlId kMaxAmplControl = Make(kP, ProcessingType::kDynamicRangeCompression, 0x04);
inline constexpr ControlId kThresholdControl = Make(kP, ProcessingType::kDynamicRangeCompression, 0x05);
inline constexpr ControlId kAttackTimeControl = Make(kP, ProcessingType::kDynamicRangeCompression, 0x06);
inline constexpr ControlId kReleaseTimeControl = Make(kP, ProcessingType::kDynamicRangeCompression, 0x07);
// CODEC block, every type (§10.6.2, §10.6.3)
inline constexpr ControlId kEnableCodecControl = Make(kC, kAnySubType, 0x01);
inline constexpr ControlId kCodecModeControl = Make(kC, kAnySubType, 0x02);
// MPEG decoder (§10.6.4.2)
inline constexpr ControlId kMpegDualChannelControl = Make(kC, CodecType::kMpegDecoder, 0x03);
inline constexpr ControlId kMpegSecondStereoControl = Make(kC, CodecType::kMpegDecoder, 0x04);
inline constexpr ControlId kMpegMultilingualControl = Make(kC, CodecType::kMpegDecoder, 0x05);
inline constexpr ControlId kMpegDynamicRangeControl = Make(kC, CodecType::kMpegDecoder, 0x06);
inline constexpr ControlId kMpegScaleControl = Make(kC, CodecType::kMpegDecoder, 0x07);
inline constexpr ControlId kMpegHighLowScalingControl = Make(kC, CodecType::kMpegDecoder, 0x08);
// AC-3 decoder (§10.6.4.3)
inline constexpr ControlId kAc3DynamicRangeControl = Make(kC, CodecType::kAc3Decoder, 0x03);
inline constexpr ControlId kAc3ScaleControl = Make(kC, CodecType::kAc3Decoder, 0x04);
inline constexpr ControlId kAc3HighLowScalingControl = Make(kC, CodecType::kAc3Decoder, 0x05);
inline constexpr ControlId kAc3DualMonoControl = Make(kC, CodecType::kAc3Decoder, 0x06);
inline constexpr ControlId kAc3DolbySurroundControl = Make(kC, CodecType::kAc3Decoder, 0x07);
inline constexpr ControlId kAc3FrameErrorStatusControl = Make(kC, CodecType::kAc3Decoder, 0x08);
inline constexpr ControlId kAc3SampleRateStatusControl = Make(kC, CodecType::kAc3Decoder, 0x09);
inline constexpr ControlId kAc3DataRateStatusControl = Make(kC, CodecType::kAc3Decoder, 0x0A);
inline constexpr ControlId kAc3LfeOnStatusControl = Make(kC, CodecType::kAc3Decoder, 0x0B);
inline constexpr ControlId kAc3ModStatusControl = Make(kC, CodecType::kAc3Decoder, 0x0C);
inline constexpr ControlId kAc3BsidStatusControl = Make(kC, CodecType::kAc3Decoder, 0x0D);
inline constexpr ControlId kAc3BsmodStatusControl = Make(kC, CodecType::kAc3Decoder, 0x0E);
inline constexpr ControlId kAc3CmixlevStatusControl = Make(kC, CodecType::kAc3Decoder, 0x0F);
inline constexpr ControlId kAc3SmixlevStatusControl = Make(kC, CodecType::kAc3Decoder, 0x10);
inline constexpr ControlId kAc3DsurStatusControl = Make(kC, CodecType::kAc3Decoder, 0x11);
inline constexpr ControlId kAc3CpyrtStatusControl = Make(kC, CodecType::kAc3Decoder, 0x12);
inline constexpr ControlId kAc3OrgnlStatusControl = Make(kC, CodecType::kAc3Decoder, 0x13);
inline constexpr ControlId kAc3DialnormStatusControl = Make(kC, CodecType::kAc3Decoder, 0x14);
inline constexpr ControlId kAc3Dialnorm2StatusControl = Make(kC, CodecType::kAc3Decoder, 0x15);
inline constexpr ControlId kAc3MixlevStatusControl = Make(kC, CodecType::kAc3Decoder, 0x16);
inline constexpr ControlId kAc3Mixlev2StatusControl = Make(kC, CodecType::kAc3Decoder, 0x17);
inline constexpr ControlId kAc3RoomtypeStatusControl = Make(kC, CodecType::kAc3Decoder, 0x18);
inline constexpr ControlId kAc3Roomtype2StatusControl = Make(kC, CodecType::kAc3Decoder, 0x19);

} // namespace controls

// ---------------------------------------------------------------------------
// Value kinds
// ---------------------------------------------------------------------------

/// How a control's data bytes are read. The fixed-width kinds are what ControlValue carries; the others
/// (graphic equalizer, modes, GUID) have their own variable layouts.
enum class AudioValueKind : uint8_t {
    kInputPlug,          ///< Selector: the input fb-plug number (1 byte; FF in a STATUS request)
    kBoolean,            ///< 70 true, 60 false, FF invalid (1 byte)
    kVolume,             ///< 1/256 dB, int16; 7FFF invalid, 8000 -infinity (volume, mixer, MaxAmpl, threshold)
    kBalance,            ///< LR / FR balance: 1/256 dB attenuation of one side, int16; 7FFF invalid
    kTone,               ///< Bass, mid, treble, equalizer gain: 0.25 dB steps, int8; 7F invalid
    kDelay,              ///< 1/64 ms, uint16; FFFF invalid (§10.3.10)
    kPercent,            ///< 0..254 percent, uint8; FF invalid (spaciousness, reverb level, feedback)
    kReverbType,         ///< 0..7 predefined, up to 254, FF invalid (§10.5.2)
    kSeconds,            ///< 1/256 s, uint16; FFFF invalid (reverb time, early time)
    kHertz,              ///< 1/256 Hz, uint16; FFFF invalid (chorus rate)
    kMilliseconds,       ///< 1/256 ms, uint16; FFFF invalid (chorus depth, attack, release)
    kRatio,              ///< 1/256 dB/dB, uint16; FFFF invalid (compression ratio)
    kScale,              ///< 0..254/256, uint8; FF invalid (MPEG and AC-3 scaling)
    kLowHighScale,       ///< MPEG high/low scaling: LowScale, HighScale (§10.6.4.2.6)
    kHighLowScale,       ///< AC-3 high/low scaling: HighScale, LowScale (§10.6.4.3.5)
    kRaw8,               ///< One byte the spec describes by name only (status controls, multilingual channel)
    kGraphicEqualizer,   ///< Variable: BandsPresent (4), ExtraBandsPresent (4), gains (§10.3.8)
    kProcessingMode,     ///< Variable: Size_of_modes (1) then that many mode bytes (§10.4.2.2)
    kCodecMode,          ///< Variable: the mode bytes (§10.6.3)
    kGuid,               ///< Variable: defined by the owner of the generic block's GUID (§8.4.2)
};

[[nodiscard]] constexpr std::optional<uint8_t> FixedWidth(AudioValueKind kind) noexcept {
    switch (kind) {
        case AudioValueKind::kInputPlug:
        case AudioValueKind::kBoolean:
        case AudioValueKind::kTone:
        case AudioValueKind::kPercent:
        case AudioValueKind::kReverbType:
        case AudioValueKind::kScale:
        case AudioValueKind::kRaw8:
            return 1;
        case AudioValueKind::kVolume:
        case AudioValueKind::kBalance:
        case AudioValueKind::kDelay:
        case AudioValueKind::kSeconds:
        case AudioValueKind::kHertz:
        case AudioValueKind::kMilliseconds:
        case AudioValueKind::kRatio:
        case AudioValueKind::kLowHighScale:
        case AudioValueKind::kHighLowScale:
            return 2;
        case AudioValueKind::kGraphicEqualizer:
        case AudioValueKind::kProcessingMode:
        case AudioValueKind::kCodecMode:
        case AudioValueKind::kGuid:
            return std::nullopt;
    }
    return std::nullopt;
}

struct ControlSpec {
    ControlId id;
    std::string_view name;  ///< The spec's identifier for the control_selector (Table A.4)
    AudioValueKind kind;
};

inline constexpr std::array kControlCatalogue{
    ControlSpec{controls::kSelectorControl, "SELECTOR_CONTROL", AudioValueKind::kInputPlug},
    ControlSpec{controls::kMuteControl, "MUTE_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kVolumeControl, "VOLUME_CONTROL", AudioValueKind::kVolume},
    ControlSpec{controls::kLrBalanceControl, "LR_BALANCE_CONTROL", AudioValueKind::kBalance},
    ControlSpec{controls::kFrBalanceControl, "FR_BALANCE_CONTROL", AudioValueKind::kBalance},
    ControlSpec{controls::kBassControl, "BASS_CONTROL", AudioValueKind::kTone},
    ControlSpec{controls::kMidControl, "MID_CONTROL", AudioValueKind::kTone},
    ControlSpec{controls::kTrebleControl, "TREBLE_CONTROL", AudioValueKind::kTone},
    ControlSpec{controls::kGraphicEqualizerControl, "GEQ_CONTROL", AudioValueKind::kGraphicEqualizer},
    ControlSpec{controls::kAutomaticGainControl, "AGC_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kDelayControl, "DELAY_CONTROL", AudioValueKind::kDelay},
    ControlSpec{controls::kBassBoostControl, "BASSBOOST_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kLoudnessControl, "LOUDNESS_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kEnableProcessingControl, "ENABLE_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kProcessingModeControl, "MODE_CONTROL", AudioValueKind::kProcessingMode},
    ControlSpec{controls::kMixerControl, "MIXER_CONTROL", AudioValueKind::kVolume},
    ControlSpec{controls::kGuidControl, "GUID_CONTROL", AudioValueKind::kGuid},
    ControlSpec{controls::kSpaciousnessControl, "SPACIOUSNESS_CONTROL", AudioValueKind::kPercent},
    ControlSpec{controls::kReverbTypeControl, "REVERBTYPE_CONTROL", AudioValueKind::kReverbType},
    ControlSpec{controls::kReverbLevelControl, "REVERBLEVEL_CONTROL", AudioValueKind::kPercent},
    ControlSpec{controls::kReverbTimeControl, "REVERBTIME_CONTROL", AudioValueKind::kSeconds},
    ControlSpec{controls::kReverbEarlyTimeControl, "REVERBEARLYTIME_CONTROL", AudioValueKind::kSeconds},
    ControlSpec{controls::kReverbDelayFeedbackControl, "REVERBDELAY_CONTROL", AudioValueKind::kPercent},
    ControlSpec{controls::kChorusRateControl, "CHORUSRATE_CONTROL", AudioValueKind::kHertz},
    ControlSpec{controls::kChorusDepthControl, "CHORUSDEPTH_CONTROL", AudioValueKind::kMilliseconds},
    ControlSpec{controls::kCompressionRatioControl, "COMPRESSION_RATIO_CONTROL", AudioValueKind::kRatio},
    ControlSpec{controls::kMaxAmplControl, "MAXAMPL_CONTROL", AudioValueKind::kVolume},
    ControlSpec{controls::kThresholdControl, "THRESHOLD_CONTROL", AudioValueKind::kVolume},
    ControlSpec{controls::kAttackTimeControl, "ATTACKTIME_CONTROL", AudioValueKind::kMilliseconds},
    ControlSpec{controls::kReleaseTimeControl, "RELEASETIME_CONTROL", AudioValueKind::kMilliseconds},
    ControlSpec{controls::kEnableCodecControl, "ENABLE_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kCodecModeControl, "MODE_CONTROL", AudioValueKind::kCodecMode},
    ControlSpec{controls::kMpegDualChannelControl, "MPEG_DUAL_CHANNEL_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kMpegSecondStereoControl, "MPEG_SECOND_STEREO_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kMpegMultilingualControl, "MPEG_MULTILINGUAL_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kMpegDynamicRangeControl, "MPEG_DYN_RANGE_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kMpegScaleControl, "MPEG_SCALE_CONTROL", AudioValueKind::kScale},
    ControlSpec{controls::kMpegHighLowScalingControl, "HL_SCALING_CONTROL", AudioValueKind::kLowHighScale},
    ControlSpec{controls::kAc3DynamicRangeControl, "AC3_DYNAMIC_RANGE_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kAc3ScaleControl, "AC3_SCALE_CONTROL", AudioValueKind::kScale},
    ControlSpec{controls::kAc3HighLowScalingControl, "AC3_HL_SCALING_CONTROL", AudioValueKind::kHighLowScale},
    ControlSpec{controls::kAc3DualMonoControl, "AC3_DUAL_MONO_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3DolbySurroundControl, "AC3_DOLBY_SURROUND_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kAc3FrameErrorStatusControl, "AC3_FRAME_ERROR_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3SampleRateStatusControl, "AC3_SAMPLE_RATE_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3DataRateStatusControl, "AC3_DATA_RATE_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3LfeOnStatusControl, "AC3_LFE_ON_STATUS_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kAc3ModStatusControl, "AC3_MOD_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3BsidStatusControl, "AC3_BSID_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3BsmodStatusControl, "AC3_BSMOD_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3CmixlevStatusControl, "AC3_CMIXLEV_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3SmixlevStatusControl, "AC3_SMIXLEV_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3DsurStatusControl, "AC3_DSUR_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3CpyrtStatusControl, "AC3_CPYRT_STATUS_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kAc3OrgnlStatusControl, "AC3_ORGNL_STATUS_CONTROL", AudioValueKind::kBoolean},
    ControlSpec{controls::kAc3DialnormStatusControl, "AC3_DIALNORM_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3Dialnorm2StatusControl, "AC3_DIALNORM2_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3MixlevStatusControl, "AC3_MIXLEV_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3Mixlev2StatusControl, "AC3_MIXLEV2_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3RoomtypeStatusControl, "AC3_ROOMTYPE_STATUS_CONTROL", AudioValueKind::kRaw8},
    ControlSpec{controls::kAc3Roomtype2StatusControl, "AC3_ROOMTYPE2_STATUS_CONTROL", AudioValueKind::kRaw8},
};

/// The catalogue entry for a control_selector in a block, or nullptr for a value Table A.4 does not
/// define. `subType` is the block's ProcessingType or CodecType (kAnySubType for Selector and Feature).
/// An entry whose sub-type is kAnySubType applies to every sub-type of its block.
[[nodiscard]] constexpr const ControlSpec* FindControl(FunctionBlockType block, uint8_t subType,
                                                       uint8_t selector) noexcept {
    for (const auto& spec : kControlCatalogue) {
        if (spec.id.block != block || spec.id.selector != selector) continue;
        if (spec.id.subType == kAnySubType || spec.id.subType == subType) return &spec;
    }
    return nullptr;
}
[[nodiscard]] constexpr const ControlSpec* FindControl(const ControlId& id) noexcept {
    return FindControl(id.block, id.subType, id.selector);
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

/// A fixed-width control value: kind, width and big-endian bytes, built by a named factory. The
/// "invalid" encodings are named, because a STATUS reply carries them and a CONTROL command must not.
struct ControlValue {
    AudioValueKind kind{AudioValueKind::kRaw8};
    uint8_t width{1};
    std::array<uint8_t, 2> bytes{};

    [[nodiscard]] constexpr std::span<const uint8_t> Bytes() const noexcept { return {bytes.data(), width}; }
    [[nodiscard]] constexpr uint16_t Raw16() const noexcept {
        return width == 2 ? static_cast<uint16_t>((bytes[0] << 8) | bytes[1]) : bytes[0];
    }

    [[nodiscard]] static constexpr ControlValue OfByte(AudioValueKind kind, uint8_t value) noexcept {
        return ControlValue{kind, 1, {value, 0}};
    }
    [[nodiscard]] static constexpr ControlValue OfWord(AudioValueKind kind, uint16_t value) noexcept {
        return ControlValue{kind, 2, {HighByte(value), LowByte(value)}};
    }

    // --- one byte ------------------------------------------------------------------------------
    [[nodiscard]] static constexpr ControlValue InputPlug(uint8_t plug) noexcept {
        return OfByte(AudioValueKind::kInputPlug, plug);
    }
    [[nodiscard]] static constexpr ControlValue Boolean(bool on) noexcept {
        return OfByte(AudioValueKind::kBoolean, on ? kBooleanTrue : kBooleanFalse);
    }
    /// Quarter-dB steps, -32.00 dB (0x80) to +31.50 dB (0x7E) (Tables 10.8-10.10, 10.15).
    [[nodiscard]] static constexpr ControlValue Tone(int8_t quarterDb) noexcept {
        return OfByte(AudioValueKind::kTone, static_cast<uint8_t>(quarterDb));
    }
    [[nodiscard]] static constexpr ControlValue Percent(uint8_t percent) noexcept {
        return OfByte(AudioValueKind::kPercent, percent);
    }
    [[nodiscard]] static constexpr ControlValue ReverbType(uint8_t type) noexcept {
        return OfByte(AudioValueKind::kReverbType, type);
    }
    [[nodiscard]] static constexpr ControlValue Scale(uint8_t scale) noexcept {
        return OfByte(AudioValueKind::kScale, scale);
    }
    [[nodiscard]] static constexpr ControlValue Raw8(uint8_t value) noexcept {
        return OfByte(AudioValueKind::kRaw8, value);
    }
    // --- two bytes -----------------------------------------------------------------------------
    [[nodiscard]] static constexpr ControlValue Volume(AvcVolume volume) noexcept {
        return OfWord(AudioValueKind::kVolume, static_cast<uint16_t>(volume.Raw()));
    }
    /// LR / FR balance in 1/256 dB (Tables 10.6, 10.7): positive attenuates the right (front) group, negative the left (rear).
    [[nodiscard]] static constexpr ControlValue Balance(int16_t value256Db) noexcept {
        return OfWord(AudioValueKind::kBalance, static_cast<uint16_t>(value256Db));
    }
    [[nodiscard]] static constexpr ControlValue Delay(uint16_t sixtyFourthsOfMs) noexcept {
        return OfWord(AudioValueKind::kDelay, sixtyFourthsOfMs);
    }
    [[nodiscard]] static constexpr ControlValue Seconds(uint16_t value256) noexcept {
        return OfWord(AudioValueKind::kSeconds, value256);
    }
    [[nodiscard]] static constexpr ControlValue Hertz(uint16_t value256) noexcept {
        return OfWord(AudioValueKind::kHertz, value256);
    }
    [[nodiscard]] static constexpr ControlValue Milliseconds(uint16_t value256) noexcept {
        return OfWord(AudioValueKind::kMilliseconds, value256);
    }
    [[nodiscard]] static constexpr ControlValue Ratio(uint16_t value256) noexcept {
        return OfWord(AudioValueKind::kRatio, value256);
    }
    [[nodiscard]] static constexpr ControlValue LowHighScale(uint8_t low, uint8_t high) noexcept {
        return ControlValue{AudioValueKind::kLowHighScale, 2, {low, high}};
    }
    [[nodiscard]] static constexpr ControlValue HighLowScale(uint8_t high, uint8_t low) noexcept {
        return ControlValue{AudioValueKind::kHighLowScale, 2, {high, low}};
    }

    // --- Named views: nullopt for the invalid value ------------------------------------------------
    /// true / false; nullopt for FF (invalid) or any other byte.
    [[nodiscard]] constexpr std::optional<bool> AsBoolean() const noexcept {
        if (kind != AudioValueKind::kBoolean) return std::nullopt;
        if (bytes[0] == kBooleanTrue) return true;
        if (bytes[0] == kBooleanFalse) return false;
        return std::nullopt;
    }
    /// Volume, mixer setting, MaxAmpl or threshold; nullopt for 7FFF (invalid).
    [[nodiscard]] constexpr std::optional<AvcVolume> AsVolume() const noexcept {
        if (kind != AudioValueKind::kVolume) return std::nullopt;
        const auto volume = AvcVolume::FromRaw(static_cast<int16_t>(Raw16()));
        if (!volume.IsValid()) return std::nullopt;
        return volume;
    }
    /// Quarter-dB tone gain; nullopt for 7F (invalid).
    [[nodiscard]] constexpr std::optional<int8_t> AsTone() const noexcept {
        if (kind != AudioValueKind::kTone || bytes[0] == kToneInvalid) return std::nullopt;
        return static_cast<int8_t>(bytes[0]);
    }

    static constexpr uint8_t kToneInvalid = 0x7F;  ///< Tables 10.8-10.10, 10.15
    static constexpr uint8_t kByteInvalid = 0xFF;  ///< Percent, reverb type, scale, boolean
    static constexpr uint16_t kWordInvalid = 0xFFFF;   ///< Delay, seconds, hertz, milliseconds, ratio
    static constexpr uint16_t kSignedWordInvalid = 0x7FFF;  ///< Volume, balance, steps

    friend constexpr bool operator==(const ControlValue&, const ControlValue&) noexcept = default;
};

/// MOVE and DELTA carry a signed step count rather than a value (Audio Table 10.3): 7FFF invalid,
/// 8000 reserved.
struct Steps {
    int16_t count{0};
    [[nodiscard]] static constexpr Steps Of(int16_t count) noexcept { return Steps{count}; }
    [[nodiscard]] constexpr uint16_t Raw() const noexcept { return static_cast<uint16_t>(count); }
};

} // namespace ASFW::AVC::Cmd
