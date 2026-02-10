// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SaffireproCommon.hpp - Common definitions for Focusrite Saffire Pro family
// Reference: snd-firewire-ctl-services/protocols/dice/src/focusrite.rs

#pragma once

#include "../Core/DICETypes.hpp"
#include <cstdint>
#include <array>

namespace ASFW::Audio::DICE::Focusrite {

// ============================================================================
// Saffire Pro Application Section Offsets
// ============================================================================

/// Common offsets in TCAT application section for Saffire Pro family
namespace Offsets {
    constexpr uint32_t kSwNotice        = 0x05ec;  ///< Software notice trigger
    constexpr uint32_t kOutputGroup     = 0x000c;  ///< Output group state
    constexpr uint32_t kInputParams     = 0x0058;  ///< Input parameters
    constexpr uint32_t kIoParams        = 0x0040;  ///< I/O configuration
    constexpr uint32_t kDspEnable       = 0x0070;  ///< DSP enable/disable (SPro24DSP)
    constexpr uint32_t kChStripFlags    = 0x0078;  ///< Channel strip flags (SPro24DSP)
    constexpr uint32_t kCoefBase        = 0x0190;  ///< DSP coefficient base (SPro24DSP)
    constexpr uint32_t kEffectGeneral   = 0x0078;  ///< Effect general params offset
}

// Convenience constants (same as Offsets namespace, for simpler access)
constexpr uint32_t kSwNoticeOffset      = Offsets::kSwNotice;
constexpr uint32_t kOutputGroupOffset   = Offsets::kOutputGroup;
constexpr uint32_t kInputOffset         = Offsets::kInputParams;
constexpr uint32_t kDspEnableOffset     = Offsets::kDspEnable;
constexpr uint32_t kCoefOffset          = Offsets::kCoefBase;
constexpr uint32_t kEffectGeneralOffset = Offsets::kEffectGeneral;

/// Size of output group state structure
constexpr size_t kOutputGroupStateSize = 0x50;

/// Size of input params structure
constexpr size_t kInputParamsSize = 8;

// ============================================================================
// Software Notice Types
// ============================================================================

/// Software notice values to commit parameter changes
enum class SwNotice : uint32_t {
    OutputSrc       = 0x01,
    DimMute         = 0x02,
    OutputPad       = 0x03,
    InputParams     = 0x04,
    ChStripFlags    = 0x05,
    CompCh0         = 0x06,
    CompCh1         = 0x07,
    MicTransformer0 = 0x08,
    EqOutputCh0     = 0x09,
    EqOutputCh1     = 0x0A,
    EqLowCh0        = 0x0C,
    EqLowCh1        = 0x0D,
    EqLowMidCh0     = 0x0F,
    EqLowMidCh1     = 0x10,
    MicTransformer1 = 0x10,
    EqHighMidCh0    = 0x12,
    EqHighMidCh1    = 0x13,
    EqHighCh0       = 0x15,
    EqHighCh1       = 0x16,
    Reverb          = 0x1A,
    DspEnable       = 0x1C,
    // Aliases for cleaner naming
    DspChanged       = 0x1C,
    CoefChanged      = 0x06,  // Same as CompCh0 (covers coefficient changes)
    EffectChanged    = 0x05,  // Same as ChStripFlags
    InputChanged     = 0x04,  // Same as InputParams
    OutputGroupChanged = 0x02, // Same as DimMute
};

// ============================================================================
// Input Level Enums
// ============================================================================

/// Microphone input level setting
enum class MicInputLevel : uint8_t {
    Line,        ///< Gain range: -10dB to +36 dB
    Instrument,  ///< Gain range: +13 to +60 dB, headroom: +8dBu
};

/// Line input level setting
enum class LineInputLevel : uint8_t {
    Low,   ///< +16 dBu
    High,  ///< -10 dBV
};

// ============================================================================
// Input Parameters
// ============================================================================

/// Analog input parameters (common to Saffire Pro 14/24/24DSP)
struct InputParams {
    std::array<MicInputLevel, 2> micLevels{};
    std::array<LineInputLevel, 2> lineLevels{};
    
    /// Parse from big-endian wire format (8 bytes)
    static InputParams FromWire(const uint8_t* data);
    
    /// Serialize to big-endian wire format (8 bytes)
    void ToWire(uint8_t* data) const;
};

// ============================================================================
// Output Group State
// ============================================================================

/// Output group state (dim, mute, volumes)
struct OutputGroupState {
    bool muteEnabled{false};
    bool dimEnabled{false};
    std::array<int8_t, 6> volumes{};      ///< Per-output volume (0-127, inverted)
    std::array<bool, 6> volMutes{};       ///< Per-output mute
    std::array<bool, 6> volHwCtls{};      ///< Per-output hardware knob control
    std::array<bool, 6> muteHwCtls{};     ///< Per-output hardware mute button
    std::array<bool, 6> dimHwCtls{};      ///< Per-output hardware dim button
    int8_t hwKnobValue{0};                ///< Current hardware knob value
    
    /// Volume range
    static constexpr int8_t kVolMin = 0;
    static constexpr int8_t kVolMax = 127;
    
    /// Parse from big-endian wire format (0x50 bytes)
    static OutputGroupState FromWire(const uint8_t* data, size_t entryCount);
    
    /// Serialize to big-endian wire format (0x50 bytes)
    void ToWire(uint8_t* data) const;
};

// ============================================================================
// Optical Output Interface Mode
// ============================================================================

/// Optical output interface signal type
enum class OpticalOutIfaceMode : uint8_t {
    Adat,    ///< ADAT signal
    Spdif,   ///< S/PDIF signal
    AesEbu,  ///< AES/EBU signal (not all models)
};

// ============================================================================
// Notification Flags (Focusrite-specific)
// ============================================================================

namespace Notify {
    constexpr uint32_t kDimMuteChange = 0x00200000;
    constexpr uint32_t kVolChange     = 0x00400000;
}

} // namespace ASFW::Audio::DICE::Focusrite
