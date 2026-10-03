// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioControlBits.hpp - Which control each bit of a function block's Controls bitmap stands for.
//
// Source: TA 1999008 Table 8.3 (Feature), Tables 8.4-8.15 (Processing), Tables 8.16-8.22 (CODEC). Bits are
// interpreted from the most significant bit of the first byte: that is how the Phase 88 captures read
// (Feature controls `C0 00` = mute and volume). Table 8.3 does not explicitly settle
// Feature bit numbering; the MSB-first mixer matrix rule in 8.4.1 is separate.
// The Duet's Feature bitmaps (`00 03`) read the other way round; the bitmap is a hint either way, and
// STATUS decides which controls a device really has (DiscoveryReducer.cpp).
//
// Some bits name a control the control_selector table (Table A.4) has no value for: Chorus Level and the
// DTS CODEC's LFE_Select and Matrixed_Stereo_Select. They print by their spec name and cannot be sent.

#pragma once

#include "AudioSubunitDescriptor.hpp"
#include "../Commands/AudioControlTypes.hpp"
#include "../Core/AvcNames.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ASFW::Protocols::AVC::Descriptors {

struct ControlBit {
    uint8_t bit;
    ::ASFW::AVC::Cmd::ControlId control;  ///< selector 0: the control has no control_selector value (Table A.4)
    std::string_view name;                ///< the spec's name for it, used when `control` has no selector
};

namespace control_bits {

namespace C = ::ASFW::AVC::Cmd::controls;
using ::ASFW::AVC::Cmd::ControlId;
inline constexpr ControlId kNone{};

inline constexpr std::array kFeature{
    ControlBit{0, C::kMuteControl, ""},          ControlBit{1, C::kVolumeControl, ""},
    ControlBit{2, C::kLrBalanceControl, ""},     ControlBit{3, C::kFrBalanceControl, ""},
    ControlBit{4, C::kBassControl, ""},          ControlBit{5, C::kMidControl, ""},
    ControlBit{6, C::kTrebleControl, ""},        ControlBit{7, C::kGraphicEqualizerControl, ""},
    ControlBit{8, C::kAutomaticGainControl, ""}, ControlBit{9, C::kDelayControl, ""},
    ControlBit{10, C::kBassBoostControl, ""},    ControlBit{11, C::kLoudnessControl, ""},
};
// Generic, Up/Down-mix and Dolby Pro Logic processing (Tables 8.5, 8.6, 8.8): Enable, Mode.
inline constexpr std::array kEnableAndMode{
    ControlBit{0, C::kEnableProcessingControl, ""},
    ControlBit{1, C::kProcessingModeControl, ""},
};
inline constexpr std::array kStereoExtender{  // Table 8.9
    ControlBit{0, C::kEnableProcessingControl, ""},
    ControlBit{1, C::kSpaciousnessControl, ""},
};
inline constexpr std::array kReverberation{  // Table 8.10: the bit order is not the selector order
    ControlBit{0, C::kEnableProcessingControl, ""}, ControlBit{1, C::kReverbTypeControl, ""},
    ControlBit{2, C::kReverbLevelControl, ""},      ControlBit{3, C::kReverbTimeControl, ""},
    ControlBit{4, C::kReverbDelayFeedbackControl, ""}, ControlBit{5, C::kReverbEarlyTimeControl, ""},
};
inline constexpr std::array kChorus{  // Table 8.12: Chorus Level (bit 1) has no control_selector in Table A.4
    ControlBit{0, C::kEnableProcessingControl, ""}, ControlBit{1, kNone, "CHORUSLEVEL_CONTROL"},
    ControlBit{2, C::kChorusRateControl, ""},       ControlBit{3, C::kChorusDepthControl, ""},
};
inline constexpr std::array kCompression{  // Table 8.15
    ControlBit{0, C::kEnableProcessingControl, ""}, ControlBit{1, C::kCompressionRatioControl, ""},
    ControlBit{2, C::kMaxAmplControl, ""},          ControlBit{3, C::kThresholdControl, ""},
    ControlBit{4, C::kAttackTimeControl, ""},       ControlBit{5, C::kReleaseTimeControl, ""},
};
inline constexpr std::array kCodec{  // Tables 8.18, 8.20, 8.22
    ControlBit{0, C::kEnableCodecControl, ""},
    ControlBit{1, C::kCodecModeControl, ""},
};
inline constexpr std::array kDts{  // Table 8.18: two bits that §10.6.4.1 and Table A.4 give no control for
    ControlBit{0, C::kEnableCodecControl, ""}, ControlBit{1, C::kCodecModeControl, ""},
    ControlBit{2, kNone, "LFE_Select"},        ControlBit{3, kNone, "Matrixed_Stereo_Select"},
};

} // namespace control_bits

