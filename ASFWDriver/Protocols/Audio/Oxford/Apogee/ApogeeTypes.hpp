// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeTypes.hpp - Apogee Duet FireWire protocol types
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs

#pragma once

#include <cstdint>
#include <array>
#include <cstddef>

namespace ASFW::Audio::Oxford::Apogee {

// ============================================================================
// Knob State
// ============================================================================

enum class KnobTarget : uint8_t {
    OutputPair0 = 0,
    InputPair0  = 1,
    InputPair1  = 2,
};

struct KnobState {
    bool outputMute{false};
    KnobTarget target{KnobTarget::OutputPair0};
    uint8_t outputVolume{0};      // 0-64
    std::array<uint8_t, 2> inputGains{}; // 10-75

    static constexpr uint8_t kOutputVolMin = 0;
    static constexpr uint8_t kOutputVolMax = 64;
    static constexpr uint8_t kInputGainMin = 10;
    static constexpr uint8_t kInputGainMax = 75;
};

// ============================================================================
// Output Parameters
// ============================================================================

enum class OutputSource : uint8_t {
    StreamInputPair0 = 0, // From FireWire stream
    MixerOutputPair0 = 1, // From Hardware Mixer
};

enum class OutputNominalLevel : uint8_t {
    Instrument = 0, // +4 dBu (Fixed)
    Consumer   = 1, // -10 dBV (Variable)
};

enum class OutputMuteMode : uint8_t {
    Never   = 0,
    Normal  = 1, // Mute on push, unmute on release
    Swapped = 2, // Mute on release, unmute on push
};

struct OutputParams {
    bool mute{false};
    uint8_t volume{0}; // 0-64
    OutputSource source{OutputSource::StreamInputPair0};
    OutputNominalLevel nominalLevel{OutputNominalLevel::Instrument};
    OutputMuteMode lineMuteMode{OutputMuteMode::Never};
    OutputMuteMode hpMuteMode{OutputMuteMode::Never};

    static constexpr uint8_t kVolumeMin = 0;
    static constexpr uint8_t kVolumeMax = 64;
};

// ============================================================================
// Input Parameters
// ============================================================================

enum class InputSource : uint8_t {
    Xlr   = 0,
    Phone = 1, // Inst
};

enum class InputXlrNominalLevel : uint8_t {
    Microphone   = 0, // Variable gain 10-75dB
    Professional = 1, // +4 dBu Fixed
    Consumer     = 2, // -10 dBV Fixed
};

struct InputParams {
    std::array<uint8_t, 2> gains{};              // 10-75
    std::array<bool, 2> polarities{};            // Phase invert
    std::array<InputXlrNominalLevel, 2> xlrNominalLevels{};
    std::array<bool, 2> phantomPowerings{};      // +48V
    std::array<InputSource, 2> sources{};
    bool clickless{false};

    static constexpr uint8_t kGainMin = 10;
    static constexpr uint8_t kGainMax = 75;
};

// ============================================================================
// Mixer Parameters
// ============================================================================

struct MixerCoefficients {
    std::array<uint16_t, 2> analogInputs{}; // Src 0, 1
    std::array<uint16_t, 2> streamInputs{}; // Src 2, 3
};

struct MixerParams {
    std::array<MixerCoefficients, 2> outputs{}; // Dst 0, 1

    static constexpr uint16_t kGainMin = 0;
    static constexpr uint16_t kGainMax = 0x3fff;
};

// ============================================================================
// Display Parameters
// ============================================================================

enum class DisplayTarget : uint8_t {
    Output = 0,
    Input  = 1,
};

enum class DisplayMode : uint8_t {
    Independent = 0,
    FollowingToKnobTarget = 1,
};

enum class DisplayOverhold : uint8_t {
    Infinite = 0,
    TwoSeconds = 1,
};

struct DisplayParams {
    DisplayTarget target{DisplayTarget::Output};
    DisplayMode mode{DisplayMode::Independent};
    DisplayOverhold overhold{DisplayOverhold::Infinite};
};

// ============================================================================
// Meter State
// ============================================================================

struct InputMeterState {
    std::array<int32_t, 2> levels{};

    static constexpr int32_t kMin = 0;
    static constexpr int32_t kMax = 0x7FFFFFFF; // i32::MAX
    static constexpr int32_t kStep = 0x100;
};

struct MixerMeterState {
    std::array<int32_t, 2> streamInputs{};
    std::array<int32_t, 2> mixerOutputs{};
};

} // namespace ASFW::Audio::Oxford::Apogee
