// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspTypes.hpp - Wire types exclusive to the Saffire Pro 24 DSP
// Reference: snd-firewire-ctl-services/protocols/dice/src/focusrite/spro24dsp.rs

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::DICE::Focusrite {

// ============================================================================
// DSP Coefficient Layout
// ============================================================================

/// Size of one DSP coefficient block (in bytes)
constexpr size_t kCoefBlockSize = 0x88;

/// Number of coefficient blocks
constexpr size_t kCoefBlockCount = 8;

/// Block indices for DSP effects
namespace CoefBlock {
    constexpr size_t kCompressor = 2;
    constexpr size_t kEqualizer  = 2;
    constexpr size_t kReverb     = 3;
}

// ============================================================================
// DSP Effect States
// ============================================================================

/// Compressor state (2-channel)
struct CompressorState {
    std::array<float, 2> output{};     ///< Output volume (0.0 to 64.0)
    std::array<float, 2> threshold{};  ///< Threshold (-1.25 to 0.0)
    std::array<float, 2> ratio{};      ///< Ratio (0.03125 to 0.5)
    std::array<float, 2> attack{};     ///< Attack (-0.9375 to -1.0)
    std::array<float, 2> release{};    ///< Release (0.9375 to 1.0)

    /// Parse from wire format (2 × kCoefBlockSize bytes)
    static CompressorState Deserialize(const uint8_t* data);

    /// Serialize to wire format
    void Serialize(uint8_t* data) const;
};

/// Reverb state
struct ReverbState {
    float size{0.0f};       ///< Room size (0.0 to 1.0)
    float air{0.0f};        ///< Air/damping (0.0 to 1.0)
    bool enabled{false};    ///< Reverb enabled
    float preFilter{0.0f};  ///< Pre-filter value (-1.0 to 1.0)

    /// Parse from wire format (kCoefBlockSize bytes)
    static ReverbState Deserialize(const uint8_t* data);

    /// Serialize to wire format
    void Serialize(uint8_t* data) const;
};

/// Channel strip general parameters
struct EffectGeneralParams {
    std::array<bool, 2> eqAfterComp{};   ///< EQ after compressor
    std::array<bool, 2> compEnable{};    ///< Compressor enabled
    std::array<bool, 2> eqEnable{};      ///< Equalizer enabled

    /// Parse from wire format (4 bytes)
    static EffectGeneralParams Deserialize(const uint8_t* data);

    /// Serialize to wire format
    void Serialize(uint8_t* data) const;
};

} // namespace ASFW::Audio::DICE::Focusrite