/// The Controls bits a function block of this type defines, in bit order. Empty for a Selector block (it has
/// none, §8.2) and for a Mixer, whose bitmap is an input x output matrix and not a list (§8.4.1).
[[nodiscard]] constexpr std::span<const ControlBit> ControlBitsOf(AudioFunctionBlockType block,
                                                                  uint8_t subType) noexcept {
    namespace B = control_bits;
    switch (block) {
        case AudioFunctionBlockType::kFeature:
            return B::kFeature;
        case AudioFunctionBlockType::kProcessing:
            switch (static_cast<::ASFW::AVC::Cmd::ProcessingType>(subType)) {
                case ::ASFW::AVC::Cmd::ProcessingType::kGeneric:
                case ::ASFW::AVC::Cmd::ProcessingType::kUpDownMix:
                case ::ASFW::AVC::Cmd::ProcessingType::kDolbyProLogic:
                    return B::kEnableAndMode;
                case ::ASFW::AVC::Cmd::ProcessingType::kStereoExtender3d:
                    return B::kStereoExtender;
                case ::ASFW::AVC::Cmd::ProcessingType::kReverberation:
                    return B::kReverberation;
                case ::ASFW::AVC::Cmd::ProcessingType::kChorus:
                    return B::kChorus;
                case ::ASFW::AVC::Cmd::ProcessingType::kDynamicRangeCompression:
                    return B::kCompression;
                case ::ASFW::AVC::Cmd::ProcessingType::kMixer:
                    return {};
            }
            return {};
        case AudioFunctionBlockType::kCodec:
            return subType == static_cast<uint8_t>(::ASFW::AVC::Cmd::CodecType::kDtsDecoder) ? std::span<const ControlBit>{B::kDts}
                                                                                           : std::span<const ControlBit>{B::kCodec};
        case AudioFunctionBlockType::kSelector:
            return {};
    }
    return {};
}

/// The Controls bits of a bitmap by the spec's names: "ENABLE_CONTROL(bit 0), MODE_CONTROL(bit 1)". A set
/// bit the spec does not define for this block prints as UNKNOWN(control_bit:N), so a bitmap read in the
/// wrong order shows up as unnamed bits and is not silently accepted.
[[nodiscard]] inline std::string DescribeControlBits(AudioFunctionBlockType block, uint8_t subType,
                                                    std::span<const uint8_t> bitmap) {
    const auto defined = ControlBitsOf(block, subType);
    std::string text;
    const auto add = [&text](const std::string& item) { text += (text.empty() ? "" : ", ") + item; };
    for (size_t bit = 0; bit < bitmap.size() * 8; ++bit) {
        if (!(bitmap[bit / 8] & (0x80u >> (bit % 8)))) continue;
        const ControlBit* known = nullptr;
        for (const auto& entry : defined) {
            if (entry.bit == bit) known = &entry;
        }
        const std::string where = "(bit " + std::to_string(bit) + ")";
        if (!known) {
            add(std::string(block == AudioFunctionBlockType::kFeature && bit >= 12 ? "RESERVED" : "UNKNOWN") + "(control_bit:" + std::to_string(bit) + ")");
        } else if (known->control.selector == 0) {
            add(std::string(known->name) + where);
        } else {
            const auto* spec = ::ASFW::AVC::Cmd::FindControl(known->control);
            add(std::string(spec ? spec->name : std::string_view{"?"}) + where);
        }
    }
    return text;
}

} // namespace ASFW::Protocols::AVC::Descriptors
